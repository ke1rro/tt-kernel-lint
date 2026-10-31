# Replaying a real TT-Metal JIT compilation through Clang

## Motivation and scope

On October 31, 2026, we captured a real Wormhole B0 JIT build of
`example_kernel`, translated its SFPI GCC commands for Clang 20, and parsed the
MATH translation unit. The resulting AST resolves the declarations called by
`compute_kernel_hw_startup`, `add_init`, and `add_tiles`. This establishes a
frontend baseline for later analysis of compute API usage. It does not yet
extract evaluated argument values or check LLK initialization protocols.

The experiment used this environment:

```text
Device architecture: wormhole_b0
SFPI version:        7.90.0 (build 1068)
SFPI GCC:            riscv-tt-elf-g++ 15.1.0
Clang frontend:      clang++-20 (20.1.8)
TT-Metal submodule:  07fac6de536ed428e55db2a700812086bc763ba0
```

The local capture includes `jit.log`, `clang/compile_commands.json`, syntax
and AST output, a compatibility header, and a modified copy of SFPI's include
directory. These are experimental files, not an SDK distributed with this
project. The original replay was in `main.py`. We migrated that implementation
to [scripts/parse_ast.py](../scripts/parse_ast.py), replacing its hardcoded JIT
hash with explicit context selection and adding failure reporting.

## How TT-Metal constructs the device translation unit

The host program in [example_kernel/main.cpp](../example_kernel/main.cpp)
allocates one input tile for A, one for B, and one output tile. A contains
random bfloat16 values, while B contains ones. The reader brings both tiles
into circular buffers, compute adds them, and the writer stores the result in
DRAM. The three device sources perform one tile operation without device loops.

`CreateKernel()` registers source paths, placement, processor configuration,
and compile time arguments in a `Program`. It does not compile each device
source as an ordinary host C++ file. When Metalium prepares the program for
execution, its JIT builds the required processor images from that configuration.
The host passes tensor accessor metadata to reader and writer, selects core
`{0, 0}`, and requests `MathFidelity::HiFi4` for compute.

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

This is the Wormhole compute branch. Reader and writer have separate data
movement builds. The same user compute source feeds all three TRISC builds
because Compute API and LLK headers select different implementations through
role macros. UNPACK moves operands into the compute machinery, MATH performs
arithmetic, and PACK moves results toward the output circular buffer. A call
visible in the source can therefore have different implementation bodies in
each role. Parsing only MATH does not establish coverage of UNPACK or PACK.

There are several different compilations in this flow. CMake builds the host
executable against Metalium. Device firmware compilation builds persistent
runtime code, using `trisc.cc` for these compute processors. User kernel JIT
compilation uses `trisck.cc`, despite that file also living under
`hw/firmware/src/`. The [HAL source selection](../tt-metal/tt_metal/llrt/hal/tt-1xx/hal_1xx_common.cpp)
separates these cases by `is_fw`. Device linking produces processor ELF images
with the appropriate linker script and, for this kernel path, symbols from a
weakened firmware ELF. Clang frontend replay is separate from all of these. It
performs preprocessing, parsing, and semantic analysis without generating or
linking a device binary.

In [build.cpp](../tt-metal/tt_metal/jit_build/build.cpp), the build state gathers
HAL flags, includes, role and architecture defines, and kernel settings.
`export_target_recipe()` supplies kernel identity and compile time arguments.
`compile_one()` adds a force included PCH umbrella and passes an argument vector
to SFPI GCC through `build_gpp_argv()`. This is the vector printed as
`g++ compile cmd:`. Link commands are logged separately and are not frontend
replay inputs.

[genfiles.cpp](../tt-metal/tt_metal/jit_build/genfiles.cpp) supplies the context
that cannot be recovered from the user `.cpp` alone. `build_trisc_prolog()`
creates a role macro and includes generated defines.
`get_kernel_source_to_include()` adds a forward declaration and includes the
user source. The generator writes one wrapper per role. It also emits
`defines_generated.h` from kernel defines and `chlkc_descriptors.h` with data
formats, tile dimensions, math fidelity, approximation settings, and related
compile time configuration. This example uses a handwritten `kernel_main()`,
so it does not need the entry shim used for `TT_KERNEL` sources.

The actual MATH include chain and the replay inputs are:

