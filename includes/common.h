#pragma once

#include <string_view>
#include <cstdint>

// COMMON.H DESCRIBES COMMON TYPES
// FROM api_spec.yaml

namespace tt_lint {

enum class ThreadKind : uint8_t {
    None            = 0,
    TRISC0_UNPACK   = 1 << 0, // 00001
    TRISC1_MATH     = 1 << 1, // 00010
    TRISC2_PACK     = 1 << 2, // 00100
    BRISC           = 1 << 3, // 01000
    NCRISC          = 1 << 4, // 10000
    AllCompute      = TRISC0_UNPACK | TRISC1_MATH | TRISC2_PACK,
    AllDataflow     = BRISC | NCRISC,
    Any             = 0xFF
};

constexpr inline ThreadKind operator|(ThreadKind a, ThreadKind b) {
    return static_cast<ThreadKind>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr inline bool operator&(ThreadKind a, ThreadKind b) {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

enum class State : uint8_t {
    ANY,
    UNINITIALIZED,
    CANONICAL_IDLE,
    MATH_IDLE,
    MATH_DST_HELD,
    PACK_IDLE,
    PACK_DST_HELD,
    CB_TILES_AVAILABLE,
    CB_SLOTS_RESERVED,
    NOC_READ_IN_FLIGHT,
    NOC_WRITE_IN_FLIGHT,
    IN_TILIZE_MODE,
    IN_UNTILIZE_MODE,
    IN_REDUCE_MODE,
    IN_UNARY_BCAST_MODE,
    IN_CUSTOM_MM_DENSE_MODE
};

enum class Category : uint8_t {
    Startup,
    Cleanup,
    RegLifecycle,
    CbLifecycle,
    DataflowNoc,
    Reconfig,
    Matmul,
    BinaryEltwise,
    BcastEltwise,
    Reduce,
    Tilize,
    Untilize,
    Pack,
    UnaryDatacopy,
    CustomMm,
    SfpuStateful,
    SfpuStateless
};

// may be useful for error messages
std::string_view toString(State state);
std::string_view toString(ThreadKind thread);

} // namespace tt_lint
