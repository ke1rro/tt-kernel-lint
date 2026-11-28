# ttkl

Clang-based static analyzer for TT-Metal compute kernels.

The project combines TT-Metal's captured compiler commands with Clang to analyze
kernel code without running it on a device. `tt-init-check` finds operations such
as `add_tiles` that are missing their matching initializer.

## Clone

```bash
git clone --branch demo --recurse-submodules https://github.com/ke1rro/tt-kernel-lint.git
cd tt-kernel-lint
```

## Build

Requires CMake 4 or newer, Ninja, and the Clang/LLVM 20 development packages.
These commands use the Ubuntu LLVM installation at `/usr/lib/llvm-20`.

Build the analysis tools:

```bash
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_PREFIX_PATH=/usr/lib/llvm-20
cmake --build build --target tt-kernel-lint tt-init-check -j4
```

To also build the tile-add host executable, install TT-Metal's
[build dependencies](tt-metal/INSTALLING.md) and SFPI 7.90.0, build 1068, at
`/opt/tenstorrent/sfpi`. Both are already set up on the demo VM.

```bash
cmake -S . -B build -DBUILD_KERNELS=ON -DTT_USE_SYSTEM_SFPI=ON
cmake --build build --target example_kernel -j4
```

Build the Wormhole simulator if it is not already available. This requires
Python 3.9 or newer and a C++20-capable `g++`:

```bash
cd ttsim
./make.py src/_out/release_wh/libttsim.so
cp ../tt-metal/tt_metal/soc_descriptors/wormhole_b0_80_arch.yaml src/_out/release_wh/soc_descriptor.yaml
cd ..
```

## Demo run on example kernel

We prepared kernel for adding two tiles A and B.

A is filed with random numbers from 0.0 to 1.0 and B is filled with constants in our case 1.0. The kernel adds A and B and writes the result to A.

On the prepared VM, load the simulator settings and run the kernel:

```bash
source .env
./build/example_kernel
```

To demonstrate the missing initializer check, open the compute kernel:

```cpp
// Tell the SFPU that we will be using circular buffers c_in0, c_in1 and c_out0
    // to perform the computation.
    compute_kernel_hw_startup(cb_in0, cb_in1, cb_out0);
    // And we are going to add tiles. This function is only called if we ever need to
    // switch operation to something else. Since we are only adding tiles, this function
    // is only called once.
    add_init(cb_in0, cb_in1);

    cb_wait_front(cb_in0, 1);
    cb_wait_front(cb_in1, 1);
```

and comment `add_init`

Than run the checker:

```bash
source .env
./build/example_kernel
./build/tt-init-check --input-log "$TTKL_INPUT_LOG" --role math --quiet-sdk-warnings
```
