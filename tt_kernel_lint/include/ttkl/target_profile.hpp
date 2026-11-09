#pragma once

#include <string>
#include <vector>

namespace tt::lint {

/** How one SFPI `-mcpu=` value is spelled for Clang. */
struct CpuMapping {
    // only tt-wh-tensix temp
    std::string mcpu;
    // riscv32-unknown-elf
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
};

}  // namespace tt::lint
