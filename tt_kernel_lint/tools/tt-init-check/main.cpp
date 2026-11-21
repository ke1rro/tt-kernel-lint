#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include "ttkl/capture.hpp"
#include "ttkl/check/init_operation_pairing.hpp"
#include "ttkl/sfpi_profile.hpp"
#include "ttkl/translator.hpp"

namespace {

namespace cl = llvm::cl;

using clang::ast_matchers::functionDecl;
using clang::ast_matchers::hasName;
using clang::ast_matchers::isDefinition;

cl::OptionCategory kOptionCategory("tt-init-check options");

cl::opt<std::string> kInputLog(
    "input-log",
    cl::desc("TT-Metal JIT log with 'g++ compile cmd:' lines"),
    cl::value_desc("path"),
    cl::init(""),
    cl::cat(kOptionCategory));

cl::opt<std::string> kKernel(
    "kernel", cl::desc("Kernel name or NAME/VARIANT"), cl::value_desc("name"), cl::init(""), cl::cat(kOptionCategory));

cl::opt<std::string> kRole(
    "role",
    cl::desc("TRISC role: unpack, math, or pack"),
    cl::value_desc("role"),
    cl::init(""),
    cl::cat(kOptionCategory));

const auto kKernelMainMatcher = functionDecl(isDefinition(), hasName("kernel_main")).bind("function");

class InMemoryCompilationDatabase final : public clang::tooling::CompilationDatabase {
public:
    explicit InMemoryCompilationDatabase(std::vector<clang::tooling::CompileCommand> commands) :
        commands_(std::move(commands)) {}

    std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef) const override { return commands_; }

    std::vector<std::string> getAllFiles() const override {
        std::vector<std::string> files;
        files.reserve(commands_.size());
        for (const clang::tooling::CompileCommand& command : commands_) {
            files.push_back(command.Filename);
        }
        return files;
    }

    std::vector<clang::tooling::CompileCommand> getAllCompileCommands() const override { return commands_; }

private:
    std::vector<clang::tooling::CompileCommand> commands_;
};

std::optional<tt::lint::ThreadRole> ParseRole(std::string_view value) {
    if (value == "unpack") {
        return tt::lint::ThreadRole::kUnpack;
    }
    if (value == "math") {
        return tt::lint::ThreadRole::kMath;
    }
    if (value == "pack") {
        return tt::lint::ThreadRole::kPack;
    }
    return std::nullopt;
}

std::string_view RoleName(tt::lint::ThreadRole role) {
    switch (role) {
        case tt::lint::ThreadRole::kUnpack: return "unpack";
        case tt::lint::ThreadRole::kMath: return "math";
        case tt::lint::ThreadRole::kPack: return "pack";
    }
    return "unknown";
}

std::optional<std::string> FindMcpu(const std::vector<std::string>& arguments) {
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index].starts_with("-mcpu=")) {
            return arguments[index].substr(6);
        }
        if (arguments[index] == "-mcpu" && index + 1 < arguments.size()) {
            return arguments[index + 1];
        }
    }
    return std::nullopt;
}

}  // namespace

int main(int argc, const char** argv) {
    cl::HideUnrelatedOptions(kOptionCategory);
    const bool parsed = cl::ParseCommandLineOptions(
        argc, argv, "tt-init-check: translate a TT-Metal JIT log and check kernel_main.\n", &llvm::errs());
    if (!parsed) {
        return 2;
    }

    if (kInputLog.empty()) {
        llvm::errs() << "tt-init-check: --input-log is required\n";
        return 2;
    }

    std::optional<tt::lint::ThreadRole> role;
    if (!kRole.empty()) {
        role = ParseRole(kRole);
        if (!role.has_value()) {
            llvm::errs() << "tt-init-check: unknown role '" << kRole << "'\n";
            return 2;
        }
    }

    tt::lint::CaptureFilter filter;
    filter.kernel = kKernel;
    filter.role = role;

    llvm::Expected<std::vector<tt::lint::Context>> contexts = tt::lint::ReadCaptureLog(kInputLog, filter);
    if (!contexts) {
        llvm::errs() << "tt-init-check: " << contexts.takeError() << "\n";
        return 1;
    }
    if (contexts->empty()) {
        llvm::errs() << "tt-init-check: no matching compute-kernel commands in " << kInputLog << "\n";
        return 1;
    }

    bool failed = false;

    for (const tt::lint::Context& context : *contexts) {
        const std::optional<std::string> mcpu = FindMcpu(context.command().arguments);
        if (!mcpu.has_value()) {
            llvm::errs() << "tt-init-check: " << context.kernel().name << ": no -mcpu option\n";
            failed = true;
            continue;
        }

        const tt::lint::ToolchainProfile profile =
            tt::lint::MakeSfpiProfile(context.command().arguments.front(), *mcpu);
        if (profile.cpus.empty()) {
            llvm::errs() << "tt-init-check: " << context.kernel().name << ": unsupported -mcpu=" << *mcpu << "\n";
            failed = true;
            continue;
        }

        llvm::Expected<tt::lint::TranslationResult> translated = tt::lint::TranslateSfpiCommand(context, profile);
        if (!translated) {
            llvm::errs() << "tt-init-check: " << context.kernel().name << ": " << translated.takeError() << "\n";
            failed = true;
            continue;
        }

        const clang::tooling::CompileCommand command{
            translated->command.directory,
            translated->command.source_file,
            translated->command.arguments,
            /*Output=*/""};

        InMemoryCompilationDatabase database({command});
        tt::lint::InitOperationPairing checker;
        clang::ast_matchers::MatchFinder finder;
        finder.addMatcher(kKernelMainMatcher, &checker);

        clang::tooling::ClangTool tool(database, {command.Filename});
        std::unique_ptr<clang::tooling::FrontendActionFactory> factory =
            clang::tooling::newFrontendActionFactory(&finder);
        const int status = tool.run(factory.get());
        if (status != 0) {
            failed = true;
            continue;
        }

        for (const auto& [id, block] : checker.blocks()) {
            llvm::outs() << context.kernel().name;
            if (!context.kernel().variant.empty()) {
                llvm::outs() << "/" << context.kernel().variant;
            }
            llvm::outs() << " [" << llvm::StringRef(RoleName(context.kernel().role)) << "]: block " << id << "\n";
            for (const tt::lint::CallSite& call : block.calls) {
                llvm::outs() << "  call " << call.name << " " << call.usr << " at " << call.call_site << "\n";
            }
            for (const unsigned predecessor : block.predecessors) {
                llvm::outs() << "  from block " << predecessor << "\n";
            }
        }
    }

    return failed ? 1 : 0;
}
