#pragma once

#include <memory>
#include <span>

#include "llvm/Support/Error.h"

#include "ttkl/compilation_ctx.hpp"
#include "ttkl/sfpi_toolchain.hpp"

namespace clang {
class ASTUnit;
class DiagnosticConsumer;
namespace tooling {
class ToolAction;
}  // namespace tooling
}  // namespace clang

namespace tt::lint {

/**
 * Runs `action` on the translation unit of a translated Clang command, in this process.
 *
 * Relative paths resolve against `command.directory` through a file system private to this run, so the process
 * working directory never changes, and `overlays` replace files in memory. Nothing is written to disk.
 *
 * @param diagnostics Receives driver and parser diagnostics; nullptr prints them to stderr.
 * @return An error if the driver rejects the command or the action reports errors.
 */
[[nodiscard]] llvm::Error RunClangTool(
    const CompileCommand& command,
    std::span<const FileOverlay> overlays,
    clang::tooling::ToolAction& action,
    clang::DiagnosticConsumer* diagnostics = nullptr);

/**
 * Parses a translated Clang command into an AST, in this process.
 *
 * Paths and overlays are handled as by RunClangTool(). The AST is returned even when Clang reports errors.
 *
 * @param diagnostics Receives driver and parser diagnostics; nullptr prints them to stderr. Must outlive the AST.
 * @return An error if the driver rejects the command.
 */
[[nodiscard]] llvm::Expected<std::unique_ptr<clang::ASTUnit>> BuildAst(
    const CompileCommand& command,
    std::span<const FileOverlay> overlays,
    clang::DiagnosticConsumer* diagnostics = nullptr);

}  // namespace tt::lint
