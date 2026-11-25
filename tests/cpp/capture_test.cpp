#include "ttkl/capture.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "ttkl/compilation_ctx.hpp"

namespace tt::lint {
namespace {

constexpr std::string_view kCompiler{"/opt/tenstorrent/sfpi/compiler/bin/riscv-tt-elf-g++"};
constexpr std::string_view kSource{"/repo/tt_metal/hw/firmware/src/tt-1xx/trisck.cc"};

// The shape of the lines jit_build/build.cpp logs for one compute kernel compile.
std::string ComputeLine(std::string_view role_define, int trisc, std::string_view full_name) {
    const std::string object{
        "/cache/kernels/" + std::string{full_name} + "trisc" + std::to_string(trisc) + "/trisck.o"};
    return "2026-11-18 12:00:00.000 | info | BuildKernels | g++ compile cmd: " + std::string{kCompiler} +
           " -mcpu=tt-wh-tensix -D" + std::string{role_define} + " -DCOMPILE_FOR_TRISC=" + std::to_string(trisc) +
           R"( -DKERNEL_BUILD -DFULL_KERNEL_NAME=")" + std::string{full_name} + "\" -c -o " + object + " " +
           std::string{kSource} + " (build.cpp:877)\n";
}

std::string DataMovementLine() {
    return "2026-11-18 12:00:00.000 | info | BuildKernels | g++ compile cmd: " + std::string{kCompiler} +
           " -mcpu=tt-wh -DCOMPILE_FOR_BRISC -DKERNEL_BUILD -c -o /cache/kernels/reader/1/brisc/brisck.o "
           "/repo/tt_metal/hw/firmware/src/tt-1xx/brisck.cc (build.cpp:877)\n";
}

// A log file that lives for the duration of one test.
class TemporaryLog {
    llvm::SmallString<128> path_;

public:
    explicit TemporaryLog(std::string_view contents) {
        int descriptor{-1};
        EXPECT_FALSE(llvm::sys::fs::createTemporaryFile("ttkl-capture", "log", descriptor, path_));
        llvm::raw_fd_ostream stream{descriptor, /*shouldClose=*/true};
        stream << contents;
    }

    TemporaryLog(const TemporaryLog&) = delete;
    TemporaryLog(TemporaryLog&&) = delete;
    TemporaryLog& operator=(const TemporaryLog&) = delete;
    TemporaryLog& operator=(TemporaryLog&&) = delete;
    ~TemporaryLog() {
        if (const std::error_code error{llvm::sys::fs::remove(path_)}) {
            ADD_FAILURE() << path_.str().str() << ": " << error.message();
        }
    }

    [[nodiscard]] llvm::StringRef path() const noexcept { return path_; }
};

TEST(CaptureTest, MatchesKernelNameVariantAndRole) {
    const KernelId kernel{"add_compute", "42", ThreadRole::kMath};

    EXPECT_TRUE(MatchesCaptureFilter(kernel, {}));
    EXPECT_TRUE(MatchesCaptureFilter(kernel, {"add_compute", std::nullopt}));
    EXPECT_TRUE(MatchesCaptureFilter(kernel, {"add_compute/42", ThreadRole::kMath}));
    EXPECT_TRUE(MatchesCaptureFilter(kernel, {"add_compute/42/", std::nullopt}));
    EXPECT_FALSE(MatchesCaptureFilter(kernel, {"add_compute/7", std::nullopt}));
    EXPECT_FALSE(MatchesCaptureFilter(kernel, {"add", std::nullopt}));
    EXPECT_FALSE(MatchesCaptureFilter(kernel, {"", ThreadRole::kPack}));
}

TEST(CaptureTest, ReadsFilteredComputeContexts) {
    const TemporaryLog log{
        ComputeLine("UCK_CHLKC_MATH", 1, "add_compute/42/") + ComputeLine("UCK_CHLKC_PACK", 2, "add_compute/42/") +
        ComputeLine("UCK_CHLKC_MATH", 1, "mul_compute/7/") + DataMovementLine()};

    const std::vector<Context> all{llvm::cantFail(ReadCaptureLog(log.path(), {}))};
    ASSERT_EQ(all.size(), 3U);

    const std::vector<Context> math{llvm::cantFail(ReadCaptureLog(log.path(), {"add_compute", ThreadRole::kMath}))};
    ASSERT_EQ(math.size(), 1U);
    EXPECT_EQ(math.front().kernel().name, "add_compute");
    EXPECT_EQ(math.front().kernel().variant, "42");
    EXPECT_EQ(math.front().kernel().role, ThreadRole::kMath);
    EXPECT_EQ(math.front().command().directory, "/cache/kernels/add_compute/42/trisc1");
}

TEST(CaptureTest, ReportsUnreadableLogsAndInconsistentCommands) {
    llvm::Expected<std::vector<Context>> missing{ReadCaptureLog("/nonexistent/jit.log", {})};
    ASSERT_FALSE(static_cast<bool>(missing));
    EXPECT_NE(llvm::toString(missing.takeError()).find("cannot open capture log"), std::string::npos);

    const TemporaryLog inconsistent{ComputeLine("UCK_CHLKC_PACK", 1, "add_compute/42/")};
    llvm::Expected<std::vector<Context>> contexts{ReadCaptureLog(inconsistent.path(), {})};
    ASSERT_FALSE(static_cast<bool>(contexts));
    EXPECT_NE(llvm::toString(contexts.takeError()).find("COMPILE_FOR_TRISC=1"), std::string::npos);
}

}  // namespace
}  // namespace tt::lint