```text
Captured directory: .../kernels/add_compute/<variant>/trisc1/
Captured argv:      role flags, -I., -I.., target, -include PCH umbrella
                            |
                            v
trisck.cc  -------------------------------> Clang frontend
    |                                           ^
    +--> defines_generated.h                    |
    +--> chlkc_list.h                            +-- compatibility header
             |                                  +-- SFPI include overlay
             +--> chlkc_descriptors.h            |
             +--> chlkc_math.cpp                 v
                      |                    syntax check, then AST dump
                      +--> defines_generated.h
                      +--> add_compute.cpp
                               |
                               +--> kernel_main()
```

[`chlkc_list.h`](../tt-metal/tt_metal/hw/ckernels/wormhole_b0/metal/common/chlkc_list.h)
is a repository header, not generated glue. `UCK_CHLKC_MATH` makes it include
the generated MATH wrapper. It also defines `run_kernel()`, which calls
`kernel_main()`. [`trisck.cc`](../tt-metal/tt_metal/hw/firmware/src/tt-1xx/trisck.cc)
provides the kernel image entry point, globals, startup work, and the call to
`run_kernel()`. The source location of a user call is therefore inside an
included `.cpp`, not the main file of the translation unit.

## Capturing the real JIT compilation

Start by recording the exact checkout and frontend version:

```bash
cd ~/tt-kernel-lint
git submodule update --init --recursive
git rev-parse HEAD
git -C tt-metal rev-parse HEAD
clang++-20 --version
```

The host repository revision identifies the example and replay tool. The
submodule revision fixes the generator, device APIs, and translator. The Clang
version matters because header acceptance and overload diagnostics can change.
Keep these outputs with the capture when reproducing the experiment.

We initially stored experimental output outside the source checkout:

```bash
CAPTURE="$HOME/tt-kernel-lint-captures/example"
mkdir -p "$CAPTURE"
```

That directory was later moved into the repository working directory. For the
available local artifacts the variable is now:

```bash
CAPTURE="$(pwd)/tt-kernel-lint-captures/example"
```

This is a local experimental path, not a required installation layout. Keeping
logs and modified vendor headers in a capture directory avoids confusing them
with project sources or an upstream SDK fix.

Enable compilation logging before running the host program:

```bash
export TT_METAL_FORCE_JIT_COMPILE=1
export TT_METAL_LOG_KERNELS_COMPILE_COMMANDS=1
export TT_LOGGER_LEVEL=info
export CCACHE_DISABLE=1
unset TT_METAL_CCACHE_KERNEL_SUPPORT
```

`TT_METAL_FORCE_JIT_COMPILE` defeats Metalium's JIT reuse so that compilation
actually occurs. `TT_METAL_LOG_KERNELS_COMPILE_COMMANDS` requests the compile
and link command logs, while `TT_LOGGER_LEVEL=info` allows those messages to be
printed. Compiler ccache is a separate layer. Unsetting
`TT_METAL_CCACHE_KERNEL_SUPPORT` removes the optional kernel ccache wrapper,
and `CCACHE_DISABLE=1` disables ccache if it is still invoked through another
path. A successful device run can otherwise reuse images without emitting all
the commands needed for a complete capture.

Use the existing host entry point:

```bash
set -o pipefail
./run_example_kernel.sh 2>&1 | tee "$CAPTURE/jit.log"
```

[run_example_kernel.sh](../run_example_kernel.sh) configures CMake with Ninja
and `BUILD_KERNELS=ON`, builds the `example_kernel` target, sets
`TT_METAL_RUNTIME_ROOT` to this submodule, and runs the executable. It also
clears `LD_LIBRARY_PATH` for execution so another Metalium installation does
not replace the freshly built libraries. The default CMake configuration leaves
`BUILD_KERNELS` off and does not configure the submodule.

`2>&1` sends stderr into stdout so compiler messages and application output both
reach `tee`. `tee` writes the log while showing it in the terminal. `pipefail`
prevents a failed build or device run from being hidden by a successful `tee`.
The original log contains:

```text
Passed!
JIT cache stats: 0/21 hits (0.0%)
```

This records successful execution on the actual Wormhole device and no JIT
cache hits in that run. It says nothing yet about Clang's ability to parse the
kernel. The log also records SFPI 7.90.0 build 1068 and GCC 15.1.0.

## Inspecting the MATH compilation context

List compiler invocations, then narrow the list by role:

```bash
grep -n "g++ compile cmd:" "$CAPTURE/jit.log"
grep "g++ compile cmd:" "$CAPTURE/jit.log" | grep "UCK_CHLKC_MATH"
```

The second command is a filter, not a unique selector. This capture has MATH
commands for firmware, `cq_dispatch_subordinate_compute`, and `add_compute`.
The relevant user kernel carries:

