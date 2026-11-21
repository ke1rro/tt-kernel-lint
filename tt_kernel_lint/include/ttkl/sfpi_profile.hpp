#pragma once

#include "llvm/ADT/StringRef.h"

#include "ttkl/target_profile.hpp"

namespace tt::lint {

/**
 * Builds the translation profile for one captured SFPI compiler and CPU.
 *
 * The SFPI include directories are derived from the compiler path captured in
 * the JIT log. If the path does not point at a complete SFPI installation, the
 * profile still contains the CPU mapping but no SFPI system includes.
 */
[[nodiscard]] ToolchainProfile MakeSfpiProfile(llvm::StringRef compiler, llvm::StringRef mcpu);

}  // namespace tt::lint
