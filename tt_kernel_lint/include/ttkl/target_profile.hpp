#pragma once

#include <string>
#include <vector>

namespace tt::lint {

/** How one SFPI `-mcpu=` value is spelled for Clang. */
struct CpuMapping {
    // SFPI -mcpu value, for example tt-wh-tensix
    std::string mcpu;
    // Clang target triple, for example riscv32-unknown-elf
    std::string triple;
    std::string march;
    std::string mabi;
};

/** What the translator needs to know about one verified SFPI release and architecture. */
struct ToolchainProfile {
    std::string name;
    std::string sfpi_ver;
    std::vector<CpuMapping> cpus;
    std::vector<std::string> system_include_directories;
    // Extra Clang options appended after translation, for example -nostdinc++
    // and the SFPI type-model overrides.
    std::vector<std::string> clang_flags;
};

}  // namespace tt::lint