```text
FULL_KERNEL_NAME="add_compute/15481761450133143433/"
```

Its compilation directory is:

```text
/home/user/.cache/tt-metal-cache/8654612443254386149/kernels/add_compute/15481761450133143433/trisc1/
```

These numeric identifiers belong to this run. They are not constants used by
`parse_ast.py`. Firmware has `FW_BUILD`, a `firmware/trisc1` directory, and
`trisc.cc` as its source. Both the dispatch compute kernel and our kernel use
`trisck.cc` and `KERNEL_BUILD`, so those markers alone are also insufficient.

The flags describe distinct parts of the context:

| Flag | Role in this experiment |
| --- | --- |
| `-DUCK_CHLKC_MATH` | Selects the MATH branch of `chlkc_list.h`. |
| `-DCOMPILE_FOR_TRISC=1` | Identifies TRISC1 within the compute processor group. |
| `-DPROCESSOR_INDEX=3` | Metalium's processor index for this MATH processor, not a fourth TRISC role. |
| `-DARCH_WORMHOLE` | Selects Wormhole headers and the SFPI analysis fallback extension. |
| `-DKERNEL_BUILD` | Distinguishes kernel builds from `FW_BUILD`. |
| `-DFULL_KERNEL_NAME` | Records kernel name and variant identity, also used by LLK sanitizer code. |
| `-I` | Adds include search directories, including generated files via `-I.` and `-I..`. |
| `-include` | Force includes a header before the main source, including the PCH umbrella in this capture. |
| `-mcpu=tt-wh-tensix` | Selects the Tensix CPU model for SFPI GCC. Clang needs a translated target description. |

Find the generated wrapper and confirm which user source it includes:

```bash
find "$HOME/.cache/tt-metal-cache" -path '*/kernels/add_compute/*/chlkc_math.cpp' -print
grep -nH 'add_compute.cpp' "$HOME"/.cache/tt-metal-cache/*/kernels/add_compute/*/chlkc_math.cpp
```

Compare the result with the directory in the selected command rather than
assuming the first file is current. For the recorded variant, inspect it with:

```bash
KERNEL_DIR="$HOME/.cache/tt-metal-cache/8654612443254386149/kernels/add_compute/15481761450133143433"
cat "$KERNEL_DIR/chlkc_math.cpp"
ls "$KERNEL_DIR/defines_generated.h" "$KERNEL_DIR/chlkc_descriptors.h"
```

The wrapper contains:

```cpp
#define TRISC_MATH
#include "defines_generated.h"
void kernel_main();
#include "/home/user/tt-kernel-lint/example_kernel/device/compute/add_compute.cpp"
```

`TRISC_MATH` selects math implementation sections in the API and LLK headers.
The defines header supplies kernel configuration before those headers are
parsed. The forward declaration allows generated code to refer to the entry
point, and the include supplies its definition from the user source.

Preserve these files, the PCH umbrella text header, the user sources, and the
referenced TT-Metal and SFPI headers. A compilation database records arguments
and paths, not file contents. Clearing the JIT cache or moving the checkout can
make an otherwise valid database unusable. Copying the capture directory alone
does not relocate its absolute source and cache paths.

## Translating SFPI GCC commands for Clang

Use the existing upstream translator, keeping wrapper translation units:

```bash
python3 tt-metal/scripts/build_kernel_clang_tidy_commands.py --input-log "$CAPTURE/jit.log" --output-dir "$CAPTURE/clang" --dedupe none --clang clang++-20
python3 -m json.tool "$CAPTURE/clang/compile_commands.json" | less
```

The result in this experiment was 21 captured commands and 21 translated
entries. This count describes this run only. The database includes firmware and
runtime support kernels as well as our example.

The [translator](../tt-metal/scripts/build_kernel_clang_tidy_commands.py)
replaces the SFPI compiler with Clang. For `tt-wh-tensix` it maps the target to
`riscv32-unknown-elf`, architecture to `rv32im`, and ABI to `ilp32`, and selects
the `wh-ilp32` standard library headers. This retains a RISC-V data model but
does not model the TT CPU's full instruction set for code generation.

