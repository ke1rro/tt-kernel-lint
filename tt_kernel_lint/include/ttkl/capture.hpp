#pragma once

#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include "ttkl/compilation_ctx.hpp"

namespace tt::lint {

/** Optional selection applied while reading a capture log. */
struct CaptureFilter {
    // <name> or <name>/<variant>; empty selects every kernel.
    std::string kernel;
    std::optional<ThreadRole> role;
};

/** Whether `kernel` passes `filter`. A trailing '/' in the requested name is ignored, as in FULL_KERNEL_NAME. */
[[nodiscard]] bool MatchesCaptureFilter(const KernelId& kernel, const CaptureFilter& filter);

/**
 * Reads TT-Metal JIT compiler commands from a log produced with
 * TT_METAL_LOG_KERNELS_COMPILE_COMMANDS=1.
 *
 * Only compute-kernel commands with COMPILE_FOR_TRISC are returned. Firmware
 * and data-movement commands are skipped.
 *
 * @return An error if the log cannot be read, a compile command line is malformed, or a compute kernel command has
 * missing or inconsistent role and name defines.
 */
[[nodiscard]] llvm::Expected<std::vector<Context>> ReadCaptureLog(llvm::StringRef path, const CaptureFilter& filter);

}  // namespace tt::lint
