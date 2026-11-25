#include <array>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "clang/AST/ASTConsumer.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "ttkl/capture.hpp"
#include "ttkl/compilation_ctx.hpp"
#include "ttkl/frontend.hpp"
#include "ttkl/jit_capture.hpp"
#include "ttkl/sfpi_toolchain.hpp"
#include "ttkl/target_profile.hpp"
#include "ttkl/translator.hpp"

namespace {

using tt::lint::CompileCommand;
using tt::lint::Context;
using tt::lint::SfpiToolchain;
using tt::lint::TranslationResult;

llvm::cl::OptionCategory tool_category{"tt-kernel-lint options"};

llvm::cl::opt<std::string> jit_log_path{
    "jit-log",
    llvm::cl::desc("TT-Metal log written with TT_METAL_LOG_KERNELS_COMPILE_COMMANDS=1 and TT_LOGGER_LEVEL=info"),
    llvm::cl::value_desc("file"),
    llvm::cl::cat(tool_category)};
llvm::cl::opt<std::string> compile_commands_path{
    "compile-commands",
    llvm::cl::desc("compile_commands.json with the SFPI GCC commands, e.g. captured with bear"),
    llvm::cl::value_desc("file"),
    llvm::cl::cat(tool_category)};
llvm::cl::opt<std::string> kernel_filter{
    "kernel",
    llvm::cl::desc("Only contexts of this kernel, as <name> or <name>/<variant>"),
    llvm::cl::cat(tool_category)};
llvm::cl::opt<std::string> role_filter{
    "role", llvm::cl::desc("Only contexts of this TRISC role: unpack, math or pack"), llvm::cl::cat(tool_category)};
llvm::cl::opt<bool> print_translation{
    "print-translation",
    llvm::cl::desc("Print each translated Clang command and the reason for every change"),
    llvm::cl::cat(tool_category)};
llvm::cl::opt<std::string> ast_dump_filter{
    "ast-dump-filter",
    llvm::cl::desc("Dump the AST of declarations whose qualified name contains this string"),
    llvm::cl::cat(tool_category)};
llvm::cl::opt<bool> show_warnings{
    "warnings", llvm::cl::desc("Print Clang warnings, not only errors"), llvm::cl::cat(tool_category)};

constexpr auto kLeakingEnvironment = std::to_array<const char*>({"CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH"});

// Counts every diagnostic and prints errors, plus warnings when asked.
class DiagnosticPrinter final : public clang::DiagnosticConsumer {
    clang::TextDiagnosticPrinter printer_;
    bool print_warnings_;

public:
    DiagnosticPrinter(clang::DiagnosticOptions* options, bool print_warnings) :
        printer_(llvm::errs(), options), print_warnings_(print_warnings) {}

    void BeginSourceFile(const clang::LangOptions& lang_options, const clang::Preprocessor* preprocessor) override {
        printer_.BeginSourceFile(lang_options, preprocessor);
    }

    void EndSourceFile() override { printer_.EndSourceFile(); }

    void HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& info) override {
        DiagnosticConsumer::HandleDiagnostic(level, info);
        if (level >= clang::DiagnosticsEngine::Error || print_warnings_) {
            printer_.HandleDiagnostic(level, info);
        }
    }
};

class AstDumpAction final : public clang::ASTFrontendAction {
    std::string filter_;

public:
    explicit AstDumpAction(std::string filter) : filter_(std::move(filter)) {}

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance& /*instance*/, llvm::StringRef /*file*/) override {
        return clang::CreateASTDumper(
            nullptr,
            filter_,
            /*DumpDecls=*/true,
            /*Deserialize=*/false,
            /*DumpLookups=*/false,
            /*DumpDeclTypes=*/false,
            clang::ADOF_Default);
    }
};

class AstDumpActionFactory final : public clang::tooling::FrontendActionFactory {
    std::string filter_;

public:
    explicit AstDumpActionFactory(std::string filter) : filter_(std::move(filter)) {}

