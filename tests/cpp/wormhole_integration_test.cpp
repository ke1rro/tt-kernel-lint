// Parses Wormhole compute kernels with the real SFPI 7.90.0 headers and the TT-Metal submodule.
//
// Needs TTKL_SFPI_ROOT pointing at an SFPI 7.90.0 installation (the directory holding compiler/ and include/); the
// tests are skipped otherwise. The capture is synthesized from tt_metal/jit_build/genfiles.cpp and build.cpp for the
// example add_compute kernel, so it checks the translator and the frontend, not a particular TT-Metal run.

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/ASTUnit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "ttkl/compilation_ctx.hpp"
#include "ttkl/frontend.hpp"
#include "ttkl/jit_capture.hpp"
#include "ttkl/sfpi_toolchain.hpp"
#include "ttkl/target_profile.hpp"
#include "ttkl/translator.hpp"

namespace tt::lint {
namespace {

constexpr std::string_view kSourceDirectory{TTKL_SOURCE_DIR};
constexpr std::string_view kKernelDirectory{"cache/5032861097226391347/kernels/add_compute/8174652301937721"};
constexpr std::string_view kPchDirectory{"cache/pch/1f2e3d4c5b6a7980"};

std::string Join(llvm::StringRef base, llvm::StringRef first, llvm::StringRef second = {}) {
    llvm::SmallString<256> path{base};
    llvm::sys::path::append(path, first);
    if (!second.empty()) {
        llvm::sys::path::append(path, second);
    }
    return std::string{path};
}

std::string ReadFile(const std::string& path) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer{llvm::MemoryBuffer::getFile(path)};
    return buffer ? std::string{(*buffer)->getBuffer()} : std::string{};
}

void WriteFile(const std::string& path, std::string_view contents) {
    ASSERT_FALSE(llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path)));
    std::error_code error;
    llvm::raw_fd_ostream stream{path, error};
    ASSERT_FALSE(error) << path << ": " << error.message();
    stream << contents;
}

void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
    for (std::size_t position{text.find(from)}; position != std::string::npos;
         position = text.find(from, position + to.size())) {
        text.replace(position, from.size(), to);
    }
}

// Collects error messages so that a failure shows what Clang rejected.
class ErrorCollector final : public clang::DiagnosticConsumer {
    std::vector<std::string> errors_;

public:
    void HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& info) override {
        DiagnosticConsumer::HandleDiagnostic(level, info);
        if (level >= clang::DiagnosticsEngine::Error) {
            llvm::SmallString<256> message;
            info.FormatDiagnostic(message);
            errors_.emplace_back(message);
        }
    }

    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }
};

// Number of calls to `callee_name` in the template instantiations of `caller_name`, i.e. the code kernel_main uses.
std::size_t CountCalls(clang::ASTContext& context, std::string_view caller_name, std::string_view callee_name) {
    using namespace clang::ast_matchers;
    const auto calls{match(
        callExpr(
            callee(functionDecl(hasName(std::string{callee_name}))),
            hasAncestor(functionDecl(hasName(std::string{caller_name}), isTemplateInstantiation()))),
        context)};
    return calls.size();
}

class WormholeIntegrationTest : public ::testing::Test {
protected:
    std::string sfpi_root_;
    std::string metal_root_;
    llvm::SmallString<256> capture_;

