#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if (( $# > 0 )); then
    if (( $# == 1 )) && [[ "$1" == --help || "$1" == -h ]]; then
        echo "Usage: ./run_example_kernel.sh"
        echo "Build, execute and check the example on physical device 0."
        echo "For JIT without device or JIT with device, use ./jit_example.sh."
        exit 0
    fi
    echo "This script executes on device. Use ./jit_example.sh for JIT preparation." >&2
    exit 2
fi

cmake -S . -B build-kernels -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm -DClang_DIR=/usr/lib/llvm-20/lib/cmake/clang -DBUILD_KERNELS=ON -DTT_USE_SYSTEM_SFPI=OFF
python3 scripts/check_sfpi.py
cmake --build build-kernels --target example_kernel --parallel 8

env -u LD_LIBRARY_PATH TT_METAL_RUNTIME_ROOT="$PWD/tt-metal" ./build-kernels/example_kernel