    std::unique_ptr<clang::FrontendAction> create() override { return std::make_unique<AstDumpAction>(filter_); }
};

[[nodiscard]] llvm::Expected<std::vector<CompileCommand>> ReadCommands() {
    if (!jit_log_path.empty()) {
        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> log{llvm::MemoryBuffer::getFile(jit_log_path)};
        if (!log) {
            return llvm::createStringError(log.getError(), "cannot read '%s'", jit_log_path.c_str());
        }
        return tt::lint::ParseJitBuildLog((*log)->getBuffer());
    }
    std::string error_message;
    const std::unique_ptr<clang::tooling::JSONCompilationDatabase> database{
        clang::tooling::JSONCompilationDatabase::loadFromFile(
            compile_commands_path, error_message, clang::tooling::JSONCommandLineSyntax::AutoDetect)};
    if (!database) {
        return llvm::createStringError(llvm::inconvertibleErrorCode(), error_message);
    }
    std::vector<CompileCommand> commands;
    for (clang::tooling::CompileCommand& command : database->getAllCompileCommands()) {
        commands.push_back(
            CompileCommand{std::move(command.Directory), std::move(command.Filename), std::move(command.CommandLine)});
    }
    return commands;
}

void PrintTranslation(const TranslationResult& translation) {
    llvm::outs() << "  directory: " << translation.command.directory << "\n  command:";
    for (const std::string& argument : translation.command.arguments) {
        llvm::outs() << " " << argument;
    }
    llvm::outs() << "\n";
    for (const auto& [original, translated, reason, kind] : translation.notes) {
        switch (kind) {
            case tt::lint::NoteKind::kAdded: llvm::outs() << "  + " << translated; break;
            case tt::lint::NoteKind::kRemoved: llvm::outs() << "  - " << original; break;
            case tt::lint::NoteKind::kReplaced: llvm::outs() << "  ~ " << original << " -> " << translated; break;
            case tt::lint::NoteKind::kPatchedHeader:
                llvm::outs() << "  ! " << original << " (patched in memory)";
                break;
        }
        llvm::outs() << "\n      " << reason << "\n";
    }
}

// The toolchains already opened in this run, keyed by compiler path. A rejected toolchain is reported once.
class ToolchainCache {
    std::map<std::string, SfpiToolchain, std::less<>> toolchains_;
    std::set<std::string, std::less<>> rejected_;

public:
    [[nodiscard]] llvm::Expected<const SfpiToolchain&> Get(std::string_view compiler) {
        if (const auto found = toolchains_.find(compiler); found != toolchains_.end()) {
            return found->second;
        }
        if (rejected_.contains(compiler)) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                llvm::Twine{"the SFPI toolchain of '"} + compiler + "' was rejected above");
        }
        llvm::Expected<SfpiToolchain> toolchain{
            SfpiToolchain::OpenForCompiler(compiler, tt::lint::WormholeSfpiProfile(), *llvm::vfs::getRealFileSystem())};
        if (!toolchain) {
            rejected_.emplace(compiler);
            return toolchain.takeError();
        }
        return toolchains_.emplace(std::string{compiler}, std::move(*toolchain)).first->second;
    }
};

}  // namespace

