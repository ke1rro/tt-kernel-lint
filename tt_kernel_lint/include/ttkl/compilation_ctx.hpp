#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tt::lint {

/** TRISC thread of a Tensix compute kernel. */
enum class ThreadRole {
    kUnpack = 0,
    kMath = 1,
    kPack = 2,
};

/** Lowercase role name, as in the TT-Metal UCK_CHLKC_* defines. */
[[nodiscard]] constexpr std::string_view ThreadRoleName(ThreadRole role) noexcept {
    switch (role) {
        case ThreadRole::kUnpack: return "unpack";
        case ThreadRole::kMath: return "math";
        case ThreadRole::kPack: return "pack";
    }
    return "unknown";
}

/** The role named by ThreadRoleName(), or std::nullopt for any other name. */
[[nodiscard]] constexpr std::optional<ThreadRole> ParseThreadRole(std::string_view name) noexcept {
    for (const ThreadRole role : {ThreadRole::kUnpack, ThreadRole::kMath, ThreadRole::kPack}) {
        if (ThreadRoleName(role) == name) {
            return role;
        }
    }
    return std::nullopt;
}

/** One compiler invocation exactly as the TT-Metal JIT build ran it. */
struct CompileCommand {
    std::string directory;
    std::string source_file;
    std::vector<std::string> arguments;
};

/** Identity of one JIT compilation context. */
struct KernelId {
    std::string name;
    // JIT configuration key that distinguishes builds of the same kernel
    std::string variant;
    ThreadRole role{};
};

/** A compilation context as captured from TT-Metal, before any translation. Immutable. */
class Context final {
    KernelId kernel_;
    CompileCommand command_;

public:
    Context(KernelId kernel, CompileCommand command) : kernel_(std::move(kernel)), command_(std::move(command)) {}

    [[nodiscard]] const KernelId& kernel() const noexcept { return kernel_; }
    [[nodiscard]] const CompileCommand& command() const noexcept { return command_; }
};

}  // namespace tt::lint
