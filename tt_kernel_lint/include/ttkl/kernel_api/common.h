#pragma once

#include <string_view>
#include <cstdint>

namespace tt_lint {

enum class ThreadKind : uint8_t {
    kNone = 0,
    kTrisc0Unpack = 1 << 0,  // 00001
    kTrisc1Math = 1 << 1,    // 00010
    kTrisc2Pack = 1 << 2,    // 00100
    kBrisc = 1 << 3,         // 01000
    kNcrisc = 1 << 4,        // 10000
    kAllCompute = kTrisc0Unpack | kTrisc1Math | kTrisc2Pack,
    kAllDataflow = kBrisc | kNcrisc,
    kAny = 0xFF
};

constexpr ThreadKind operator|(ThreadKind a, ThreadKind b) {
    return static_cast<ThreadKind>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr bool operator&(ThreadKind a, ThreadKind b) {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

enum class State : uint8_t {
    kAny,
    kUninitialized,
    kCanonicalIdle,
    kMathIdle,
    kMathDstHeld,
    kPackIdle,
    kPackDstHeld,
    kCbTilesAvailable,
    kCbSlotsReserved,
    kNocReadInFlight,
    kNocWriteInFlight,
    kInTilizeMode,
    kInUntilizeMode,
    kInReduceMode,
    kInUnaryBcastMode,
    kInCustomMmDenseMode
};

enum class Category : uint8_t {
    kStartup,
    kCleanup,
    kRegLifecycle,
    kCbLifecycle,
    kDataflowNoc,
    kReconfig,
    kMatmul,
    kBinaryEltwise,
    kBcastEltwise,
    kReduce,
    kTilize,
    kUntilize,
    kPack,
    kUnaryDatacopy,
    kCustomMm,
    kSfpuStateful,
    kSfpuStateless
};

}  // namespace tt_lint
