#include "ttkl/jit_capture.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "llvm/Support/Error.h"

#include "ttkl/compilation_ctx.hpp"

namespace tt::lint {
namespace {

constexpr const char* kCompiler{"/opt/tenstorrent/sfpi/compiler/bin/riscv-tt-elf-g++"};
constexpr const char* kSource{"/repo/tt_metal/hw/firmware/src/tt-1xx/trisck.cc"};
constexpr const char* kObject{"/cache/kernels/add_compute/42/trisc1/trisck.o"};

// The shape of the lines jit_build/build.cpp logs, including the tt-logger prefix and source location.
std::string LogLine(const std::string& command, const std::string& location = " (build.cpp:877)") {
    return "2026-11-18 12:00:00.000 | info     |    BuildKernels |     g++ compile cmd: " + command + location + "\n";
}

std::string MathCommandText() {
    return std::string{kCompiler} +
           " -O3 -mcpu=tt-wh-tensix -DUCK_CHLKC_MATH -DCOMPILE_FOR_TRISC=1 -DKERNEL_BUILD "
           R"(-DFULL_KERNEL_NAME="add_compute/42/" -c -o )" +
           kObject + " " + kSource + " -MF /cache/kernels/add_compute/42/trisc1/trisck.d";
}

CompileCommand MakeCommand(std::vector<std::string> arguments) {
    return CompileCommand{"/cache/kernels/k/42/trisc1", kSource, std::move(arguments)};
}

TEST(JitCaptureTest, ReadsCompileCommandsOnce) {
    const std::string log{
        "2026-11-18 12:00:00.000 | info     |          Always | unrelated (metal.cpp:1)\n" +
        LogLine(MathCommandText()) + LogLine(MathCommandText(), " \x1b[90m(build.cpp:877)\x1b[0m") +
        "2026-11-18 12:00:01.000 | info     |    BuildKernels |     g++ link cmd: cd /cache && g++ -o trisc1.elf "
        "(build.cpp:964)\n"};

    const std::vector<CompileCommand> commands{llvm::cantFail(ParseJitBuildLog(log))};

    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands.front().directory, "/cache/kernels/add_compute/42/trisc1");
    EXPECT_EQ(commands.front().source_file, kSource);
    EXPECT_EQ(commands.front().arguments.front(), kCompiler);
    EXPECT_EQ(commands.front().arguments.back(), "/cache/kernels/add_compute/42/trisc1/trisck.d");
}

TEST(JitCaptureTest, RejectsCompileLinesWithoutTheJitLayout) {
    llvm::Expected<std::vector<CompileCommand>> missing_output{
        ParseJitBuildLog(LogLine(std::string{kCompiler} + " -c " + kSource))};
    ASSERT_FALSE(static_cast<bool>(missing_output));
    EXPECT_NE(llvm::toString(missing_output.takeError()).find("log line 1"), std::string::npos);

    llvm::Expected<std::vector<CompileCommand>> relative_object{
        ParseJitBuildLog(LogLine(std::string{kCompiler} + " -c -o trisck.o " + kSource))};
    ASSERT_FALSE(static_cast<bool>(relative_object));
    EXPECT_NE(llvm::toString(relative_object.takeError()).find("not absolute"), std::string::npos);
}

TEST(JitCaptureTest, IdentifiesComputeKernelContext) {
    const std::vector<CompileCommand> commands{llvm::cantFail(ParseJitBuildLog(LogLine(MathCommandText())))};

    const std::optional<Context> context{llvm::cantFail(IdentifyComputeContext(commands.front()))};

    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(context->kernel().name, "add_compute");
    EXPECT_EQ(context->kernel().variant, "42");
    EXPECT_EQ(context->kernel().role, ThreadRole::kMath);
    EXPECT_EQ(context->command().arguments, commands.front().arguments);
}

TEST(JitCaptureTest, SkipsFirmwareAndDataMovementCommands) {
    const std::vector<std::vector<std::string>> skipped{
        {kCompiler, "-mcpu=tt-wh", "-DCOMPILE_FOR_BRISC", "-DKERNEL_BUILD", "-c", kSource},
        {kCompiler, "-mcpu=tt-wh-tensix", "-DUCK_CHLKC_MATH", "-DCOMPILE_FOR_TRISC=1", "-DFW_BUILD", "-c", kSource},
    };
    for (const std::vector<std::string>& arguments : skipped) {
        EXPECT_FALSE(llvm::cantFail(IdentifyComputeContext(MakeCommand(arguments))).has_value());
    }
}

TEST(JitCaptureTest, RejectsArgumentsSplitByTheLog) {
    llvm::Expected<std::vector<CompileCommand>> commands{
        ParseJitBuildLog(LogLine(std::string{kCompiler} + R"( -DCFG="a -Ifoo" -c -o )" + kObject + " " + kSource))};

    ASSERT_FALSE(static_cast<bool>(commands));
    EXPECT_NE(llvm::toString(commands.takeError()).find("unbalanced quote"), std::string::npos);
}

TEST(JitCaptureTest, KeepsTrailingArgumentsThatLookLikeALocation) {
    const std::vector<CompileCommand> commands{llvm::cantFail(
        ParseJitBuildLog(LogLine(std::string{kCompiler} + " -c -o " + kObject + " " + kSource + " -DLOC=(a:1)", "")))};

    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands.front().arguments.back(), "-DLOC=(a:1)");
}

TEST(JitCaptureTest, AppliesDefinesInCommandLineOrder) {
    const std::vector<std::string> arguments{
        kCompiler,
        "-DCOMPILE_FOR_TRISC=0",
        "-DUCK_CHLKC_PACK",
        "-DCOMPILE_FOR_TRISC=1",
        "-UUCK_CHLKC_PACK",
        "-D",
        "UCK_CHLKC_MATH",
        "-DKERNEL_BUILD",
        R"(-DFULL_KERNEL_NAME="k/42/")",
    };

    const std::optional<Context> context{llvm::cantFail(IdentifyComputeContext(MakeCommand(arguments)))};

    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(context->kernel().role, ThreadRole::kMath);
}

TEST(JitCaptureTest, RejectsInconsistentComputeDefines) {
    const std::vector<std::vector<std::string>> inconsistent{
        {kCompiler, "-DUCK_CHLKC_PACK", "-DCOMPILE_FOR_TRISC=1", "-DKERNEL_BUILD", R"(-DFULL_KERNEL_NAME="k/42/")"},
        {kCompiler,
         "-DUCK_CHLKC_MATH",
         "-DUCK_CHLKC_PACK",
         "-DCOMPILE_FOR_TRISC=1",
         "-DKERNEL_BUILD",
         R"(-DFULL_KERNEL_NAME="k/42/")"},
        {kCompiler, "-DCOMPILE_FOR_TRISC=3", "-DKERNEL_BUILD", R"(-DFULL_KERNEL_NAME="k/42/")"},
        {kCompiler, "-DUCK_CHLKC_MATH", "-DCOMPILE_FOR_TRISC=1", "-DKERNEL_BUILD"},
        {kCompiler, "-DUCK_CHLKC_MATH", "-DCOMPILE_FOR_TRISC=1", "-DKERNEL_BUILD", R"(-DFULL_KERNEL_NAME="k")"},
    };
    for (const std::vector<std::string>& arguments : inconsistent) {
        llvm::Expected<std::optional<Context>> context{IdentifyComputeContext(MakeCommand(arguments))};
        EXPECT_FALSE(static_cast<bool>(context)) << ::testing::PrintToString(arguments);
        llvm::consumeError(context.takeError());
    }
}

}  // namespace
}  // namespace tt::lint
