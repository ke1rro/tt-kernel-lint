#pragma once

#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include "ttkl/compilation_ctx.hpp"
#include "ttkl/sfpi_toolchain.hpp"

namespace tt::lint {

enum class NoteKind {
    kAdded,
    kRemoved,
    kReplaced,
};

/** One deliberate difference between the captured SFPI GCC command and the Clang command. */
struct TranslationNote {
    std::string original;
    std::string translated;
    std::string reason;
    NoteKind kind{};
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

/** The command cannot be translated with the given toolchain. */
class TranslationError final : public llvm::ErrorInfo<TranslationError> {
    std::string message_;
    TranslationErrorKind kind_;

public:
    static char ID;

    TranslationError(TranslationErrorKind kind, std::string message) : message_(std::move(message)), kind_(kind) {}

    [[nodiscard]] TranslationErrorKind kind() const noexcept { return kind_; }

    void log(llvm::raw_ostream& os) const override;
    [[nodiscard]] std::error_code convertToErrorCode() const override;
};

/**
 * Finds the SFPI GCC driver of a captured command, skipping a ccache launcher and its KEY=VALUE settings.
 *
 * @return A view into `command.arguments`.
 */
[[nodiscard]] llvm::Expected<std::string_view> FindSfpiCompiler(const CompileCommand& command);

/**
 * Translates a captured SFPI GCC command into a Clang command that parses the same translation unit.
 *
 * The input is not modified. Every option must be known: an option outside the verified Wormhole command set is an
 * error, not a pass-through. The result is a syntax-only command; patched SFPI headers come from
 * `toolchain.overlays()`.
 */
[[nodiscard]] llvm::Expected<TranslationResult> TranslateSfpiCommand(
    const Context& context, const SfpiToolchain& toolchain);

}  // namespace tt::lint