    void SetUp() override {
        const char* sfpi_root{std::getenv("TTKL_SFPI_ROOT")};
        if (sfpi_root == nullptr || *sfpi_root == '\0') {
            GTEST_SKIP() << "set TTKL_SFPI_ROOT to an SFPI 7.90.0 installation";
        }
        sfpi_root_ = sfpi_root;
        metal_root_ = Join(kSourceDirectory, "tt-metal");
        if (!llvm::sys::fs::exists(Join(metal_root_, "tt_metal/hw/firmware/src/tt-1xx/trisck.cc"))) {
            GTEST_SKIP() << "the tt-metal submodule is not checked out";
        }
        ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("ttkl-wormhole", capture_));
    }

    void TearDown() override {
        if (!capture_.empty()) {
            EXPECT_FALSE(llvm::sys::fs::remove_directories(capture_));
        }
    }

    [[nodiscard]] std::string Fixture(llvm::StringRef file) const {
        return Join(kSourceDirectory, "tests/fixtures/wormhole_add_compute", file);
    }

    // The files genfiles.cpp writes for a compute kernel, and the log build.cpp writes for its compiles.
    std::string WriteCapture() {
        const std::string kernel_directory{Join(capture_, kKernelDirectory)};
        const std::string kernel_source{Join(kSourceDirectory, "example_kernel/device/compute/add_compute.cpp")};
        WriteFile(Join(kernel_directory, "defines_generated.h"), "#pragma once\n");
        WriteFile(Join(kernel_directory, "chlkc_descriptors.h"), ReadFile(Fixture("chlkc_descriptors.h")));
        for (const auto& [file, define] : {
                 std::pair{"chlkc_unpack.cpp", "TRISC_UNPACK"},
                 std::pair{"chlkc_math.cpp", "TRISC_MATH"},
                 std::pair{"chlkc_pack.cpp", "TRISC_PACK"},
             }) {
            WriteFile(
                Join(kernel_directory, file),
                std::string{"#define "} + define + "\n#include \"defines_generated.h\"\nvoid kernel_main();\n" +
                    "#include \"" + kernel_source + "\"\n");
        }
        for (const char* target : {"trisc0", "trisc1", "trisc2"}) {
            EXPECT_FALSE(llvm::sys::fs::create_directories(Join(kernel_directory, target)));
        }
        const std::string pch_directory{Join(capture_, kPchDirectory)};
        WriteFile(Join(pch_directory, "pch.h"), ReadFile(Join(metal_root_, "tt_metal/hw/inc/internal/pch.h")));
        WriteFile(Join(pch_directory, "pch.h.gch"), "not a clang precompiled header");

        std::string log{ReadFile(Fixture("jit.log.in"))};
        ReplaceAll(log, "@TT_METAL_ROOT@", metal_root_);
        ReplaceAll(log, "@SFPI_ROOT@", sfpi_root_);
        ReplaceAll(log, "@CAPTURE@", capture_.str());
        return log;
    }

    [[nodiscard]] static std::optional<SfpiToolchain> OpenToolchain(std::string_view compiler) {
        llvm::Expected<SfpiToolchain> toolchain{
            SfpiToolchain::OpenForCompiler(compiler, WormholeSfpiProfile(), *llvm::vfs::getRealFileSystem())};
        if (!toolchain) {
            ADD_FAILURE() << llvm::toString(toolchain.takeError());
            return std::nullopt;
        }
        return std::move(*toolchain);
    }
};

TEST_F(WormholeIntegrationTest, ParsesEveryComputeRoleOfACapture) {
    const std::vector<CompileCommand> commands{llvm::cantFail(ParseJitBuildLog(WriteCapture()))};
    ASSERT_EQ(commands.size(), 4U) << "unpack, math, pack and one data movement compile";

    std::vector<Context> contexts;
    for (const CompileCommand& command : commands) {
        if (std::optional<Context> context{llvm::cantFail(IdentifyComputeContext(command))}) {
            contexts.push_back(std::move(*context));
        }
    }
    ASSERT_EQ(contexts.size(), 3U);
    const std::optional<SfpiToolchain> toolchain{
        OpenToolchain(llvm::cantFail(FindSfpiCompiler(contexts.front().command())))};
    ASSERT_TRUE(toolchain.has_value());
    EXPECT_EQ(toolchain->overlays().size(), 2U);

    for (const Context& context : contexts) {
        const std::string_view role{ThreadRoleName(context.kernel().role)};
        SCOPED_TRACE(std::string{role});
        EXPECT_EQ(context.kernel().name, "add_compute");
        EXPECT_EQ(context.kernel().variant, "8174652301937721");

        const TranslationResult translation{llvm::cantFail(TranslateSfpiCommand(context, *toolchain))};
        ErrorCollector diagnostics;
        llvm::Expected<std::unique_ptr<clang::ASTUnit>> ast{
            BuildAst(translation.command, toolchain->overlays(), &diagnostics)};
        ASSERT_TRUE(static_cast<bool>(ast)) << llvm::toString(ast.takeError());
        EXPECT_EQ(diagnostics.errors(), std::vector<std::string>{});

        clang::ASTContext& ast_context{(*ast)->getASTContext()};
        const std::size_t unpack_calls{CountCalls(ast_context, "add_tiles", "llk_unpack_AB")};
        const std::size_t math_calls{CountCalls(ast_context, "add_tiles", "llk_math_eltwise_binary")};
        switch (context.kernel().role) {
            case ThreadRole::kUnpack:
                EXPECT_EQ(unpack_calls, 1U);
                EXPECT_EQ(math_calls, 0U);
                break;
            case ThreadRole::kMath:
                EXPECT_EQ(unpack_calls, 0U);
                EXPECT_EQ(math_calls, 1U);
                break;
            case ThreadRole::kPack:
                EXPECT_EQ(unpack_calls, 0U);
                EXPECT_EQ(math_calls, 0U);
                break;
        }
    }
}

