#pragma once

#include <span>
#include <string_view>

namespace tt::lint {

/** How one SFPI `-mcpu=` value is spelled for Clang, and which SFPI GCC system headers it selects. */
struct CpuMapping {
    std::string_view mcpu;
    std::string_view triple;
    std::string_view march;
    std::string_view mabi;
    // Relative to the SFPI root, in the order the SFPI GCC driver searches them for this CPU's multilib.
    std::span<const std::string_view> system_include_directories;
};

/** A predefined macro that SFPI GCC defines differently from Clang for the same target. */
struct MacroOverride {
    // Includes the parameter list of a function-like macro, e.g. "__INT32_C(c)".
    std::string_view name;
    std::string_view value;
};

/** Expected SHA-256 digest of one file in the SFPI include directory. */
struct HeaderDigest {
    std::string_view file;
    std::string_view sha256;
};

/** An exact-text change to one SFPI header, applied in memory so that Clang resolves code as SFPI GCC does. */
struct HeaderPatch {
    std::string_view file;
    std::string_view original;
    std::string_view replacement;
    std::string_view reason;
};

/**
 * Everything the translator assumes about one verified SFPI release and architecture.
 *
 * @note Views refer to static storage; a profile outlives every command translated with it.
 */
struct ToolchainProfile {
    std::string_view name;
    std::string_view sfpi_version;
    // File name of the SFPI GCC driver, which lives in <sfpi root>/compiler/bin.
    std::string_view compiler;
    std::span<const CpuMapping> cpus;
    std::span<const MacroOverride> macro_overrides;
    std::span<const HeaderDigest> headers;
    std::span<const HeaderPatch> patches;
};

/** The pinned profile for Wormhole compute kernels built by TT-Metal with SFPI 7.90.0. */
[[nodiscard]] const ToolchainProfile& WormholeSfpiProfile() noexcept;

}  // namespace tt::lint
