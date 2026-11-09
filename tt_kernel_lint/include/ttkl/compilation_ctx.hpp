#pragma once

#include <string>
#include <vector>
#include <utility>

namespace tt::lint {

    enum class ThreadRole{
        kUnpack = 0,
        kMath = 1,
        kPack = 2,
    };

    struct CompileCommand{
        std::string directory;
        std::string source_file;
        std::vector<std::string> arguments;
    };

    struct KernelId{
        std::string name;
        // JIT configuration key that distinguishes builds of the same kernel
        std::string variant;
        ThreadRole role;
    };

    class Context {
        KernelId kernel_;
        CompileCommand command_;
        public:
        Context(const KernelId& kernel, const CompileCommand& command) : kernel_(kernel), command_(command) {}

        [[nodiscard]] const KernelId& kernel() const {return kernel_;}
        [[nodiscard]] const CompileCommand& command() const {return command_;}
    };
};