int main(int argc, const char** argv) {
    const llvm::InitLLVM init{argc, argv};
    llvm::cl::HideUnrelatedOptions(tool_category);
    llvm::cl::ParseCommandLineOptions(
        argc,
        argv,
        "Translates TT-Metal Wormhole compute kernel compilations from SFPI GCC to Clang and parses them in "
        "process.\n");

    if (jit_log_path.empty() == compile_commands_path.empty()) {
        llvm::errs() << "error: give exactly one of --jit-log and --compile-commands\n";
        return EXIT_FAILURE;
    }
    std::optional<tt::lint::ThreadRole> role;
    if (!role_filter.empty()) {
        role = tt::lint::ParseThreadRole(role_filter);
        if (!role) {
            llvm::errs() << "error: unknown role '" << role_filter << "', expected unpack, math or pack\n";
            return EXIT_FAILURE;
        }
    }
    for (const char* variable : kLeakingEnvironment) {
        if (const char* value = std::getenv(variable); value != nullptr && *value != '\0') {
            llvm::errs() << "warning: " << variable << " is set, the Clang driver appends it to every include path\n";
        }
    }

    llvm::Expected<std::vector<CompileCommand>> commands{ReadCommands()};
    if (!commands) {
        llvm::errs() << "error: " << llvm::toString(commands.takeError()) << "\n";
        return EXIT_FAILURE;
    }

    const tt::lint::CaptureFilter filter{kernel_filter.getValue(), role};
    std::size_t skipped{0};
    std::size_t failed{0};
    std::vector<Context> contexts;
    for (CompileCommand& command : *commands) {
        llvm::Expected<std::optional<Context>> context{tt::lint::IdentifyComputeContext(std::move(command))};
        if (!context) {
            llvm::errs() << "error: " << llvm::toString(context.takeError()) << "\n";
            ++failed;
        } else if (!*context) {
            ++skipped;
        } else if (tt::lint::MatchesCaptureFilter((**context).kernel(), filter)) {
            contexts.push_back(std::move(**context));
        }
    }

    if (contexts.empty()) {
        llvm::errs() << "error: no compute kernel context matches the input and the filters\n";
        return EXIT_FAILURE;
    }

    const llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> diagnostic_options{new clang::DiagnosticOptions()};
    diagnostic_options->ShowOptionNames = true;
    DiagnosticPrinter diagnostics{diagnostic_options.get(), show_warnings};
    ToolchainCache toolchains;
    const std::unique_ptr<clang::tooling::FrontendActionFactory> action{
        ast_dump_filter.empty() ? clang::tooling::newFrontendActionFactory<clang::SyntaxOnlyAction>()
                                : std::make_unique<AstDumpActionFactory>(ast_dump_filter)};

    for (const Context& context : contexts) {
        const tt::lint::KernelId& kernel{context.kernel()};
        const std::string label{
            llvm::formatv("{0}/{1} {2}", kernel.name, kernel.variant, tt::lint::ThreadRoleName(kernel.role)).str()};
        llvm::Expected<std::string_view> compiler{tt::lint::FindSfpiCompiler(context.command())};
        llvm::Expected<const SfpiToolchain&> toolchain{
            compiler ? toolchains.Get(*compiler) : llvm::Expected<const SfpiToolchain&>{compiler.takeError()}};
        if (!toolchain) {
            llvm::errs() << "error: " << label << ": " << llvm::toString(toolchain.takeError()) << "\n";
            ++failed;
            continue;
        }
        llvm::Expected<TranslationResult> translation{tt::lint::TranslateSfpiCommand(context, *toolchain)};
        if (!translation) {
            llvm::errs() << "error: " << llvm::toString(translation.takeError()) << "\n";
            ++failed;
            continue;
        }
        llvm::outs() << label << "\n";
        if (print_translation) {
            PrintTranslation(*translation);
        }
        diagnostics.clear();
        llvm::Error parse_error{
            tt::lint::RunClangTool(translation->command, toolchain->overlays(), *action, &diagnostics)};
        const bool parsed{!parse_error};
        llvm::consumeError(std::move(parse_error));
        llvm::outs() << llvm::formatv(
            "  {0}: {1} errors, {2} warnings\n",
            parsed ? "parsed" : "failed",
            diagnostics.getNumErrors(),
            diagnostics.getNumWarnings());
        failed += parsed ? 0 : 1;
    }

    llvm::outs() << llvm::formatv(
        "{0} compute contexts, {1} other compile commands skipped, {2} failed\n", contexts.size(), skipped, failed);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
