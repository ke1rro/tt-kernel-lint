#pragma once

#include "ttkl/kernel_api/common.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace tt::lint {

struct ExuTransition {
    ExuUnit unit{ExuUnit::kUnpack};
    std::span<const ExuModeId> require;
    ExuModeId set{ExuModeId::kNone};
    bool dirty{false};

    constexpr bool matches(ExuModeId current_mode, bool is_dirty) const {
        if (require.empty()) {
            return true;
        }
        for (ExuModeId req : require) {
            if (matchesExuRequirement(current_mode, is_dirty, req)) {
                return true;
            }
        }
        return false;
    }
};

struct ResourceTransition {
    ResourceKind resource{ResourceKind::kDstMath};
    ResourceState require{ResourceState::kNone};
    ResourceState set{ResourceState::kNone};

    constexpr bool matches(ResourceState current) const {
        return require == ResourceState::kNone || current == require;
    }
};

struct ArgBinding {
    ArgRole role{ArgRole::kIcb0};
    std::uint8_t index{0};
};

struct TArgBinding {
    TArgRole role{TArgRole::kSrcOrder};
    std::uint8_t index{0};
};

struct Instruction {
    std::string_view name;
    std::string_view header;
    InstructionKind kind{InstructionKind::kOp};
    ThreadKind threads{ThreadKind::kNone};
    ArchMask arch{ArchMask::kAll};
    std::span<const ExuTransition> exu;
    std::span<const ResourceTransition> resources;
    std::span<const std::string_view> uninits;
    std::span<const std::string_view> pairs;
    std::span<const ArgBinding> args;
    std::span<const TArgBinding> targs;
    bool stateful{false};
    bool in_loop{true};
    bool deprecated{false};
    bool experimental{false};

    constexpr bool supportsArch(ArchMask target) const { return hasArch(arch, target); }

    constexpr bool runsOnThread(ThreadKind thread) const { return hasThread(threads, thread); }

    constexpr bool marksAnyExuDirty() const {
        for (const ExuTransition& transition : exu) {
            if (transition.dirty) {
                return true;
            }
        }
        return false;
    }

    constexpr std::optional<std::uint8_t> findArgIndex(ArgRole role) const {
        for (const ArgBinding& binding : args) {
            if (binding.role == role) {
                return binding.index;
            }
        }
        return std::nullopt;
    }

    constexpr std::optional<std::uint8_t> findTArgIndex(TArgRole role) const {
        for (const TArgBinding& binding : targs) {
            if (binding.role == role) {
                return binding.index;
            }
        }
        return std::nullopt;
    }
};

}  // namespace tt::lint
