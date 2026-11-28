#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
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

// One finding and the contexts it came from: a kernel compiles once per TRISC role and compile-time variant.
struct ReportedFinding {
    tt::lint::Finding finding;
    std::vector<std::string> contexts;
};

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

cl::opt<bool> quiet_sdk_warnings{
    "quiet-sdk-warnings",
    cl::desc("Save full Clang diagnostics beside the input log; hide only known SDK warnings"),
    cl::cat(option_category)};

// Presentation only: Clang still emits and counts these diagnostics. Its handling
// of SFPI attributes does not establish semantic equivalence to SFPI GCC.
class SdkDiagnosticPrinter final : public clang::DiagnosticConsumer {
    llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> options_{new clang::DiagnosticOptions};
    clang::TextDiagnosticPrinter terminal_{llvm::errs(), options_.get()};
    clang::TextDiagnosticPrinter full_;
    std::string metal_prefix_;
    std::string sfpi_prefix_;
    bool hidden_{false};

    [[nodiscard]] bool IsKnownWarning(const clang::Diagnostic& info) const {
        if (!info.hasSourceManager() || info.getLocation().isInvalid()) {
            return false;
        }
        const clang::SourceManager& sources{info.getSourceManager()};
        llvm::SmallString<256> file{sources.getFilename(sources.getSpellingLoc(info.getLocation()))};
        llvm::sys::path::remove_dots(file, /*remove_dot_dot=*/true);
        const llvm::StringRef path{file};
        const bool metal_file{!metal_prefix_.empty() && path.starts_with(metal_prefix_)};
        const bool sfpi_file{path.starts_with(sfpi_prefix_)};
        llvm::SmallString<256> message;
        info.FormatDiagnostic(message);
        if (message == "unknown attribute 'rvtt_l1_ptr' ignored" ||
            message == "unknown attribute 'rvtt_reg_ptr' ignored") {
            return metal_file || sfpi_file;
        }
        if (path == sfpi_prefix_ + "lltt.h" &&
            message == "attribute 'rvtt_reg_ptr' ignored, because it cannot be applied to a type") {
            return true;
        }
        return metal_file && path == metal_prefix_ + "tt-llk/tt_llk_wormhole_b0/llk_lib/llk_math_matmul.h" &&
               message == "variable 't' is incremented both in the loop header and in the loop body";
    }

public:
    SdkDiagnosticPrinter(
        llvm::raw_ostream& full_log, const tt::lint::CompileCommand& command, llvm::StringRef sfpi_root) :
        full_(full_log, options_.get()), sfpi_prefix_((sfpi_root + "/include/").str()) {
        const llvm::StringRef source{command.source_file};
        const std::size_t position{source.find("/tt_metal/")};
        if (position != llvm::StringRef::npos) {
            metal_prefix_ = source.take_front(position + llvm::StringRef{"/tt_metal/"}.size()).str();
        }
    }

    void BeginSourceFile(const clang::LangOptions& options, const clang::Preprocessor* preprocessor) override {
        terminal_.BeginSourceFile(options, preprocessor);
        full_.BeginSourceFile(options, preprocessor);
    }

    void EndSourceFile() override {
        terminal_.EndSourceFile();
        full_.EndSourceFile();
    }

    void HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& info) override {
        full_.HandleDiagnostic(level, info);
        if (level != clang::DiagnosticsEngine::Note) {
            hidden_ = level == clang::DiagnosticsEngine::Warning && IsKnownWarning(info);
        }
        if (!hidden_) {
            // The terminal summary counts visible diagnostics. The full printer
            // and Clang's diagnostics engine retain every diagnostic.
            DiagnosticConsumer::HandleDiagnostic(level, info);
            terminal_.HandleDiagnostic(level, info);
        }
    }
};

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

    llvm::SmallString<256> diagnostics_path;
    std::unique_ptr<llvm::raw_fd_ostream> diagnostics_log;
    if (quiet_sdk_warnings) {
        int descriptor{-1};
        if (const std::error_code error{llvm::sys::fs::createUniqueFile(
                input_log_path.getValue() + ".diagnostics-%%%%%%.log", descriptor, diagnostics_path)}) {
            llvm::errs() << "tt-init-check: cannot save Clang diagnostics: " << error.message() << "\n";
            return 2;
        }
        diagnostics_log = std::make_unique<llvm::raw_fd_ostream>(descriptor, /*shouldClose=*/true);
    }

    bool failed{false};
    std::map<std::string, ReportedFinding> reported;
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
        std::unique_ptr<SdkDiagnosticPrinter> diagnostics;
        if (diagnostics_log) {
            diagnostics =
                std::make_unique<SdkDiagnosticPrinter>(*diagnostics_log, translated->command, toolchain->second.root());
        }
        if (llvm::Error error{tt::lint::RunClangTool(
                translated->command, toolchain->second.overlays(), *factory, diagnostics.get())}) {
            llvm::errs() << "tt-init-check: " << context.kernel().name << ": " << llvm::toString(std::move(error))
                         << "\n";
            failed = true;
            continue;
        }

        for (const std::string& error : checker.errors()) {
            llvm::errs() << "tt-init-check: " << context.kernel().name << ": " << error << "\n";
            failed = true;
        }
        const tt::lint::KernelId& kernel{context.kernel()};
        const std::string label{
            kernel.name + (kernel.variant.empty() ? "" : "/" + kernel.variant) + " " +
            std::string{tt::lint::ThreadRoleName(kernel.role)}};
        for (const tt::lint::Finding& finding : checker.findings()) {
            const std::string key{
                finding.call_site + '\x1f' + finding.operation + '\x1f' + finding.initializer + '\x1f' +
                llvm::join(finding.active_initializers, ",")};
            ReportedFinding& entry{reported[key]};
            if (entry.contexts.empty()) {
                entry.finding = finding;
            }
            entry.contexts.push_back(label);
        }
    }

    for (const auto& entry : reported) {
        const auto& [finding, contexts] = entry.second;
        llvm::outs() << finding.call_site << ": " << finding.operation << " has no matching " << finding.initializer
                     << " on every path (active: " << llvm::join(finding.active_initializers, ", ") << ") in "
                     << llvm::join(contexts, ", ") << "\n";
    }
    if (diagnostics_log) {
        diagnostics_log->flush();
        if (diagnostics_log->has_error()) {
            llvm::errs() << "tt-init-check: could not write full Clang diagnostics\n";
            diagnostics_log->clear_error();
            failed = true;
        }
        llvm::errs() << "Clang diagnostics: " << diagnostics_path << "\n";
    }
    if (failed) {
        llvm::errs() << "tt-init-check: analysis could not complete\n";
    } else if (reported.empty()) {
        llvm::outs() << "No initialization issues found\n";
    }
    return (failed || !reported.empty()) ? 1 : 0;
}