It drops SFPI `-ftt-*` extensions, GCC LTO options, selected GCC optimization
options, `-o` output arguments, and `-MF` dependency paths. It removes bare
`-Werror` but does not remove every warning option. When SFPI feature backports
were present, it replaces `-std=c++17` with `-std=c++20`. It adds SFPI's own
newlib and libstdc++ header directories with `-nostdinc++`, `-nostdlibinc`, and
`-isystem`, rather than using the host C++ library. It overrides the riscv32
integer type macros because this GCC/newlib configuration defines `int32_t` as
`long`, which SFPI checks. It also demotes SFPI includes to system includes and
suppresses selected diagnostics for unsupported attributes and template syntax.

The translator preserves the main source, compilation directory, kernel and
role defines, generated include paths, and force included headers. Running with
the recorded working directory is essential. Here `-I.` is `trisc1/` and
`-I..` is the variant directory containing the wrappers and descriptors.

There is a known variant deduplication limitation. The default `kernel-role`
key can collapse different variants sharing a kernel name and target. Even
`--dedupe none` uses the directory and source as a key in this pinned script,
so distinct argv variants at the same directory and source can still collapse.
The log reader also collapses identical command lines. We did not fix this
upstream behavior. The 21 to 21 result does not prove configuration coverage
for another workload.

The pinned [kernel clang-tidy report](../tt-metal/tech_reports/code-indexing/kernel-clang-tidy.md)
and [SFPI fallback README](../tt-metal/tt_metal/jit_build/kernel_clang_tidy/README.md)
explain the upstream approach. The README's unaided parsing result refers to
SFPI 7.73.0. Our SFPI 7.90.0 context required the compatibility investigation
below, so that earlier claim cannot be applied to this capture unchanged.

## Why the initial attempts failed

The first direct attempt was:

```bash
clang++-20 -fsyntax-only example_kernel/device/compute/add_compute.cpp
```

It failed to find `api/compute/common.h`. Adding only that header search path
would still omit the role macros, generated descriptors, platform headers,
integer model, and firmware wrapper declarations. Parsing `add_compute.cpp`
directly is not parsing the actual device translation unit.

An incidental Bash mistake also affected the old replay invocation:

```bash
python3 main.py "$(CAPTURE)"  # Wrong: tries to execute a command named CAPTURE.
python3 main.py "$CAPTURE"    # Correct historical invocation: expands the variable.
```

Those commands describe the original positional interface. Use the new
`--capture` interface below after the migration. Command substitution runs the
text inside `$()`, whereas `$CAPTURE` reads a variable.

Once we replayed the correct MATH translation unit, Clang reached SFPI and
reported two substantive compatibility problems.

### Missing vector type

SFPI's `tensix_builtins.h` detects compilers without Tensix builtins and provides
vector types and builtin declarations for analysis tools. In this version it
declares `__xtt_vector`, `__xtt_vector2`, and `__xtt_vector4`, but not
`__xtt_vector8`, which `tensix_builtins.def` references. We supplied the missing
declaration in a separate Clang compatibility header:

```bash
cat > "$CAPTURE/sfpi_clang_compat.hpp" <<'HEADER'
#pragma once
using __xtt_vector8 = unsigned __attribute__((vector_size(sizeof(unsigned) * 32 * 8)));
HEADER
```

The replay passes this file with `-include` so the type exists before Clang
encounters the SFPI declarations. It is an experimental analysis declaration,
not part of the SFPI device compiler build, and supplies no intrinsic behavior.

### Ambiguous conversion from vMag to vInt

With the vector type available, Clang reported ambiguous conversions in SFPU
headers. In `sfpi_classes.h`, `sfpi::impl_::vMag` aliases
`vNarrow<vUInt, uint32_t>`, and `vNarrow` inherits from its vector type. Thus a
`vMag` inherits `sfpi::vUInt`'s conversion operator. Meanwhile, `sfpi::vInt`
has a converting constructor accepting `impl_::vMag`:

```cpp
// In class sfpi::vInt
sfpi_inline vInt (impl_::vMag);
// In class sfpi::vUInt, inherited by vMag
sfpi_inline operator vInt () const;
```

Clang could not choose between the constructor and inherited conversion
operator for implicit initialization such as `sfpi::vInt man = sfpi::exman(...)`.
The constructor definition in `sfpi_funcs.h` delegates to `vInt(val.get())`.

We copied the real include directory so the investigation would not modify the
installed SFPI headers:

```bash
cp -a tt-metal/runtime/sfpi/include "$CAPTURE/sfpi-include"
```

In that copy only, edit `sfpi.h` to change:

```cpp
sfpi_inline vInt (impl_::vMag);
```

into:

```cpp
sfpi_inline explicit vInt (impl_::vMag);
```

This command applies the exact edit and stops if the original declaration is
not unique. Use it on a fresh copy, not an already patched overlay:

