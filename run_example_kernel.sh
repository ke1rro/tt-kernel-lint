#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

export TT_METAL_FORCE_JIT_COMPILE=1
export CCACHE_DISABLE=1   # if kernel ccache is enabled
export TT_METAL_LOG_KERNELS_COMPILE_COMMANDS=1
export TT_LOGGER_LEVEL=info   # the compile-cmd lines are logged at info level

cmake -S . -B build-kernels -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm -DClang_DIR=/usr/lib/llvm-20/lib/cmake/clang -DBUILD_KERNELS=ON
cmake --build build-kernels --target example_kernel --parallel 8

env -u LD_LIBRARY_PATH TT_METAL_RUNTIME_ROOT="$PWD/tt-metal" ./build-kernels/example_kernel 2>&1 | tee ./tmp/run.log
