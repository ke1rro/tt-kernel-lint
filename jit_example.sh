#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if (( $# > 1 )); then
    echo "Choose one mode. Use ./jit_example.sh --help." >&2
    exit 2
fi
mode=--jit-without-device
case "${1:-}" in
    --help|-h)
        echo "Usage: ./jit_example.sh [--jit-without-device | --jit-with-device]"
        echo "JIT without device (default): use TT-Metal's predefined Wormhole configuration."
        echo "JIT with device: open device 0 and use its initialized configuration."
        echo "Both build real device binaries without dispatching the example kernel."
        exit 0
        ;;
    --jit-without-device|--mock-compile-only|"") ;;
    --jit-with-device|--device|--compile-only)
        mode=--jit-with-device
        ;;
    *)
        echo "Unknown mode. Use ./jit_example.sh --help." >&2
        exit 2
        ;;
esac

cmake -S . -B build-kernels -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm -DClang_DIR=/usr/lib/llvm-20/lib/cmake/clang -DBUILD_KERNELS=ON -DTT_USE_SYSTEM_SFPI=OFF
python3 scripts/check_sfpi.py
cmake --build build-kernels --target example_kernel --parallel 8

env -u LD_LIBRARY_PATH -u TT_METAL_CCACHE_KERNEL_SUPPORT -u TT_METAL_SIMULATOR -u TT_METAL_JIT_SERVER_ENABLE TT_METAL_RUNTIME_ROOT="$PWD/tt-metal" TT_METAL_FORCE_JIT_COMPILE=1 TT_METAL_LOG_KERNELS_COMPILE_COMMANDS=1 TT_LOGGER_LEVEL=info CCACHE_DISABLE=1 ./build-kernels/example_kernel "$mode"
