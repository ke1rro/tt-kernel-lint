#include "ttkl/translator.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"

#include "sfpi_option_rules.hpp"

namespace tt::lint {

char TranslationError::ID{0};

void TranslationError::log(llvm::raw_ostream& os) const { os << message_; }

std::error_code TranslationError::convertToErrorCode() const { return llvm::inconvertibleErrorCode(); }

namespace {

using namespace rules;

[[nodiscard]] bool Contains(std::span<const std::string_view> values, std::string_view value) {
    for (const std::string_view candidate : values) {
        if (candidate == value) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool StartsWithAny(std::span<const std::string_view> prefixes, std::string_view argument) {
    for (const std::string_view prefix : prefixes) {
        if (argument.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<std::string_view> FindExact(
    std::span<const OptionRule> options, std::string_view argument) {
    for (const auto& [name, reason] : options) {
        if (name == argument) {
            return reason;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> FindPrefix(
    std::span<const OptionRule> options, std::string_view argument) {
    for (const auto& [name, reason] : options) {
        if (argument.starts_with(name)) {
            return reason;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool IsKeptJoined(std::string_view argument) {
    return StartsWithAny(kKeptPrefixes, argument) && !StartsWithAny(kForwardingPrefixes, argument);
}

// TT-Metal runs the compiler as "ccache [KEY=VALUE...] <compiler>" when TT_METAL_CCACHE_KERNEL_SUPPORT is set.
[[nodiscard]] llvm::Expected<std::size_t> CompilerIndex(std::span<const std::string> arguments) {
    if (arguments.empty()) {
        return llvm::make_error<TranslationError>(TranslationErrorKind::kMalformedCommand, kNoArgumentsMessage);
    }
    if (llvm::sys::path::filename(arguments.front()) != llvm::StringRef{kCompilerLauncher}) {
        return 0;
    }
    std::size_t index{1};
    while (index < arguments.size() && !arguments[index].starts_with('-') &&
           arguments[index].find('=') != std::string::npos) {
        ++index;
    }
    if (index == arguments.size()) {
        return llvm::make_error<TranslationError>(TranslationErrorKind::kMalformedCommand, kNoCompilerMessage);
    }
    return index;
}

class CommandTranslation {
    const Context& context_;
    const SfpiToolchain& toolchain_;
    TranslationResult result_;
    const CpuMapping* cpu_{nullptr};
    bool needs_cxx20_{false};
    bool translated_standard_{false};
    bool has_compile_{false};
    bool has_source_{false};

public:
    CommandTranslation(const Context& context, const SfpiToolchain& toolchain) :
        context_(context), toolchain_(toolchain) {}

    [[nodiscard]] llvm::Expected<TranslationResult> Run() &&;

private:
    [[nodiscard]] const std::vector<std::string>& arguments() const noexcept { return context_.command().arguments; }

    template <typename... Values>
    [[nodiscard]] llvm::Error Fail(TranslationErrorKind kind, const char* format, Values&&... values) const {
        const std::string detail{llvm::formatv(format, std::forward<Values>(values)...).str()};
        const KernelId& kernel{context_.kernel()};
        return llvm::make_error<TranslationError>(
            kind,
            llvm::formatv(
                kKernelContextMessage,
                kernel.name + "/" + kernel.variant,
                llvm::StringRef{ThreadRoleName(kernel.role)},
                detail)
                .str());
    }

    [[nodiscard]] llvm::Error RequireOperand(std::size_t index) const;

    void Keep(std::string_view argument);
    void Note(NoteKind kind, std::string original, std::string translated, std::string_view reason);

    [[nodiscard]] llvm::Error TranslateArgument(std::size_t& index);
    [[nodiscard]] llvm::Error TranslateSource(std::string_view argument);
    [[nodiscard]] llvm::Error TranslateStandard(std::string_view argument);
    [[nodiscard]] llvm::Error TranslateCpu(std::string_view argument);

    void AddSystemIncludes();
    void AddTypeModel();
    void AddDiagnostics();
    void AddHeaderPatches();
};

llvm::Error CommandTranslation::RequireOperand(std::size_t index) const {
    if (index + 1 < arguments().size()) {
        return llvm::Error::success();
    }
    return Fail(TranslationErrorKind::kMalformedCommand, kMissingOperandMessage, arguments()[index]);
}

void CommandTranslation::Keep(std::string_view argument) { result_.command.arguments.emplace_back(argument); }

void CommandTranslation::Note(NoteKind kind, std::string original, std::string translated, std::string_view reason) {
    result_.notes.push_back(TranslationNote{std::move(original), std::move(translated), std::string{reason}, kind});
}

llvm::Error CommandTranslation::TranslateSource(std::string_view argument) {
    if (argument != context_.command().source_file) {
        return Fail(
            TranslationErrorKind::kMalformedCommand,
            kUnexpectedPositionalMessage,
            llvm::StringRef{argument},
            context_.command().source_file);
    }
    if (has_source_) {
        return Fail(TranslationErrorKind::kMalformedCommand, kRepeatedSourceMessage);
    }
    has_source_ = true;
    Keep(argument);
    return llvm::Error::success();
}

llvm::Error CommandTranslation::TranslateStandard(std::string_view argument) {
    if (argument != kGccStandard) {
        return Fail(
            TranslationErrorKind::kUnsupportedOption,
            kUnsupportedStandardMessage,
            llvm::StringRef{argument},
            llvm::StringRef{kGccStandard});
    }
    if (!needs_cxx20_) {
        Keep(argument);
        return llvm::Error::success();
    }
    translated_standard_ = true;
    Note(NoteKind::kReplaced, std::string{argument}, std::string{kClangStandard}, kStandardReplacedReason);
    Keep(kClangStandard);
    return llvm::Error::success();
}

llvm::Error CommandTranslation::TranslateCpu(std::string_view argument) {
    if (cpu_ != nullptr) {
        return Fail(TranslationErrorKind::kMalformedCommand, kRepeatedCpuMessage);
    }
    const std::string_view mcpu{argument.substr(kCpuPrefix.size())};
    const ToolchainProfile& profile{toolchain_.profile()};
    for (const CpuMapping& cpu : profile.cpus) {
        if (cpu.mcpu != mcpu) {
            continue;
        }
        cpu_ = &cpu;
        const std::string target_option{std::string{kTargetPrefix} + std::string{cpu.triple}};
        const std::string march_option{std::string{kMarchPrefix} + std::string{cpu.march}};
        const std::string mabi_option{std::string{kMabiPrefix} + std::string{cpu.mabi}};
        Note(
            NoteKind::kReplaced,
            std::string{argument},
            target_option + " " + march_option + " " + mabi_option,
            kCpuReplacedReason);
        Keep(target_option);
        Keep(march_option);
        Keep(mabi_option);
        return llvm::Error::success();
    }
    const std::string supported{
        llvm::join(llvm::map_range(profile.cpus, [](const CpuMapping& cpu) { return cpu.mcpu; }), ", ")};
    return Fail(
        TranslationErrorKind::kUnsupportedCpu,
        kUnsupportedCpuMessage,
        llvm::StringRef{mcpu},
        llvm::StringRef{profile.name},
        supported);
}

llvm::Error CommandTranslation::TranslateArgument(std::size_t& index) {
    const std::string_view argument{arguments()[index]};

    if (!argument.starts_with('-')) {
        return TranslateSource(argument);
    }
    if (argument == kCompileOnly) {
        has_compile_ = true;
        Note(NoteKind::kReplaced, std::string{argument}, std::string{kSyntaxOnly}, kSyntaxOnlyReason);
        Keep(kSyntaxOnly);
        return llvm::Error::success();
    }
    if (argument == kOptimizeFast) {
        Note(
            NoteKind::kReplaced,
            std::string{argument},
            std::string{kOptimizeFastLevel} + " " + std::string{kFastMath},
            kOptimizeFastReason);
        Keep(kOptimizeFastLevel);
        Keep(kFastMath);
        return llvm::Error::success();
    }
    if (const auto reason = FindExact(kDroppedFlags, argument)) {
        Note(NoteKind::kRemoved, std::string{argument}, {}, *reason);
        return llvm::Error::success();
    }
    if (const auto reason = FindPrefix(kDroppedPrefixes, argument)) {
        Note(NoteKind::kRemoved, std::string{argument}, {}, *reason);
        return llvm::Error::success();
    }
    if (const auto reason = FindExact(kDroppedWithOperand, argument)) {
        if (llvm::Error error{RequireOperand(index)}) {
            return error;
        }
        Note(NoteKind::kRemoved, std::string{argument} + " " + arguments()[index + 1], {}, *reason);
        ++index;
        return llvm::Error::success();
    }
    if (argument.starts_with(kCpuPrefix)) {
        return TranslateCpu(argument);
    }
    if (argument.starts_with(kStandardPrefix)) {
        return TranslateStandard(argument);
    }
    if (Contains(kKeptWithOperand, argument)) {
        if (llvm::Error error{RequireOperand(index)}) {
            return error;
        }
        Keep(argument);
        Keep(arguments()[++index]);
        return llvm::Error::success();
    }
    if (Contains(kKeptFlags, argument) || IsKeptJoined(argument)) {
        Keep(argument);
        return llvm::Error::success();
    }
    return Fail(TranslationErrorKind::kUnsupportedOption, kUnsupportedOptionMessage, llvm::StringRef{argument});
}

void CommandTranslation::AddSystemIncludes() {
    std::string added{kNoStandardIncludes};
    Keep(kNoStandardIncludes);
    for (const std::string_view directory : cpu_->system_include_directories) {
        llvm::SmallString<256> path{toolchain_.root()};
        llvm::sys::path::append(path, directory);
        Keep(kSystemInclude);
        Keep(path.str());
        added += " ";
        added += kSystemInclude;
        added += " ";
        added += path.str();
    }
    Note(NoteKind::kAdded, {}, std::move(added), kSystemIncludesReason);
}

void CommandTranslation::AddTypeModel() {
    std::string added;
    for (const auto& [name, value] : toolchain_.profile().macro_overrides) {
        std::string undefine{std::string{kUndefinePrefix} + std::string{name.substr(0, name.find('('))}};
        std::string define{std::string{kDefinePrefix} + std::string{name} + "=" + std::string{value}};
        added += added.empty() ? "" : " ";
        added += undefine + " " + define;
        result_.command.arguments.push_back(std::move(undefine));
        result_.command.arguments.push_back(std::move(define));
    }
    if (!added.empty()) {
        Note(NoteKind::kAdded, {}, std::move(added), kTypeModelReason);
    }
}

void CommandTranslation::AddDiagnostics() {
    for (const auto& [name, reason] : kAddedDiagnostics) {
        Keep(name);
        Note(NoteKind::kAdded, {}, std::string{name}, reason);
    }
    if (translated_standard_) {
        Keep(kCxx20Diagnostic.name);
        Note(NoteKind::kAdded, {}, std::string{kCxx20Diagnostic.name}, kCxx20Diagnostic.reason);
    }
    Keep(kErrorLimit);
    Note(NoteKind::kAdded, {}, std::string{kErrorLimit}, kErrorLimitReason);
}

void CommandTranslation::AddHeaderPatches() {
    for (const HeaderPatch& patch : toolchain_.profile().patches) {
        llvm::SmallString<256> path{toolchain_.root()};
        llvm::sys::path::append(path, kSfpiIncludeDirectory, patch.file);
        Note(NoteKind::kPatchedHeader, std::string{path}, {}, patch.reason);
    }
}

llvm::Expected<TranslationResult> CommandTranslation::Run() && {
    llvm::Expected<std::size_t> compiler_index{CompilerIndex(arguments())};
    if (!compiler_index) {
        return Fail(TranslationErrorKind::kMalformedCommand, "{0}", llvm::toString(compiler_index.takeError()));
    }
    const std::span<const std::string> launcher{arguments().data(), *compiler_index};
    const std::string& compiler{arguments()[*compiler_index]};
    if (compiler != toolchain_.compiler()) {
        return Fail(
            TranslationErrorKind::kUnsupportedCompiler, kUnsupportedCompilerMessage, toolchain_.compiler(), compiler);
    }
    for (const std::string& argument : arguments()) {
        needs_cxx20_ = needs_cxx20_ || Contains(kStandardBackports, argument);
    }

    result_.command.directory = context_.command().directory;
    result_.command.source_file = context_.command().source_file;
    result_.command.arguments.reserve(arguments().size() + 2 * toolchain_.profile().macro_overrides.size() + 24);
    if (!launcher.empty()) {
        Note(NoteKind::kRemoved, llvm::join(launcher, " "), {}, kLauncherReason);
    }
    Note(NoteKind::kReplaced, compiler, std::string{kClangDriver}, kCompilerReplacedReason);
    Keep(kClangDriver);

    for (std::size_t index{*compiler_index + 1}; index < arguments().size(); ++index) {
        if (llvm::Error error{TranslateArgument(index)}) {
            return std::move(error);
        }
    }
    if (cpu_ == nullptr) {
        return Fail(TranslationErrorKind::kMalformedCommand, kMissingCpuMessage);
    }
    if (!has_compile_) {
        return Fail(TranslationErrorKind::kMalformedCommand, kMissingCompileMessage);
    }
    if (!has_source_) {
        return Fail(TranslationErrorKind::kMalformedCommand, kMissingSourceMessage, context_.command().source_file);
    }
    if (needs_cxx20_ && !translated_standard_) {
        return Fail(TranslationErrorKind::kUnsupportedOption, kMissingStandardMessage, llvm::StringRef{kGccStandard});
    }
    AddSystemIncludes();
    AddTypeModel();
    AddDiagnostics();
    AddHeaderPatches();
    return std::move(result_);
}

}  // namespace

llvm::Expected<std::string_view> FindSfpiCompiler(const CompileCommand& command) {
    llvm::Expected<std::size_t> index{CompilerIndex(command.arguments)};
    if (!index) {
        return index.takeError();
    }
    return std::string_view{command.arguments[*index]};
}

llvm::Expected<TranslationResult> TranslateSfpiCommand(const Context& context, const SfpiToolchain& toolchain) {
    return CommandTranslation(context, toolchain).Run();
}

}  // namespace tt::lint