```bash
python3 - "$CAPTURE/sfpi-include/sfpi.h" <<'PY'
import pathlib
import sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = "sfpi_inline vInt (impl_::vMag);"
assert text.count(old) == 1, "Expected exactly one original vMag constructor"
path.write_text(text.replace(old, "sfpi_inline explicit vInt (impl_::vMag);"))
PY
```

The replay substitutes this directory for the original `sfpi/include` search
path. SFPI's compiler standard library paths remain unchanged. Making the
constructor explicit excludes it from the competing implicit initialization
path. Clang then uses the inherited conversion operator, which is marked
deprecated for implicit `vUInt` to `vInt` conversions, explaining the warnings
that appeared after the ambiguity was removed.

This experiment changes overload resolution. It is not an upstream fix or a
verified semantically equivalent replacement for production compilation. The
overlay is useful as a diagnostic frontend baseline, but needs further
validation before it can support trusted production analysis.

### Warnings that did not block the final replay

Clang ignored the GCC `.gch` PCH because it is not a Clang PCH, then parsed the
text umbrella header. The captured warning flag was
`-Wno-error=multistatement-macros`, which Clang diagnosed using the unknown
`-Werror=multistatement-macros` group spelling. Neither warning was the cause
of the missing type or conversion ambiguity.

The final stderr also contains the deprecated conversion warnings described
above and a loop analysis warning in `llk_math_matmul.h`: `t` is incremented in
both the loop header and body. That header is included through the compute API
although this example performs addition. These remained warnings while both
frontend invocations returned zero. Removing warning messages would not solve
the semantic compatibility problems.

## Obtaining the AST and resolving API calls

The final original replay reported:

```text
Exit code: 0
AST exit code: 0
AST size: 13607 bytes
```

It first used `-fsyntax-only`, then ran again with these frontend options:

```text
-fsyntax-only
-Xclang -ast-dump
-Xclang -ast-dump-filter=kernel_main
```

`-fsyntax-only` performs semantic analysis without producing an object file.
`-Xclang` forwards each following option to the frontend. `-ast-dump` requests
a textual AST, and the filter limits the dump to declarations matching
`kernel_main`. The filter does not avoid parsing the rest of the translation
unit, nor does it select only a definition in our user source.

We checked the original output with:

```bash
grep -nE "FunctionDecl.*kernel_main|add_init|add_tiles|compute_kernel_hw_startup" "$CAPTURE/ast.stdout" | head -40
```

The relevant declarations and references, with node addresses omitted, include:

```text
FunctionDecl ... chlkc_math.cpp:3 ... kernel_main 'void ()'
FunctionDecl ... add_compute.cpp:6 ... kernel_main 'void ()'
DeclRefExpr ... 'compute_kernel_hw_startup' 'void (uint32_t, uint32_t, uint32_t)'
DeclRefExpr ... 'add_init' 'void (std::uint32_t, std::uint32_t, bool, std::uint32_t)'
DeclRefExpr ... 'add_tiles' 'void (std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)'
```

The first `FunctionDecl` is the wrapper's forward declaration. The second
refers back to it and has a `CompoundStmt` containing the body defined in
`add_compute.cpp`. A future extractor must distinguish the definition from the
forward declaration instead of treating both as user entry points.

Within that body, a `CallExpr` represents a call and its arguments.
`DeclRefExpr` identifies the resolved function declaration or a referenced
variable. `ImplicitCastExpr` records conversions, including function to pointer
decay and conversion of CB indices to the API's integer parameter types.
These nodes confirm that Clang resolved the expected API declarations, rather
than merely finding their names in source text.

The source calls `add_init(cb_in0, cb_in1)` with two written arguments, but its
declaration has four parameters. The defaults are `acc_to_dest = false` and
`call_line = __builtin_LINE()`. The AST includes two `CXXDefaultArgExpr` nodes,
with source line information represented through a rewritten default expression.
Later LLK normalization will need both defaults and explicit arguments. We
have not yet implemented extraction of fully evaluated argument values.

For a C++ LibTooling implementation, `CallExpr::getDirectCallee()` is the
appropriate starting point for obtaining a resolved function declaration.
This experiment only produces a textual dump. It is not a production
`ResolvedCall` extractor and does not build a CFG or enforce init rules.

## Reproducing the replay with scripts/parse_ast.py

Keep the JIT cache, source checkout, and SFPI installation at the captured
paths. With the translated database and both experimental compatibility inputs
available, run:

```bash
python3 scripts/parse_ast.py --capture "$CAPTURE" --kernel add_compute --role math --compat-header "$CAPTURE/sfpi_clang_compat.hpp" --sfpi-overlay "$CAPTURE/sfpi-include" --ast-filter kernel_main
```

The compatibility flags are optional and never enabled silently. Omitting both
reproduces the baseline failure for this SFPI version. Supplying only
`--compat-header` gets past the vector declaration and exposes the conversion
ambiguity. Use `--output-dir "$CAPTURE/replay-baseline"` or another new directory
to keep those diagnostics separate.

The default database is `$CAPTURE/clang/compile_commands.json`. `--database`
can select another translated database. The tool accepts `arguments` arrays or
shell quoted `command` strings, using `shlex.split` for the latter without
executing a shell. The database must already name a Clang compiler, since this
utility does not replace the upstream translator.

Selection requires `trisck.cc`, `KERNEL_BUILD`, the requested kernel identity,
the requested `UCK_CHLKC_*` role, the matching `COMPILE_FOR_TRISC`, and a matching
`kernels/<name>/<variant>/triscN` directory. It verifies the generated wrapper's
role macro and included user source. If several variants or duplicate entries
remain, it fails with their directories. Pass `--kernel NAME/VARIANT` to choose
an exact identity rather than silently taking the first entry. `--role` accepts
`unpack`, `math`, or `pack`, but only this MATH context was validated here.

The replay keeps the selected entry's working directory and translated semantic
options. It removes compilation and dependency output options, including both
separate and joined operands for `-o`, `-MF`, `-MT`, `-MQ`, and `-MJ`, so those
operands do not become accidental input files. Relative include paths remain
relative and resolve against the recorded directory, including `-I.` and
`-I..`. The optional SFPI substitution only changes include paths ending in
`sfpi/include`, not arbitrary text in defines or standard library paths.

Outputs go under `$CAPTURE/replay` by default:

- `syntax.stdout` and `syntax.stderr` contain the first frontend result.
- `ast.stdout` and `ast.stderr` contain the filtered dump and diagnostics, written
  only when syntax parsing succeeds.
- `syntax.command.json` and `ast.command.json` store exact argv arrays and cwd.
- `replay.json` records the selected database entry, wrapper, compatibility
  paths, exit statuses, and AST byte count when generated.

Original output files directly under `$CAPTURE` are preserved. Reusing an output
directory replaces that replay's files. Check `replay.json` for the current
statuses, since a skipped AST stage leaves any previous AST files untouched.
A null `ast_exit_code` means it did not run. Compiler failures return nonzero,
and input or selection errors report a message and return status 2. A compiler
launch failure is recorded as stage status 127 and returns nonzero too.

Inspect the new output rather than confusing it with the original capture:

```bash
cat "$CAPTURE/replay/replay.json"
grep -nE "FunctionDecl.*kernel_main|add_init|add_tiles|compute_kernel_hw_startup" "$CAPTURE/replay/ast.stdout" | head -40
```

## Verification and remaining limits

During this documentation and migration work we reran the translator on the
original log into a temporary directory. It again produced 21 entries from 21
captured commands. We replayed the existing capture with the migrated utility,
and both frontend stages returned zero with a 13,607 byte AST. The two
`kernel_main` declarations and references to all three expected compute APIs
were present. Baseline replay without compatibility inputs failed on
`__xtt_vector8`, while replay with only the vector header failed on the
`vMag` conversion. These were newly executed frontend checks, not a new device
run. The hardware success above remains evidence from the original `jit.log`.

The utility's metadata and failure tests can be run without hardware or an SDK:

```bash
python3 -m py_compile scripts/parse_ast.py
python3 scripts/parse_ast.py --help
python3 -m unittest discover -s tests -p test_parse_ast.py -v
```

The tests cover both database forms, role and variant selection, missing inputs,
ambiguous contexts, wrapper checks, output option operands, relative include
context, overlay substitution, and compiler launch failure reporting. They use
small metadata fixtures, not fake SFPI APIs.

Replaying this capture still depends on local generated files and absolute
paths outside the capture directory. The utility cannot restore deleted cache
headers or recover contexts the translator collapsed. Blackhole, other SFPI
releases, and the remaining compute roles have not been validated by this AST
experiment. Successful parsing under the modified overlay does not establish
production semantic equivalence, device code generation correctness, or LLK
protocol correctness. The next analysis step is a structured extraction of
resolved calls and argument expressions under an explicitly recorded context.
