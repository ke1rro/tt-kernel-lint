#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tt::lint {

enum class InstructionKind : std::uint8_t {
    kStartup = 0,
    kCleanup,
    kReconfig,
    kInit,
    kOp,
    kUninit,
    kAcquire,
    kRelease,
};

constexpr std::string_view toString(InstructionKind kind) {
    switch (kind) {
        case InstructionKind::kStartup: return "startup";
        case InstructionKind::kCleanup: return "cleanup";
        case InstructionKind::kReconfig: return "reconfig";
        case InstructionKind::kInit: return "init";
        case InstructionKind::kOp: return "op";
        case InstructionKind::kUninit: return "uninit";
        case InstructionKind::kAcquire: return "acquire";
        case InstructionKind::kRelease: return "release";
    }
    return "unknown";
}

enum class ThreadKind : std::uint8_t {
    kNone = 0,
    kUnpack = 1U << 0U,
    kMath = 1U << 1U,
    kPack = 1U << 2U,
    kDm = 1U << 3U,
    kAllCompute = kUnpack | kMath | kPack,
    kAll = kAllCompute | kDm,
};

constexpr ThreadKind operator|(ThreadKind a, ThreadKind b) {
    return static_cast<ThreadKind>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr ThreadKind operator&(ThreadKind a, ThreadKind b) {
    return static_cast<ThreadKind>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

constexpr bool hasThread(ThreadKind mask, ThreadKind flag) { return (mask & flag) != ThreadKind::kNone; }

enum class ArchMask : std::uint8_t {
    kNone = 0,
    kWormhole = 1U << 0U,
    kBlackhole = 1U << 1U,
    kQuasar = 1U << 2U,
    kAll = kWormhole | kBlackhole | kQuasar,
};

constexpr ArchMask operator|(ArchMask a, ArchMask b) {
    return static_cast<ArchMask>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr ArchMask operator&(ArchMask a, ArchMask b) {
    return static_cast<ArchMask>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

constexpr bool hasArch(ArchMask mask, ArchMask arch) { return (mask & arch) != ArchMask::kNone; }

enum class ExuUnit : std::uint8_t {
    kUnpack = 0,
    kFpu,
    kSfpu,
    kPack,
    kSfpuPack,
    kNocRdState,
    kNocWrState,
    kNocInlineState,
    kCount,
};

inline constexpr std::size_t kExuUnitCount{static_cast<std::size_t>(ExuUnit::kCount)};

constexpr std::string_view toString(ExuUnit unit) {
    switch (unit) {
        case ExuUnit::kUnpack: return "unpack";
        case ExuUnit::kFpu: return "fpu";
        case ExuUnit::kSfpu: return "sfpu";
        case ExuUnit::kPack: return "pack";
        case ExuUnit::kSfpuPack: return "sfpu_pack";
        case ExuUnit::kNocRdState: return "noc_rd_state";
        case ExuUnit::kNocWrState: return "noc_wr_state";
        case ExuUnit::kNocInlineState: return "noc_inline_state";
        case ExuUnit::kCount: break;
    }
    return "unknown";
}

constexpr bool isComputeExu(ExuUnit unit) {
    switch (unit) {
        case ExuUnit::kUnpack:
        case ExuUnit::kFpu:
        case ExuUnit::kSfpu:
        case ExuUnit::kPack:
        case ExuUnit::kSfpuPack: return true;
        case ExuUnit::kNocRdState:
        case ExuUnit::kNocWrState:
        case ExuUnit::kNocInlineState:
        case ExuUnit::kCount: return false;
    }
    return false;
}

/// Interned identifier for an EXU mode or meta-requirement.
/// IDs 0..4 are fixed across all specifications; concrete mode IDs (>= kFirstConcrete)
/// are generated in rules_db.h together with exuModeName().
enum class ExuModeId : std::uint16_t {
    kNone = 0,           // No transition (`set` omitted).
    kUninit = 1,         // Uninitialized (before compute hw startup or after cleanup poison).
    kIdle = 2,           // Clean baseline after startup or uninit.
    kClean = 3,          // Meta-requirement: mode != kUninit && !dirty.
    kStarted = 4,        // Meta-requirement: mode != kUninit (clean or dirty).
    kFirstConcrete = 5,  // First generated concrete mode ID.
};

constexpr ExuModeId initialExuMode(ExuUnit unit) { return isComputeExu(unit) ? ExuModeId::kUninit : ExuModeId::kIdle; }

constexpr bool isMetaExuRequirement(ExuModeId mode) { return mode == ExuModeId::kClean || mode == ExuModeId::kStarted; }

constexpr bool matchesExuRequirement(ExuModeId current_mode, bool is_dirty, ExuModeId required) {
    switch (required) {
        case ExuModeId::kNone: return true;
        case ExuModeId::kUninit: return current_mode == ExuModeId::kUninit;
        case ExuModeId::kStarted: return current_mode != ExuModeId::kUninit;
        case ExuModeId::kClean: return current_mode != ExuModeId::kUninit && !is_dirty;
        default: return current_mode == required;
    }
}

enum class ResourceKind : std::uint8_t {
    kDstMath = 0,
    kDstPack,
    kDestAcc,
    kCbIn,
    kCbOut,
    kNocRd,
    kNocWr,
    kNocAtomic,
    kCount,
};

inline constexpr std::size_t kResourceKindCount{static_cast<std::size_t>(ResourceKind::kCount)};

constexpr std::string_view toString(ResourceKind resource) {
    switch (resource) {
        case ResourceKind::kDstMath: return "dst_math";
        case ResourceKind::kDstPack: return "dst_pack";
        case ResourceKind::kDestAcc: return "dest_acc";
        case ResourceKind::kCbIn: return "cb_in";
        case ResourceKind::kCbOut: return "cb_out";
        case ResourceKind::kNocRd: return "noc_rd";
        case ResourceKind::kNocWr: return "noc_wr";
        case ResourceKind::kNocAtomic: return "noc_atomic";
        case ResourceKind::kCount: break;
    }
    return "unknown";
}

enum class ResourceState : std::uint8_t {
    kNone = 0,
    kFree,
    kHeld,
    kDefault,
    kOverridden,
    kIdle,
    kReady,
    kReserved,
    kInFlight,
};

constexpr std::string_view toString(ResourceState state) {
    switch (state) {
        case ResourceState::kNone: return "none";
        case ResourceState::kFree: return "free";
        case ResourceState::kHeld: return "held";
        case ResourceState::kDefault: return "default";
        case ResourceState::kOverridden: return "overridden";
        case ResourceState::kIdle: return "idle";
        case ResourceState::kReady: return "ready";
        case ResourceState::kReserved: return "reserved";
        case ResourceState::kInFlight: return "in_flight";
    }
    return "unknown";
}

constexpr ResourceState initialResourceState(ResourceKind resource) {
    switch (resource) {
        case ResourceKind::kDstMath:
        case ResourceKind::kDstPack: return ResourceState::kFree;
        case ResourceKind::kDestAcc: return ResourceState::kDefault;
        case ResourceKind::kCbIn:
        case ResourceKind::kCbOut:
        case ResourceKind::kNocRd:
        case ResourceKind::kNocWr:
        case ResourceKind::kNocAtomic: return ResourceState::kIdle;
        case ResourceKind::kCount: break;
    }
    return ResourceState::kNone;
}

constexpr bool isPerKeyResource(ResourceKind resource) {
    switch (resource) {
        case ResourceKind::kCbIn:
        case ResourceKind::kCbOut:
        case ResourceKind::kNocRd:
        case ResourceKind::kNocWr:
        case ResourceKind::kNocAtomic: return true;
        case ResourceKind::kDstMath:
        case ResourceKind::kDstPack:
        case ResourceKind::kDestAcc:
        case ResourceKind::kCount: return false;
    }
    return false;
}

enum class ArgRole : std::uint8_t {
    kBlock = 0,
    kCb,
    kCtDim,
    kIcb0,
    kIcb1,
    kIdst,
    kIdst0,
    kIdst1,
    kIdst2,
    kKtDim,
    kNoc,
    kNtiles,
    kOcb,
    kOdst,
    kRtDim,
    kTranspose,
    kTrid,
};

constexpr std::string_view toString(ArgRole role) {
    switch (role) {
        case ArgRole::kBlock: return "block";
        case ArgRole::kCb: return "cb";
        case ArgRole::kCtDim: return "ct_dim";
        case ArgRole::kIcb0: return "icb0";
        case ArgRole::kIcb1: return "icb1";
        case ArgRole::kIdst: return "idst";
        case ArgRole::kIdst0: return "idst0";
        case ArgRole::kIdst1: return "idst1";
        case ArgRole::kIdst2: return "idst2";
        case ArgRole::kKtDim: return "kt_dim";
        case ArgRole::kNoc: return "noc";
        case ArgRole::kNtiles: return "ntiles";
        case ArgRole::kOcb: return "ocb";
        case ArgRole::kOdst: return "odst";
        case ArgRole::kRtDim: return "rt_dim";
        case ArgRole::kTranspose: return "transpose";
        case ArgRole::kTrid: return "trid";
    }
    return "unknown";
}

enum class TArgRole : std::uint8_t {
    kApprox = 0,
    kBcastDim,
    kBcastOp,
    kBcastType,
    kBlockCtDim,
    kCtDim,
    kDense,
    kEltwiseBinaryType,
    kEnable,
    kFullCtDim,
    kInputClamping,
    kIs32Bit,
    kLayout,
    kNarrowRow,
    kNormalize,
    kReduceDim,
    kReduceType,
    kRespectTrigger,
    kReuseDest,
    kRowNumDatums,
    kSrcOrder,
    kTileDimReconfig,
    kTranspose,
    kTransposeOfFaces,
};

constexpr std::string_view toString(TArgRole role) {
    switch (role) {
        case TArgRole::kApprox: return "approx";
        case TArgRole::kBcastDim: return "bcast_dim";
        case TArgRole::kBcastOp: return "bcast_op";
        case TArgRole::kBcastType: return "bcast_type";
        case TArgRole::kBlockCtDim: return "block_ct_dim";
        case TArgRole::kCtDim: return "ct_dim";
        case TArgRole::kDense: return "dense";
        case TArgRole::kEltwiseBinaryType: return "eltwise_binary_type";
        case TArgRole::kEnable: return "enable";
        case TArgRole::kFullCtDim: return "full_ct_dim";
        case TArgRole::kInputClamping: return "input_clamping";
        case TArgRole::kIs32Bit: return "is_32bit";
        case TArgRole::kLayout: return "layout";
        case TArgRole::kNarrowRow: return "narrow_row";
        case TArgRole::kNormalize: return "normalize";
        case TArgRole::kReduceDim: return "reduce_dim";
        case TArgRole::kReduceType: return "reduce_type";
        case TArgRole::kRespectTrigger: return "respect_trigger";
        case TArgRole::kReuseDest: return "reuse_dest";
        case TArgRole::kRowNumDatums: return "row_num_datums";
        case TArgRole::kSrcOrder: return "src_order";
        case TArgRole::kTileDimReconfig: return "tile_dim_reconfig";
        case TArgRole::kTranspose: return "transpose";
        case TArgRole::kTransposeOfFaces: return "transpose_of_faces";
    }
    return "unknown";
}

}  // namespace tt::lint
