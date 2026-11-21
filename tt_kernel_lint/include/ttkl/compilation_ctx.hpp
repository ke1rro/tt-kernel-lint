#pragma once

#include <array>
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

struct ThreadRoleEntry {
    ThreadRole role;
    std::string_view name;
};

inline constexpr std::array<ThreadRoleEntry, 3> kThreadRoles{{
    {ThreadRole::kUnpack, "unpack"},
    {ThreadRole::kMath, "math"},
    {ThreadRole::kPack, "pack"},
}};

[[nodiscard]] inline std::optional<ThreadRole> ParseThreadRole(std::string_view name) {
    for (const ThreadRoleEntry& entry : kThreadRoles) {
        if (entry.name == name) {
            return entry.role;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string_view ThreadRoleName(ThreadRole role) {
    for (const ThreadRoleEntry& entry : kThreadRoles) {
        if (entry.role == role) {
            return entry.name;
        }
    }
    return "unknown";
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
