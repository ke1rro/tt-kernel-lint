#include "ttkl/check/init_operation_pairing.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringExtras.h"

namespace tt::lint {
namespace {

constexpr std::string_view kApiHeader{"/virtual/tt_metal/hw/inc/api/compute/api.h"};
constexpr std::string_view kKernelFile{"/virtual/kernels/kernel.cpp"};

// Stands in for the TT-Metal compute API: same namespace and names, declared under the compute API directory.
// copy_tile_init and copy_and_hold run an initializer or an operation themselves, as some real API functions do.
constexpr std::string_view kApiSource{R"(#pragma once
namespace ckernel {
inline void add_init(int, int) {}
inline void mul_init(int, int) {}
inline void copy_init(int) {}
inline void add_tiles(int, int) {}
inline void mul_tiles(int, int) {}
inline void copy_tile(int) {}
inline void copy_tile_init(int cb) { copy_init(cb); }
inline void copy_and_hold(int cb) { copy_tile(cb); }
}  // namespace ckernel
using namespace ckernel;
volatile int flag = 0;
)"};

// The findings for one kernel, as "<operation>: <active initializers>".
std::vector<std::string> Check(std::string_view kernel) {
    using namespace clang::ast_matchers;
    InitOperationPairing checker;
    MatchFinder finder;
    finder.addMatcher(functionDecl(isDefinition(), hasName("kernel_main")).bind("function"), &checker);
    const std::unique_ptr<clang::tooling::FrontendActionFactory> factory{
        clang::tooling::newFrontendActionFactory(&finder)};
    const bool parsed{clang::tooling::runToolOnCodeWithArgs(
        factory->create(),
        "#include \"" + std::string{kApiHeader} + "\"\n" + std::string{kernel},
        {"-std=c++20"},
        kKernelFile,
        "init-operation-pairing-test",
        std::make_shared<clang::PCHContainerOperations>(),
        {{std::string{kApiHeader}, std::string{kApiSource}}})};
    EXPECT_TRUE(parsed) << kernel;
    EXPECT_TRUE(checker.errors().empty()) << llvm::join(checker.errors(), "\n");

    std::vector<std::string> findings;
    for (const Finding& finding : checker.findings()) {
        EXPECT_TRUE(llvm::StringRef{finding.call_site}.starts_with(kKernelFile)) << finding.call_site;
        findings.push_back(finding.operation + ": " + llvm::join(finding.active_initializers, ","));
    }
    return findings;
}

using Findings = std::vector<std::string>;

TEST(InitOperationPairingTest, AcceptsOperationsAfterTheirInitializer) {
    EXPECT_EQ(
        Check("void kernel_main() { add_init(0, 1); add_tiles(0, 1); mul_init(0, 1); mul_tiles(0, 1); }"), Findings{});
    EXPECT_EQ(
        Check("void kernel_main() { for (int i = 0; i < 4; ++i) { add_init(0, 1); add_tiles(0, 1); } }"), Findings{});
}

TEST(InitOperationPairingTest, RequiresTheInitializerOnEveryPath) {
    EXPECT_EQ(Check("void kernel_main() { add_tiles(0, 1); }"), Findings{"add_tiles: none"});
    EXPECT_EQ(Check("void kernel_main() { add_tiles(0, 1); add_init(0, 1); }"), Findings{"add_tiles: none"});
    EXPECT_EQ(
        Check("void kernel_main() { if (flag) { add_init(0, 1); } add_tiles(0, 1); }"),
        Findings{"add_tiles: none,add_init"});
    EXPECT_EQ(
        Check("void kernel_main() { for (int i = 0; i < 4; ++i) { add_tiles(0, 1); add_init(0, 1); } }"),
        Findings{"add_tiles: none,add_init"});
}

TEST(InitOperationPairingTest, AnotherInitializerReplacesTheActiveOne) {
    EXPECT_EQ(
        Check("void kernel_main() { add_init(0, 1); mul_init(0, 1); add_tiles(0, 1); }"),
        Findings{"add_tiles: mul_init"});
    EXPECT_EQ(
        Check(
            "void kernel_main() { add_init(0, 1); add_tiles(0, 1); mul_init(0, 1); mul_tiles(0, 1); add_tiles(0, 1); "
            "}"),
        Findings{"add_tiles: mul_init"});
}

TEST(InitOperationPairingTest, FollowsKernelHelpers) {
    EXPECT_EQ(
        Check("static void setup() { add_init(0, 1); }\nvoid kernel_main() { setup(); add_tiles(0, 1); }"), Findings{});
    EXPECT_EQ(
        Check(
            "static void setup() { if (flag) { add_init(0, 1); } }\nvoid kernel_main() { setup(); add_tiles(0, 1); }"),
        Findings{"add_tiles: none,add_init"});
    EXPECT_EQ(
        Check("template <int N> void run() { add_tiles(0, N); }\nvoid kernel_main() { add_init(0, 1); run<1>(); }"),
        Findings{});
    EXPECT_EQ(
        Check("template <int N> void run() { add_tiles(0, N); }\nvoid kernel_main() { mul_init(0, 1); run<1>(); }"),
        Findings{"add_tiles: mul_init"});
}

TEST(InitOperationPairingTest, FollowsApiFunctionsAndReportsAtTheKernelCall) {
    EXPECT_EQ(Check("void kernel_main() { copy_tile_init(0); copy_tile(0); }"), Findings{});
    EXPECT_EQ(Check("void kernel_main() { copy_and_hold(0); }"), Findings{"copy_tile via copy_and_hold: none"});
    EXPECT_EQ(Check("void kernel_main() { copy_init(0); copy_and_hold(0); }"), Findings{});
}

TEST(InitOperationPairingTest, IgnoresKernelFunctionsWithApiNames) {
    EXPECT_EQ(
        Check("namespace user { void add_tiles(int, int) {} }\nvoid kernel_main() { user::add_tiles(0, 1); }"),
        Findings{});
}

}  // namespace
}  // namespace tt::lint
