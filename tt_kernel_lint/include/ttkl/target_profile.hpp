#pragma once

#include <string>
#include <vector>


namespace tt::lint {

    struct CpuMapping{
        // only tt-wh-tensix temp
        std::string mcpu;
        // riscv32-unknown-elf
        std::string triple;
        std::string march;
        std::string mabi;
    };

    struct ToolchainProfile {
        std::string name;
        std::string sfpi_ver;
        std::vector<CpuMapping> cpus;
        std::vector<std::string> system_include_directories;
    };
};
