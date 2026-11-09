#include "ttkl/translator.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"

#include "sfpi_option_rules.hpp"

namespace tt::lint {

char TranslationError::ID = 0;

void TranslationError::log(llvm::raw_ostream& os) const { os << message_; }

std::error_code TranslationError::convertToErrorCode() const { return llvm::inconvertibleErrorCode(); }

namespace {

using namespace rules;

bool Contains(std::span<const std::string_view> values, std::string_view value) {
    for (const std::string_view candidate : values) {
        if (candidate == value) {
            return true;
        }
    }
    return false;
}

const DroppedOption* FindExact(std::span<const DroppedOption> options, std::string_view argument) {
    for (const DroppedOption& option : options) {
        if (option.name == argument) {
            return &option;
        }
    }
    return nullptr;
}

const DroppedOption* FindPrefix(std::span<const DroppedOption> options, std::string_view argument) {
    for (const DroppedOption& option : options) {
        if (argument.starts_with(option.name)) {
            return &option;
        }
    }
    return nullptr;
}

bool HasKeptPrefix(std::string_view argument) {
    for (const std::string_view prefix : kKeptPrefixes) {
        if (argument.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

class CommandTranslation {
    const Context& context_;
    const ToolchainProfile& profile_;
    TranslationResult result_;
    bool needs_cxx20_ = false;
    bool has_cpu_ = false;
    bool has_source_ = false;

public:
    CommandTranslation(const Context& context, const ToolchainProfile& profile) :
        context_(context), profile_(profile) {}

    [[nodiscard]] llvm::Expected<TranslationResult> Run() &&;

private:
    [[nodiscard]] const std::vector<std::string>& arguments() const { return context_.command().arguments; }

    template <typename... Values>
    [[nodiscard]] llvm::Error Fail(TranslationErrorKind kind, const char* format, Values&&... values) const {
        const std::string detail = llvm::formatv(format, std::forward<Values>(values)...).str();
        return llvm::make_error<TranslationError>(
            kind, llvm::formatv(kKernelContextMessage, context_.kernel().name, detail).str());
    }

    [[nodiscard]] llvm::Error RequireOperand(std::size_t index) const;

    void Keep(std::string_view argument);
    void Remove(std::string_view original, std::string_view reason);
    void Replace(std::string_view original, std::string_view translated, std::string_view reason);

    [[nodiscard]] llvm::Error TranslateArgument(std::size_t& index);
    [[nodiscard]] llvm::Error TranslateSource(std::string_view argument);
    [[nodiscard]] llvm::Error TranslateStandard(std::string_view argument);
    [[nodiscard]] llvm::Error TranslateCpu(std::string_view argument);
};

llvm::Error CommandTranslation::RequireOperand(std::size_t index) const {
    if (index + 1 < arguments().size()) {
        return llvm::Error::success();
    }
    return Fail(TranslationErrorKind::kMalformedCommand, kMissingOperandMessage, arguments()[index]);
}

void CommandTranslation::Keep(std::string_view argument) { result_.command.arguments.emplace_back(argument); }

void CommandTranslation::Remove(std::string_view original, std::string_view reason) {
    result_.notes.push_back(TranslationNote{NoteKind::kRemoved, std::string(original), {}, std::string(reason)});
}

void CommandTranslation::Replace(std::string_view original, std::string_view translated, std::string_view reason) {
    result_.notes.push_back(
        TranslationNote{NoteKind::kReplaced, std::string(original), std::string(translated), std::string(reason)});
}

llvm::Error CommandTranslation::TranslateSource(std::string_view argument) {
    if (argument != context_.command().source_file) {
        return Fail(
            TranslationErrorKind::kMalformedCommand,
            kUnexpectedPositionalMessage,
            llvm::StringRef(argument),
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
            llvm::StringRef(argument),
            llvm::StringRef(kGccStandard));
    }
    if (!needs_cxx20_) {
        Keep(argument);
        return llvm::Error::success();
    }
    Replace(argument, kClangStandard, kStandardReplacedReason);
    Keep(kClangStandard);
    return llvm::Error::success();
}

llvm::Error CommandTranslation::TranslateCpu(std::string_view argument) {
    if (has_cpu_) {
        return Fail(TranslationErrorKind::kMalformedCommand, kRepeatedCpuMessage);
    }
    const std::string_view mcpu = argument.substr(kCpuPrefix.size());
    for (const CpuMapping& cpu : profile_.cpus) {
        if (cpu.mcpu != mcpu) {
            continue;
        }
        has_cpu_ = true;
        const std::string target = std::string(kTargetPrefix) + cpu.triple;
        const std::string march = std::string(kMarchPrefix) + cpu.march;
        const std::string mabi = std::string(kMabiPrefix) + cpu.mabi;
        Replace(argument, target + " " + march + " " + mabi, kCpuReplacedReason);
        Keep(target);
        Keep(march);
        Keep(mabi);
        return llvm::Error::success();
    }
    std::string supported;
    for (const CpuMapping& cpu : profile_.cpus) {
        supported += (supported.empty() ? "" : ", ") + cpu.mcpu;
    }
    return Fail(
        TranslationErrorKind::kUnsupportedCpu, kUnsupportedCpuMessage, llvm::StringRef(mcpu), profile_.name, supported);
}

llvm::Error CommandTranslation::TranslateArgument(std::size_t& index) {
    const std::string_view argument = arguments()[index];

    if (!argument.starts_with('-')) {
        return TranslateSource(argument);
    }
    if (const DroppedOption* option = FindExact(kDroppedFlags, argument)) {
        Remove(argument, option->reason);
        return llvm::Error::success();
    }
    if (const DroppedOption* option = FindPrefix(kDroppedPrefixes, argument)) {
        Remove(argument, option->reason);
        return llvm::Error::success();
    }
    if (const DroppedOption* option = FindExact(kDroppedWithOperand, argument)) {
        if (llvm::Error error = RequireOperand(index)) {
            return error;
        }
        Remove(std::string(argument) + " " + arguments()[index + 1], option->reason);
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
        if (llvm::Error error = RequireOperand(index)) {
            return error;
        }
        Keep(argument);
        Keep(arguments()[++index]);
        return llvm::Error::success();
    }
    if (Contains(kKeptFlags, argument) || HasKeptPrefix(argument)) {
        Keep(argument);
        return llvm::Error::success();
    }
    return Fail(TranslationErrorKind::kUnsupportedOption, kUnsupportedOptionMessage, llvm::StringRef(argument));
}

llvm::Expected<TranslationResult> CommandTranslation::Run() && {
    if (arguments().empty()) {
        return Fail(TranslationErrorKind::kMalformedCommand, kNoArgumentsMessage);
    }
    const std::string& compiler = arguments().front();
    if (llvm::sys::path::filename(compiler) != llvm::StringRef(kSfpiCompiler)) {
        return Fail(
            TranslationErrorKind::kUnsupportedCompiler,
            kUnsupportedCompilerMessage,
            llvm::StringRef(kSfpiCompiler),
            compiler);
    }
    for (const std::string& argument : arguments()) {
        needs_cxx20_ = needs_cxx20_ || Contains(kStandardBackports, argument);
    }

    result_.command.directory = context_.command().directory;
    result_.command.source_file = context_.command().source_file;
    result_.command.arguments.reserve(arguments().size());
    Replace(compiler, kClangDriver, kCompilerReplacedReason);
    Keep(kClangDriver);

    for (std::size_t index = 1; index < arguments().size(); ++index) {
        if (llvm::Error error = TranslateArgument(index)) {
            return std::move(error);
        }
    }
    if (!has_cpu_) {
        return Fail(TranslationErrorKind::kMalformedCommand, kMissingCpuMessage);
    }
    if (!has_source_) {
        return Fail(TranslationErrorKind::kMalformedCommand, kMissingSourceMessage, context_.command().source_file);
    }
    return std::move(result_);
}

}  // namespace

llvm::Expected<TranslationResult> TranslateSfpiCommand(const Context& context, const ToolchainProfile& profile) {
    return CommandTranslation(context, profile).Run();
}

}  // namespace tt::lint
