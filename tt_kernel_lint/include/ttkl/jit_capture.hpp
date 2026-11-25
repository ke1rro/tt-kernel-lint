#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "llvm/Support/Error.h"

#include "ttkl/compilation_ctx.hpp"

namespace tt::lint {

/**
 * Reads the compile commands from a TT-Metal log written with TT_METAL_LOG_KERNELS_COMPILE_COMMANDS=1 and
 * TT_LOGGER_LEVEL=info. A command logged more than once is returned once, in log order.
 *
 * @note TT-Metal joins argv with single spaces and no quoting, so an argument that contains a space cannot be
 * recovered. A split quoted value is rejected here; any other split leaves a stray positional argument that fails
 * translation. A split whose second half itself looks like an option is not detectable from the log.
 * @return An error for a "compile cmd" line without the "-c -o <object> <source>" sequence that TT-Metal emits, or with
 * an argument whose double quotes do not pair up.
 */
[[nodiscard]] llvm::Expected<std::vector<CompileCommand>> ParseJitBuildLog(std::string_view log);

/**
 * Identifies the compute kernel compilation context of a captured command.
 *
 * The role comes from COMPILE_FOR_TRISC and must agree with UCK_CHLKC_*; the name and variant come from
 * FULL_KERNEL_NAME.
 *
 * @return std::nullopt for a command that does not compile a compute kernel, such as firmware or a data movement
 * kernel, and an error for a compute kernel command whose defines are missing or inconsistent.
 */
[[nodiscard]] llvm::Expected<std::optional<Context>> IdentifyComputeContext(CompileCommand command);

}  // namespace tt::lint
