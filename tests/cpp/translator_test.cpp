#include "ttkl/translator.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"

#include "ttkl/compilation_ctx.hpp"
#include "ttkl/sfpi_toolchain.hpp"
#include "ttkl/target_profile.hpp"

namespace tt::lint {
namespace {

constexpr const char* kCompiler{"/opt/tenstorrent/sfpi/compiler/bin/riscv-tt-elf-g++"};
constexpr const char* kSfpiHeader{"/opt/tenstorrent/sfpi/include/sfpi.h"};
constexpr const char* kDirectory{"/cache/kernels/add_compute/abc123/trisc1"};
constexpr const char* kSource{"/repo/tt_metal/hw/firmware/src/tt-1xx/trisck.cc"};
constexpr std::string_view kHeaderContents{"class vInt;\n"};

constexpr auto kTestIncludes = std::to_array<std::string_view>({"compiler/include/c++", "compiler/include"});
constexpr auto kTestCpus = std::to_array<CpuMapping>({
    {"tt-wh-tensix", "riscv32-unknown-elf", "rv32im_zicsr_zifencei", "ilp32", kTestIncludes},
});
constexpr auto kTestMacros = std::to_array<MacroOverride>({{"__INT32_TYPE__", "long int"}, {"__INT32_C(c)", "c ## L"}});
constexpr auto kTestHeaders = std::to_array<HeaderDigest>({
    {"sfpi.h", "f6f2abd5eeecb15dd2a8b382f848244886bbd617e3b4c09d6a1ae00125b60ec6"},
});
constexpr auto kTestPatches = std::to_array<HeaderPatch>({
    {"sfpi.h", "class vInt;\n", "class vInt;\nclass vMag;\n", "test patch"},
});
constexpr ToolchainProfile kTestProfile{
    "test-profile", "0.0.0", "riscv-tt-elf-g++", kTestCpus, kTestMacros, kTestHeaders, kTestPatches};

constexpr auto kMismatchedPatches = std::to_array<HeaderPatch>({{"sfpi.h", "absent\n", "present\n", "test patch"}});
constexpr ToolchainProfile kMismatchedPatchProfile{
    "mismatched-patch", "0.0.0", "riscv-tt-elf-g++", kTestCpus, kTestMacros, kTestHeaders, kMismatchedPatches};

llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> MakeSfpiFileSystem(
    std::string_view header = kHeaderContents, bool with_includes = true) {
    auto file_system{llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>()};
    file_system->addFile(kCompiler, 0, llvm::MemoryBuffer::getMemBuffer(""));
    file_system->addFile(kSfpiHeader, 0, llvm::MemoryBuffer::getMemBufferCopy(header));
    if (with_includes) {
        file_system->addFile(
            "/opt/tenstorrent/sfpi/compiler/include/c++/vector", 0, llvm::MemoryBuffer::getMemBuffer(""));
    }
    return file_system;
}

SfpiToolchain OpenTestToolchain() {
    return llvm::cantFail(SfpiToolchain::OpenForCompiler(kCompiler, kTestProfile, *MakeSfpiFileSystem()));
}

std::optional<TranslationErrorKind> ErrorKind(llvm::Error error) {
    std::optional<TranslationErrorKind> kind;
    llvm::handleAllErrors(
        std::move(error),
        [&kind](const TranslationError& translation_error) { kind = translation_error.kind(); },
        [](const llvm::ErrorInfoBase& /*other*/) {});
    return kind;
}

Context MakeContext(std::vector<std::string> arguments) {
    return Context{
        KernelId{"add_compute", "abc123", ThreadRole::kMath},
        CompileCommand{kDirectory, kSource, std::move(arguments)}};
}

// Synthetic: assembled from the flags in tt_metal/jit_build/build.cpp and wh_hal.cpp, not captured from a real run.
std::vector<std::string> WormholeMathArguments() {
    return {
        kCompiler,
        "-O3",
        "-std=c++17",
        "-ftt-nttp",
        "-ftt-constinit",
        "-ftt-consteval",
        "-ftt-no-dyninit",
        "-flto=1",
        "-flto-partition=one",
        "-ffast-math",
        "-fno-finite-math-only",
        "-fsigned-zeros",
        "-fno-associative-math",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-use-cxa-atexit",
        "-MMD",
        "-Wall",
        "-Werror",
        "-Wno-error=deprecated-declarations",
        "-Wno-error=multistatement-macros",
        "--param=min-pagesize=0",
        "-mcpu=tt-wh-tensix",
        "-Winvalid-pch",
        "-Wno-error=invalid-pch",
        "-I.",
        "-I..",
        "-I/repo/",
        "-I/opt/tenstorrent/sfpi/include",
        "-include",
        "/cache/pch/1f2e/pch.h",
        "-DTENSIX_FIRMWARE",
        "-DUCK_CHLKC_MATH",
        "-DCOMPILE_FOR_TRISC=1",
        "-DARCH_WORMHOLE",
        "-DKERNEL_BUILD",
        R"(-DFULL_KERNEL_NAME="add_compute/abc123/")",
        "-DKERNEL_COMPILE_TIME_ARGS=1,2,3",
        "-c",
        "-o",
        "/cache/kernels/add_compute/abc123/trisc1/trisck.o",
        kSource,
        "-MF",
        "/cache/kernels/add_compute/abc123/trisc1/trisck.d",
    };
}

bool HasNote(const TranslationResult& result, NoteKind kind, std::string_view text) {
    for (const auto& [original, translated, reason, note_kind] : result.notes) {
        if (note_kind == kind && (original == text || translated == text)) {
            return true;
        }
    }
    return false;
}

TEST(TranslatorTest, TranslatesWormholeComputeCommand) {
    const SfpiToolchain toolchain{OpenTestToolchain()};
    const Context context{MakeContext(WormholeMathArguments())};

    const TranslationResult result{llvm::cantFail(TranslateSfpiCommand(context, toolchain))};

    const std::vector<std::string> expected{
        "clang++",
        "-O3",
        "-std=c++20",
        "-ffast-math",
        "-fno-finite-math-only",
        "-fsigned-zeros",
        "-fno-associative-math",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-use-cxa-atexit",
        "-Wall",
        "-Wno-error=deprecated-declarations",
        "-Wno-error=multistatement-macros",
        "--target=riscv32-unknown-elf",
        "-march=rv32im_zicsr_zifencei",
        "-mabi=ilp32",
        "-Winvalid-pch",
        "-Wno-error=invalid-pch",
        "-I.",
        "-I..",
        "-I/repo/",
        "-I/opt/tenstorrent/sfpi/include",
        "-include",
        "/cache/pch/1f2e/pch.h",
        "-DTENSIX_FIRMWARE",
        "-DUCK_CHLKC_MATH",
        "-DCOMPILE_FOR_TRISC=1",
        "-DARCH_WORMHOLE",
        "-DKERNEL_BUILD",
        R"(-DFULL_KERNEL_NAME="add_compute/abc123/")",
        "-DKERNEL_COMPILE_TIME_ARGS=1,2,3",
        "-fsyntax-only",
        kSource,
        "-nostdinc",
        "-isystem",
        "/opt/tenstorrent/sfpi/compiler/include/c++",
        "-isystem",
        "/opt/tenstorrent/sfpi/compiler/include",
        "-U__INT32_TYPE__",
        "-D__INT32_TYPE__=long int",
        "-U__INT32_C",
        "-D__INT32_C(c)=c ## L",
        "-Wno-unknown-warning-option",
        "-Wno-ignored-gch",
        "-Wno-missing-template-arg-list-after-template-kw",
        "-Wno-deprecated-enum-enum-conversion",
        "-ferror-limit=0",
    };
    EXPECT_EQ(result.command.arguments, expected);
    EXPECT_EQ(result.command.directory, kDirectory);
    EXPECT_EQ(result.command.source_file, kSource);
    EXPECT_EQ(context.command().arguments, WormholeMathArguments());

    EXPECT_TRUE(HasNote(result, NoteKind::kReplaced, kCompiler));
    EXPECT_TRUE(HasNote(result, NoteKind::kReplaced, "-mcpu=tt-wh-tensix"));
    EXPECT_TRUE(HasNote(result, NoteKind::kReplaced, "-std=c++17"));
    EXPECT_TRUE(HasNote(result, NoteKind::kReplaced, "-c"));
    EXPECT_TRUE(HasNote(result, NoteKind::kRemoved, "-o /cache/kernels/add_compute/abc123/trisc1/trisck.o"));
    EXPECT_TRUE(HasNote(result, NoteKind::kRemoved, "-MF /cache/kernels/add_compute/abc123/trisc1/trisck.d"));
    EXPECT_TRUE(HasNote(result, NoteKind::kRemoved, "-ftt-no-dyninit"));
    EXPECT_TRUE(HasNote(result, NoteKind::kRemoved, "-flto-partition=one"));
    EXPECT_TRUE(HasNote(result, NoteKind::kAdded, "-ferror-limit=0"));
}

TEST(TranslatorTest, KeepsCxx17WithoutSfpiBackports) {
    const SfpiToolchain toolchain{OpenTestToolchain()};
    const Context context{MakeContext({kCompiler, "-std=c++17", "-mcpu=tt-wh-tensix", "-c", kSource})};

    const TranslationResult result{llvm::cantFail(TranslateSfpiCommand(context, toolchain))};

    const std::vector<std::string>& arguments{result.command.arguments};
    EXPECT_NE(std::find(arguments.begin(), arguments.end(), "-std=c++17"), arguments.end());
    EXPECT_EQ(std::find(arguments.begin(), arguments.end(), "-std=c++20"), arguments.end());
    EXPECT_EQ(std::find(arguments.begin(), arguments.end(), "-Wno-deprecated-enum-enum-conversion"), arguments.end());
}

TEST(TranslatorTest, RemovesCcacheLauncherWithSettings) {
    const SfpiToolchain toolchain{OpenTestToolchain()};
    const Context context{
        MakeContext({"ccache", "sloppiness=pch_defines,time_macros", kCompiler, "-mcpu=tt-wh-tensix", "-c", kSource})};

    llvm::Expected<std::string_view> compiler{FindSfpiCompiler(context.command())};
    ASSERT_TRUE(static_cast<bool>(compiler)) << llvm::toString(compiler.takeError());
    EXPECT_EQ(*compiler, kCompiler);

    const TranslationResult result{llvm::cantFail(TranslateSfpiCommand(context, toolchain))};
    EXPECT_EQ(result.command.arguments.front(), "clang++");
    EXPECT_TRUE(HasNote(result, NoteKind::kRemoved, "ccache sloppiness=pch_defines,time_macros"));
}

TEST(TranslatorTest, ReplacesOfastWithItsClangSpelling) {
    const SfpiToolchain toolchain{OpenTestToolchain()};
    const Context context{MakeContext({kCompiler, "-Ofast", "-mcpu=tt-wh-tensix", "-c", kSource})};

    const TranslationResult result{llvm::cantFail(TranslateSfpiCommand(context, toolchain))};

    const std::vector<std::string> prefix(result.command.arguments.begin(), result.command.arguments.begin() + 3);
    EXPECT_EQ(prefix, (std::vector<std::string>{"clang++", "-O3", "-ffast-math"}));
}

TEST(TranslatorTest, RejectsCommandsOutsideTheVerifiedSet) {
    const SfpiToolchain toolchain{OpenTestToolchain()};
    const std::vector<std::pair<std::vector<std::string>, TranslationErrorKind>> cases{
        {{kCompiler, "-fno-strict-aliasing", "-mcpu=tt-wh-tensix", "-c", kSource},
         TranslationErrorKind::kUnsupportedOption},
        {{kCompiler, "-Wp,-DHIDDEN", "-mcpu=tt-wh-tensix", "-c", kSource}, TranslationErrorKind::kUnsupportedOption},
        {{kCompiler, "-std=c++14", "-mcpu=tt-wh-tensix", "-c", kSource}, TranslationErrorKind::kUnsupportedOption},
        {{kCompiler, "-mcpu=tt-bh-tensix", "-c", kSource}, TranslationErrorKind::kUnsupportedCpu},
        {{kCompiler, "-mcpu=tt-wh", "-c", kSource}, TranslationErrorKind::kUnsupportedCpu},
        {{kCompiler, "-c", kSource}, TranslationErrorKind::kMalformedCommand},
        {{kCompiler, "-mcpu=tt-wh-tensix", kSource}, TranslationErrorKind::kMalformedCommand},
        {{kCompiler, "-mcpu=tt-wh-tensix", "-c"}, TranslationErrorKind::kMalformedCommand},
        {{kCompiler, "-mcpu=tt-wh-tensix", "-c", kSource, "-o"}, TranslationErrorKind::kMalformedCommand},
        {{kCompiler, "-mcpu=tt-wh-tensix", "-DFILL=fill<1024,", "int32_t>", "-c", kSource},
         TranslationErrorKind::kMalformedCommand},
        {{kCompiler, "-mcpu=tt-wh-tensix", "-c", kSource, kSource}, TranslationErrorKind::kMalformedCommand},
        {{"/usr/bin/riscv-tt-elf-g++", "-mcpu=tt-wh-tensix", "-c", kSource},
         TranslationErrorKind::kUnsupportedCompiler},
        {{"ccache", "sloppiness=pch_defines"}, TranslationErrorKind::kMalformedCommand},
        {{}, TranslationErrorKind::kMalformedCommand},
    };
    for (const auto& [arguments, kind] : cases) {
        llvm::Expected<TranslationResult> result{TranslateSfpiCommand(MakeContext(arguments), toolchain)};
        ASSERT_FALSE(static_cast<bool>(result)) << "accepted: " << ::testing::PrintToString(arguments);
        EXPECT_EQ(ErrorKind(result.takeError()), kind) << ::testing::PrintToString(arguments);
    }
}

TEST(SfpiToolchainTest, AppliesPatchesInMemory) {
    const SfpiToolchain toolchain{OpenTestToolchain()};

    EXPECT_EQ(toolchain.root(), "/opt/tenstorrent/sfpi");
    ASSERT_EQ(toolchain.overlays().size(), 1U);
    EXPECT_EQ(toolchain.overlays().front().path, kSfpiHeader);
    EXPECT_EQ(toolchain.overlays().front().contents, "class vInt;\nclass vMag;\n");
}

TEST(SfpiToolchainTest, RejectsMismatchedInstallations) {
    const auto open_error = [](std::string_view compiler, const ToolchainProfile& profile, llvm::vfs::FileSystem& fs) {
        llvm::Expected<SfpiToolchain> toolchain{SfpiToolchain::OpenForCompiler(compiler, profile, fs)};
        return toolchain ? std::string{} : llvm::toString(toolchain.takeError());
    };

    EXPECT_NE(
        open_error(kCompiler, kTestProfile, *MakeSfpiFileSystem("class vMag;\n")).find("is not release 0.0.0"),
        std::string::npos);
    EXPECT_NE(
        open_error(kCompiler, kTestProfile, *MakeSfpiFileSystem(kHeaderContents, false))
            .find("missing directory 'compiler/include/c++'"),
        std::string::npos);
    EXPECT_NE(
        open_error(kCompiler, kMismatchedPatchProfile, *MakeSfpiFileSystem()).find("matched 0 times"),
        std::string::npos);
    EXPECT_NE(
        open_error("compiler/bin/riscv-tt-elf-g++", kTestProfile, *MakeSfpiFileSystem()).find("not absolute"),
        std::string::npos);
    EXPECT_NE(
        open_error("/opt/sfpi/bin/riscv-tt-elf-g++", kTestProfile, *MakeSfpiFileSystem())
            .find("is not in <sfpi root>/compiler/bin"),
        std::string::npos);
}

TEST(SfpiToolchainTest, WormholeProfilePinsSfpi7900) {
    const ToolchainProfile& profile{WormholeSfpiProfile()};

    EXPECT_EQ(profile.sfpi_version, "7.90.0");
    ASSERT_EQ(profile.cpus.size(), 1U);
    const CpuMapping& cpu{profile.cpus.front()};
    EXPECT_EQ(cpu.mcpu, "tt-wh-tensix");
    EXPECT_EQ(cpu.mabi, "ilp32");
    ASSERT_EQ(cpu.system_include_directories.size(), 6U);
    EXPECT_EQ(cpu.system_include_directories[1], "compiler/riscv-tt-elf/include/c++/15.1.0/riscv-tt-elf/wh-ilp32");
    EXPECT_EQ(profile.headers.size(), 10U);
    for (const HeaderPatch& patch : profile.patches) {
        bool pinned{false};
        for (const HeaderDigest& header : profile.headers) {
            pinned = pinned || header.file == patch.file;
        }
        EXPECT_TRUE(pinned) << patch.file;
    }
}

}  // namespace
}  // namespace tt::lint
