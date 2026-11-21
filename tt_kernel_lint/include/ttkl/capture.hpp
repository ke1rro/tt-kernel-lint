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
    std::string kernel;
    std::optional<ThreadRole> role;
};

/**
 * Reads TT-Metal JIT compiler commands from a log produced with
 * TT_METAL_LOG_KERNELS_COMPILE_COMMANDS=1.
 *
 * Only compute-kernel commands with COMPILE_FOR_TRISC are returned. Firmware
 * and data-movement commands are skipped.
 */
[[nodiscard]] llvm::Expected<std::vector<Context>> ReadCaptureLog(llvm::StringRef path, const CaptureFilter& filter);

}  // namespace tt::lint
