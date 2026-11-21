#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "ttkl/capture.hpp"
#include "ttkl/check/init_operation_pairing.hpp"
#include "ttkl/frontend.hpp"
#include "ttkl/sfpi_toolchain.hpp"
#include "ttkl/target_profile.hpp"
#include "ttkl/translator.hpp"

namespace {

namespace cl = llvm::cl;

using clang::ast_matchers::functionDecl;
using clang::ast_matchers::hasName;
using clang::ast_matchers::isDefinition;

cl::OptionCategory option_category{"tt-init-check options"};

cl::opt<std::string> input_log_path{
    "input-log",
    cl::desc("TT-Metal JIT log with 'g++ compile cmd:' lines"),
    cl::value_desc("path"),
    cl::init(""),
    cl::cat(option_category)};

cl::opt<std::string> kernel_filter{
    "kernel", cl::desc("Kernel name or NAME/VARIANT"), cl::value_desc("name"), cl::init(""), cl::cat(option_category)};

cl::opt<std::string> role_filter{
    "role",
    cl::desc("TRISC role: unpack, math, or pack"),
    cl::value_desc("role"),
    cl::init(""),
    cl::cat(option_category)};

}  // namespace

int main(int argc, const char** argv) {
    cl::HideUnrelatedOptions(option_category);
    const bool parsed{cl::ParseCommandLineOptions(
        argc, argv, "tt-init-check: translate a TT-Metal JIT log and check kernel_main.\n", &llvm::errs())};
    if (!parsed) {
        return 2;
    }

    if (input_log_path.empty()) {
        llvm::errs() << "tt-init-check: --input-log is required\n";
        return 2;
    }

    std::optional<tt::lint::ThreadRole> role;
    if (!role_filter.empty()) {
        role = tt::lint::ParseThreadRole(role_filter);
        if (!role.has_value()) {
            llvm::errs() << "tt-init-check: unknown role '" << role_filter << "'\n";
            return 2;
        }
    }

    const tt::lint::CaptureFilter filter{kernel_filter.getValue(), role};

    llvm::Expected<std::vector<tt::lint::Context>> contexts{tt::lint::ReadCaptureLog(input_log_path, filter)};
    if (!contexts) {
        llvm::errs() << "tt-init-check: " << llvm::toString(contexts.takeError()) << "\n";
        return 1;
    }
    if (contexts->empty()) {
        llvm::errs() << "tt-init-check: no matching compute-kernel commands in " << input_log_path << "\n";
        return 1;
    }

    bool failed{false};
    std::map<std::string, tt::lint::SfpiToolchain, std::less<>> toolchains;
    const auto kernel_main_matcher = functionDecl(isDefinition(), hasName("kernel_main")).bind("function");

    for (const tt::lint::Context& context : *contexts) {
        llvm::Expected<std::string_view> compiler{tt::lint::FindSfpiCompiler(context.command())};
        if (!compiler) {
            llvm::errs() << "tt-init-check: " << llvm::toString(compiler.takeError()) << "\n";
            failed = true;
            continue;
        }
        auto toolchain = toolchains.find(*compiler);
        if (toolchain == toolchains.end()) {
            llvm::Expected<tt::lint::SfpiToolchain> opened{tt::lint::SfpiToolchain::OpenForCompiler(
                *compiler, tt::lint::WormholeSfpiProfile(), *llvm::vfs::getRealFileSystem())};
            if (!opened) {
                llvm::errs() << "tt-init-check: " << context.kernel().name << ": " << llvm::toString(opened.takeError())
                             << "\n";
                failed = true;
                continue;
            }
            toolchain = toolchains.emplace(std::string{*compiler}, std::move(*opened)).first;
        }

        llvm::Expected<tt::lint::TranslationResult> translated{
            tt::lint::TranslateSfpiCommand(context, toolchain->second)};
        if (!translated) {
            llvm::errs() << "tt-init-check: " << llvm::toString(translated.takeError()) << "\n";
            failed = true;
            continue;
        }

        tt::lint::InitOperationPairing checker;
        clang::ast_matchers::MatchFinder finder;
        finder.addMatcher(kernel_main_matcher, &checker);

        const std::unique_ptr<clang::tooling::FrontendActionFactory> factory{
            clang::tooling::newFrontendActionFactory(&finder)};
        if (llvm::Error error{tt::lint::RunClangTool(translated->command, toolchain->second.overlays(), *factory)}) {
            llvm::errs() << "tt-init-check: " << context.kernel().name << ": " << llvm::toString(std::move(error))
                         << "\n";
            failed = true;
            continue;
        }

        for (const auto& [id, block] : checker.blocks()) {
            llvm::outs() << context.kernel().name;
            if (!context.kernel().variant.empty()) {
                llvm::outs() << "/" << context.kernel().variant;
            }
            llvm::outs() << " [" << tt::lint::ThreadRoleName(context.kernel().role) << "]: block " << id << "\n";
            for (const tt::lint::CallSite& call : block.calls) {
                llvm::outs() << "  call " << call.name << " " << call.usr << " at " << call.call_site << "\n";
            }
            for (const tt::lint::CfgBlockId predecessor : block.predecessors) {
                llvm::outs() << "  from block " << predecessor << "\n";
            }
        }
    }

    return failed ? 1 : 0;
}