// Probes the properties that SFPI GCC fixes and Clang would otherwise get wrong: the newlib ilp32 type model and the
// vMag to vInt conversion that SFPI GCC resolves to the vInt(vMag) constructor.
TEST_F(WormholeIntegrationTest, MatchesSfpiGccTypeModelAndConversions) {
    const std::string probe{Join(capture_, "probe.cpp")};
    WriteFile(probe, R"(#include <climits>
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace ckernel { extern volatile std::uint32_t* instrn_buffer; }
#include <sfpi.h>

static_assert(sizeof(long) == 4 && sizeof(void*) == 4 && sizeof(long long) == 8);
static_assert(std::is_same_v<std::int32_t, long> && std::is_same_v<std::uint32_t, unsigned long>);
static_assert(std::is_same_v<std::int_least32_t, long> && std::is_same_v<std::uint_least32_t, unsigned long>);
static_assert(std::is_same_v<decltype(INT32_MAX), std::int32_t> && std::is_same_v<decltype(UINT32_MAX), std::uint32_t>);
static_assert(std::is_same_v<decltype(INT32_C(1)), std::int32_t> && std::is_same_v<decltype(UINT32_C(1)), std::uint32_t>);
static_assert(std::is_same_v<std::int_fast8_t, int> && std::is_same_v<std::int_fast16_t, int>);
static_assert(std::is_same_v<std::uint_fast8_t, unsigned> && std::is_same_v<std::uint_fast16_t, unsigned>);
static_assert(INT_FAST8_MAX == 0x7fffffff && UINT_FAST16_MAX == 0xffffffffU);
static_assert(std::is_same_v<std::size_t, unsigned> && std::is_same_v<std::intptr_t, int>);
static_assert(!std::is_signed_v<char> && __riscv_xlen == 32 && __riscv_xtttensixwh == 1);
static_assert(__cplusplus == 202002L, "SFPI GCC keeps 201703L, a known difference");

sfpi::vInt FromMagnitude(sfpi::vMag magnitude) { return magnitude; }
)");
    const std::string compiler{Join(sfpi_root_, "compiler/bin/riscv-tt-elf-g++")};
    const Context context{
        KernelId{"probe", "0", ThreadRole::kMath},
        CompileCommand{
            std::string{capture_},
            probe,
            {compiler,
             "-O3",
             "-std=c++17",
             "-ftt-nttp",
             "-mcpu=tt-wh-tensix",
             "-I" + Join(sfpi_root_, "include"),
             "-DARCH_WORMHOLE",
             "-DCOMPILE_FOR_TRISC=1",
             "-c",
             "-o",
             Join(capture_, "probe.o"),
             probe}}};
    const std::optional<SfpiToolchain> toolchain{OpenToolchain(compiler)};
    ASSERT_TRUE(toolchain.has_value());

    const TranslationResult translation{llvm::cantFail(TranslateSfpiCommand(context, *toolchain))};
    ErrorCollector diagnostics;
    llvm::Expected<std::unique_ptr<clang::ASTUnit>> ast{
        BuildAst(translation.command, toolchain->overlays(), &diagnostics)};
    ASSERT_TRUE(static_cast<bool>(ast)) << llvm::toString(ast.takeError());
    EXPECT_EQ(diagnostics.errors(), std::vector<std::string>{});

    using namespace clang::ast_matchers;
    const auto constructions{match(
        cxxConstructExpr(
            hasAncestor(functionDecl(hasName("FromMagnitude"))),
            hasDeclaration(cxxConstructorDecl(ofClass(hasName("::sfpi::vInt")), parameterCountIs(1)).bind("ctor"))),
        (*ast)->getASTContext())};
    std::vector<std::string> parameters;
    for (const BoundNodes& nodes : constructions) {
        const auto* constructor{nodes.getNodeAs<clang::CXXConstructorDecl>("ctor")};
        parameters.push_back(constructor->getParamDecl(0)->getType().getAsString());
    }
    const bool from_magnitude{std::any_of(parameters.begin(), parameters.end(), [](const std::string& parameter) {
        return parameter.find("vMag") != std::string::npos;
    })};
    EXPECT_TRUE(from_magnitude) << ::testing::PrintToString(parameters);
}

}  // namespace
}  // namespace tt::lint
