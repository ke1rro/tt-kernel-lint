# Guide about compilation and AST parse using CLI

I used [add_compute.cpp](../example_kernel/device/compute/add_compute.cpp) to add two 32 by 32 tiles
using TT-Metal compute APIs. Here I capture its JIT commands and generate a Clang AST of the resolved calls.

Supported options of work:

| Mode                     | When to use it                                                                                             |
| ------------------------ | ---------------------------------------------------------------------------------------------------------- |
| JIT without device       | Uses TT-Metal's predefined Wormhole N150 configuration without opening hardware. This is the default.      |
| JIT with device          | Opens and initializes device 0 and uses its actual configuration. It does not dispatch the example kernel. |
| Offline Clang AST replay | Parses an existing capture without running the host application or opening a device.                       |

Both modes use real SFPI to generate wrappers, configuration headers and device binaries.
JIT without device uses TT-Metal's upstream mock configuration. Configuration inputs can differ.

Here is example diagram of program lifecycle:

```text
example_kernel/main.cpp       CMake/Ninja host compilation
        |
        v
CreateKernel(...)             register sources and configurations
        |
        v
TT-Metal JIT build            during program preparation
        |
        +--> generated headers
        |      defines_generated.h
        |      chlkc_descriptors.h
        |
        +--> generated compute wrappers
        |      chlkc_unpack.cpp
        |      chlkc_math.cpp
        |      chlkc_pack.cpp
        |
        v
SFPI GCC compilation          trisck.cc with each role's flags
        |
        +--> TRISC0  UNPACK
        +--> TRISC1  MATH
        +--> TRISC2  PACK
        |
        v
Device linking                objects + linker script + firmware symbols
        |
        v
Device execution              firmware enters the loaded kernel images
```

## 1. Run JIT and capture compiler commands

Run from the repository root in Bash with the TT-Metal submodule initialized.
Have Clang 20, LLVM development packages, CMake 4 or newer, Ninja and Python available.

```bash
export CAPTURE="$PWD/tt-kernel-lint-captures/jit-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$CAPTURE"
export TT_METAL_CACHE="$CAPTURE"
set -o pipefail
./jit_example.sh --jit-without-device 2>&1 | tee "$CAPTURE/jit.log"
```

The script builds the host application, checks SFPI 7.90.0 build 1068, and runs JIT preparation.
It forces compilation, disables ccache and enables compiler logging. `tee` saves the log.
`pipefail` preserves failures, and `TT_METAL_CACHE` keeps generated files inside `CAPTURE`.
Expect exit status 0 and `JIT without device completed`.

To get configuration from a physical board instead, replace the last command with:

```bash
./jit_example.sh --jit-with-device 2>&1 | tee "$CAPTURE/jit.log"
```

Expect `JIT with device completed`. Both modes stop before example buffer
transfers and kernel dispatch. `run_example_kernel.sh` is for actual execution.

## 2. Find the MATH command and translate it for Clang

```bash
rg 'g\+\+ compile cmd:' "$CAPTURE/jit.log" | rg 'UCK_CHLKC_MATH' | rg 'add_compute/'
python3 tt-metal/scripts/build_kernel_clang_tidy_commands.py --input-log "$CAPTURE/jit.log" --output-dir "$CAPTURE/clang" --dedupe none --clang clang++-20
```

The first command finds the `add_compute` MATH/TRISC1 invocation. The translator reads
`--input-log`, writes to `--output-dir`, keeps contexts with `--dedupe none`, and selects
Clang 20 with `--clang`. Require a nonzero entry count. Commands with the same working
directory and source can still be merged by this pinned translator.

The result is `$CAPTURE/clang/compile_commands.json`, a list of source files, arguments
and working directories. It keeps `trisck.cc` as the main translation unit, meaning
that source file plus everything it includes. Its generated MATH wrapper `chlkc_math.cpp`
includes `add_compute.cpp`. Parsing `add_compute.cpp` directly would miss the role defines,
generated configuration and startup context.

## 3. Create the SFPI compatibility overlay

SFPI is the GCC based compiler used for device code. Its 7.90.0 headers have two
problems with Clang. SFPI builtin declarations reference the `__xtt_vector8` vector type,
but the fallback for other compilers only declares smaller vector types. A separate header
supplies the missing declaration before parsing. It does not replace any SFPI operations.

Clang also sees two implicit paths from `vMag` to `vInt`: a constructor and an inherited
conversion operator. The conversion is ambiguous, so we make the copied constructor explicit:

```diff
-sfpi_inline vInt (impl_::vMag);
+sfpi_inline explicit vInt (impl_::vMag);
```

This removes the constructor from implicit copy initialization, allowing the inherited operator.
It changes overload resolution and is not verified as equivalent to SFPI GCC behavior.

```bash
python3 scripts/check_sfpi.py --output-dir "$CAPTURE/sfpi-clang" > "$CAPTURE/sfpi-check-overlay.json"
```

`--output-dir` creates an overlay with `include/`, the vector header `sfpi_clang_compat.hpp`
and a hash manifest. Original header hashes are verified and the installed SDK stays unchanged.
Unsupported versions, modified inputs and an existing destination directory are rejected.

## 4. Generate the AST

```bash
python3 scripts/parse_ast.py --capture "$CAPTURE" --kernel add_compute --role math --compat-header "$CAPTURE/sfpi-clang/sfpi_clang_compat.hpp" --sfpi-overlay "$CAPTURE/sfpi-clang/include" --ast-filter kernel_main
```

The script checks C++ syntax and resolves calls, then dumps the AST if parsing succeeds.
Require `Exit code: 0` and `AST exit code: 0`. My capture produced 13,607 bytes, but size can vary.

| Flag              | Meaning                                                                             |
| ----------------- | ----------------------------------------------------------------------------------- |
| `--capture`       | Capture directory. The default database is `CAPTURE/clang/compile_commands.json`.   |
| `--kernel`        | Kernel name or exact `NAME/VARIANT`. Ambiguous contexts are rejected.               |
| `--role`          | `math`, `unpack` or `pack`. This example checks `math`, or TRISC1.                  |
| `--compat-header` | Header included before parsing, supplying `__xtt_vector8`.                          |
| `--sfpi-overlay`  | Replacement SFPI public include directory for Clang analysis.                       |
| `--ast-filter`    | Prints declarations matching this name. The whole translation unit is still parsed. |
| `--database`      | Optional path to a different compilation database.                                  |
| `--output-dir`    | Optional result directory. Default is `CAPTURE/replay`.                             |

## 5. Inspect the output

```bash
cat "$CAPTURE/replay/replay.json"
cat "$CAPTURE/replay/syntax.stderr"
cat "$CAPTURE/replay/ast.stderr"
rg -n 'FunctionDecl.*kernel_main|CallExpr|DeclRefExpr.*(compute_kernel_hw_startup|add_init|add_tiles)|CXXDefaultArgExpr' "$CAPTURE/replay/ast.stdout"
less "$CAPTURE/replay/ast.stdout"
```

`replay.json` records the selected command and statuses. Diagnostics go to `syntax.stderr`
and `ast.stderr`. The AST is in `ast.stdout`. Old outputs are cleared before each attempt.
The `*.command.json` files record the exact arguments used for each stage.

Here is a real `add_init` fragment, with addresses, paths and unrelated nodes shortened using `...`:

```text
FunctionDecl ... <.../add_compute.cpp:6:1, line:47:1> line:6:6 used kernel_main 'void ()'
`-CompoundStmt ... <col:20, line:47:1>
  ...
  |-CallExpr ... <line:24:5, col:28> 'void'
  | |-ImplicitCastExpr ... <col:5> 'void (*)(std::uint32_t, std::uint32_t, bool, std::uint32_t)' <FunctionToPointerDecay>
  | | `-DeclRefExpr ... <col:5> 'void (std::uint32_t, std::uint32_t, bool, std::uint32_t)' lvalue Function ... 'add_init' 'void (std::uint32_t, std::uint32_t, bool, std::uint32_t)'
  ...
  | |-CXXDefaultArgExpr ... <<invalid sloc>> 'bool'
  | | `-CXXBoolLiteralExpr ... <...> 'bool' false
  | `-CXXDefaultArgExpr ... <<invalid sloc>> 'std::uint32_t':'unsigned long' has rewritten init
```

`FunctionDecl` describes `kernel_main`, and `CallExpr` is its call to `add_init`.
`DeclRefExpr` identifies the function declaration Clang resolved. `CXXDefaultArgExpr`
represents omitted arguments. We wrote two arguments to `add_init`, but it has four parameters.
The defaults supply `acc_to_dest = false` and `call_line = __builtin_LINE()`.

For offline replay, keep all referenced files at their recorded paths and rerun step 4.
