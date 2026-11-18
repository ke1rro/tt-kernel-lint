#pragma once

#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include "ttkl/compilation_ctx.hpp"
#include "ttkl/target_profile.hpp"

namespace tt::lint {

enum class NoteKind {
    kAdded,
    kRemoved,
    kReplaced,
};

/** One deliberate difference between the captured SFPI GCC command and the Clang command. */
struct TranslationNote {
    NoteKind kind{};
    std::string original;
    std::string translated;
    std::string reason;
};

/** A translated command together with the list of changes made to it. */
struct TranslationResult {
    CompileCommand command;
    std::vector<TranslationNote> notes;
};

enum class TranslationErrorKind {
    kMalformedCommand,
    kUnsupportedCompiler,
    kUnsupportedCpu,
    kUnsupportedOption,
};

/** The command cannot be translated with the given profile. */
class TranslationError final : public llvm::ErrorInfo<TranslationError> {
    TranslationErrorKind kind_;
    std::string message_;

public:
    static char ID;

    TranslationError(TranslationErrorKind kind, std::string message) : kind_(kind), message_(std::move(message)) {}

    [[nodiscard]] TranslationErrorKind kind() const noexcept { return kind_; }

    void log(llvm::raw_ostream& os) const override;
    [[nodiscard]] std::error_code convertToErrorCode() const override;
};

/**
 * Translates a captured SFPI GCC command into a Clang command. The input is not modified.
 *
 * Every option must be known: an option outside the verified Wormhole command set is an error, not a pass-through.
 */
[[nodiscard]] llvm::Expected<TranslationResult> TranslateSfpiCommand(
    const Context& context, const ToolchainProfile& profile);

}  // namespace tt::lint
