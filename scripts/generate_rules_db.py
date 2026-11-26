#!/usr/bin/env python3
"""
scripts/generate_rules_db.py

Generates a C++20 header (tt_kernel_lint/include/ttkl/kernel_api/rules_db.hpp)
containing constexpr EXU/resource state-machine tables and constexpr O(log N)
lookup helpers from specs/api_spec.yaml (schema v2.1).
Uses types defined in ttkl/kernel_api/common.hpp and ttkl/kernel_api/rule.hpp.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from typing import Any

import yaml

KIND_MAP: dict[str, str] = {
    "startup": "InstructionKind::kStartup",
    "cleanup": "InstructionKind::kCleanup",
    "reconfig": "InstructionKind::kReconfig",
    "init": "InstructionKind::kInit",
    "op": "InstructionKind::kOp",
    "uninit": "InstructionKind::kUninit",
    "acquire": "InstructionKind::kAcquire",
    "release": "InstructionKind::kRelease",
}

THREAD_MAP: dict[str, str] = {
    "unpack": "ThreadKind::kUnpack",
    "math": "ThreadKind::kMath",
    "pack": "ThreadKind::kPack",
    "dm": "ThreadKind::kDm",
}

ARCH_MAP: dict[str, str] = {
    "wh": "ArchMask::kWormhole",
    "bh": "ArchMask::kBlackhole",
    "quasar": "ArchMask::kQuasar",
}

EXU_UNIT_MAP: dict[str, str] = {
    "unpack": "ExuUnit::kUnpack",
    "fpu": "ExuUnit::kFpu",
    "sfpu": "ExuUnit::kSfpu",
    "pack": "ExuUnit::kPack",
    "sfpu_pack": "ExuUnit::kSfpuPack",
    "noc_rd_state": "ExuUnit::kNocRdState",
    "noc_wr_state": "ExuUnit::kNocWrState",
    "noc_inline_state": "ExuUnit::kNocInlineState",
}

BUILTIN_EXU_MODES: dict[str, tuple[int, str]] = {
    "none": (0, "ExuModeId::kNone"),
    "uninit": (1, "ExuModeId::kUninit"),
    "idle": (2, "ExuModeId::kIdle"),
    "clean": (3, "ExuModeId::kClean"),
    "started": (4, "ExuModeId::kStarted"),
}

RESOURCE_KIND_MAP: dict[str, str] = {
    "dst_math": "ResourceKind::kDstMath",
    "dst_pack": "ResourceKind::kDstPack",
    "dest_acc": "ResourceKind::kDestAcc",
    "cb_in": "ResourceKind::kCbIn",
    "cb_out": "ResourceKind::kCbOut",
    "noc_rd": "ResourceKind::kNocRd",
    "noc_wr": "ResourceKind::kNocWr",
    "noc_atomic": "ResourceKind::kNocAtomic",
}

RESOURCE_STATE_MAP: dict[str, str] = {
    "free": "ResourceState::kFree",
    "held": "ResourceState::kHeld",
    "default": "ResourceState::kDefault",
    "overridden": "ResourceState::kOverridden",
    "idle": "ResourceState::kIdle",
    "ready": "ResourceState::kReady",
    "reserved": "ResourceState::kReserved",
    "in_flight": "ResourceState::kInFlight",
}

ARG_ROLE_MAP: dict[str, str] = {
    "block": "ArgRole::kBlock",
    "cb": "ArgRole::kCb",
    "ct_dim": "ArgRole::kCtDim",
    "icb0": "ArgRole::kIcb0",
    "icb1": "ArgRole::kIcb1",
    "idst": "ArgRole::kIdst",
    "idst0": "ArgRole::kIdst0",
    "idst1": "ArgRole::kIdst1",
    "idst2": "ArgRole::kIdst2",
    "kt_dim": "ArgRole::kKtDim",
    "noc": "ArgRole::kNoc",
    "ntiles": "ArgRole::kNtiles",
    "ocb": "ArgRole::kOcb",
    "odst": "ArgRole::kOdst",
    "rt_dim": "ArgRole::kRtDim",
    "transpose": "ArgRole::kTranspose",
    "trid": "ArgRole::kTrid",
}

TARG_ROLE_MAP: dict[str, str] = {
    "approx": "TArgRole::kApprox",
    "bcast_dim": "TArgRole::kBcastDim",
    "bcast_op": "TArgRole::kBcastOp",
    "bcast_type": "TArgRole::kBcastType",
    "block_ct_dim": "TArgRole::kBlockCtDim",
    "ct_dim": "TArgRole::kCtDim",
    "dense": "TArgRole::kDense",
    "eltwise_binary_type": "TArgRole::kEltwiseBinaryType",
    "enable": "TArgRole::kEnable",
    "full_ct_dim": "TArgRole::kFullCtDim",
    "input_clamping": "TArgRole::kInputClamping",
    "is_32bit": "TArgRole::kIs32Bit",
    "layout": "TArgRole::kLayout",
    "narrow_row": "TArgRole::kNarrowRow",
    "normalize": "TArgRole::kNormalize",
    "reduce_dim": "TArgRole::kReduceDim",
    "reduce_type": "TArgRole::kReduceType",
    "respect_trigger": "TArgRole::kRespectTrigger",
    "reuse_dest": "TArgRole::kReuseDest",
    "row_num_datums": "TArgRole::kRowNumDatums",
    "src_order": "TArgRole::kSrcOrder",
    "tile_dim_reconfig": "TArgRole::kTileDimReconfig",
    "transpose": "TArgRole::kTranspose",
    "transpose_of_faces": "TArgRole::kTransposeOfFaces",
}

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def as_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value]
    return [str(value)]


def c_str(value: str | None) -> str:
    """Escapes a string as a C++ double-quoted string literal."""
    if value is None:
        return '""'
    return json.dumps(str(value))


def mode_to_cpp_ident(mode: str) -> str:
    """Converts a snake_case mode name into a kPascalCase C++ identifier."""
    parts = [p for p in re.split(r"[^A-Za-z0-9]+", mode) if p]
    return "k" + "".join(p[0].upper() + p[1:] for p in parts)


def format_with_clang_format(source: str, output_path: str) -> str:
    """Formats generated C++ with the repository .clang-format, as the hook does."""
    venv_binary = os.path.join(REPO_ROOT, ".venv", "bin", "clang-format")
    binary = (
        venv_binary if os.path.isfile(venv_binary) else shutil.which("clang-format")
    )
    if binary is None:
        print("Error: clang-format not found (run setup.sh).", file=sys.stderr)
        sys.exit(1)
    result = subprocess.run(
        [binary, "--style=file", f"--assume-filename={os.path.abspath(output_path)}"],
        input=source,
        capture_output=True,
        text=True,
        check=True,
        cwd=REPO_ROOT,
    )
    return result.stdout


def format_threads(threads: list[str], inst_name: str) -> str:
    if not threads:
        raise ValueError(f"{inst_name}: missing threads")
    cpp_flags: list[str] = []
    for thread in threads:
        if thread not in THREAD_MAP:
            raise ValueError(f"{inst_name}: unknown thread '{thread}'")
        cpp_flags.append(THREAD_MAP[thread])
    return " | ".join(cpp_flags)


def format_arch(archs: list[str] | None, inst_name: str) -> str:
    if not archs:
        return "ArchMask::kAll"
    cpp_flags: list[str] = []
    for arch in archs:
        if arch not in ARCH_MAP:
            raise ValueError(f"{inst_name}: unknown arch '{arch}'")
        cpp_flags.append(ARCH_MAP[arch])
    return " | ".join(cpp_flags)


def validate_spec(instructions: dict[str, dict[str, Any]]) -> None:
    """Validates YAML schema and state-machine invariants before code generation."""
    errors: list[str] = []
    modes_set: dict[str, set[str]] = {unit: set() for unit in EXU_UNIT_MAP}

    for name, entry in instructions.items():
        if entry.get("kind") not in KIND_MAP:
            errors.append(f"{name}: invalid kind '{entry.get('kind')}'")
        for unit, tr in entry.get("exu", {}).items():
            if unit not in EXU_UNIT_MAP:
                errors.append(f"{name}: unknown EXU unit '{unit}'")
                continue
            if "set" in tr:
                modes_set[unit].add(str(tr["set"]))

    for name, entry in instructions.items():
        dirty_units: dict[str, str] = {}
        for unit, tr in entry.get("exu", {}).items():
            if unit not in EXU_UNIT_MAP:
                continue
            for req in as_list(tr.get("require")):
                if req not in BUILTIN_EXU_MODES and req not in modes_set[unit]:
                    errors.append(
                        f"{name}: exu.{unit}.require='{req}' is never set on '{unit}'"
                    )
            if tr.get("dirty"):
                if "set" not in tr:
                    errors.append(f"{name}: exu.{unit} sets dirty=true without 'set'")
                else:
                    dirty_units[unit] = str(tr["set"])

        uninits = as_list(entry.get("uninit"))
        if dirty_units and not uninits:
            errors.append(f"{name}: dirty EXUs {dirty_units} lack 'uninit'")
        for uninit_name in uninits:
            if uninit_name not in instructions:
                errors.append(f"{name}: uninit '{uninit_name}' not found")
                continue
            u_entry = instructions[uninit_name]
            if u_entry.get("kind") != "uninit":
                errors.append(
                    f"{name}: uninit '{uninit_name}' has kind '{u_entry.get('kind')}'"
                )
            for d_unit, d_mode in dirty_units.items():
                u_tr = u_entry.get("exu", {}).get(d_unit)
                if (
                    not u_tr
                    or u_tr.get("set") != "idle"
                    or d_mode not in as_list(u_tr.get("require"))
                ):
                    errors.append(
                        f"{name}: dirty {d_unit}='{d_mode}' not reset to idle "
                        f"by '{uninit_name}'"
                    )

        for res_name, res_tr in entry.get("resources", {}).items():
            if res_name not in RESOURCE_KIND_MAP:
                errors.append(f"{name}: unknown resource '{res_name}'")
            for key in ("require", "set"):
                if key in res_tr:
                    vals = as_list(res_tr[key])
                    if len(vals) != 1 or vals[0] not in RESOURCE_STATE_MAP:
                        errors.append(
                            f"{name}: invalid resources.{res_name}.{key}={res_tr[key]}"
                        )

        for partner in as_list(entry.get("pair")):
            if partner not in instructions:
                errors.append(f"{name}: pair '{partner}' not found")
            elif name not in as_list(instructions[partner].get("pair")):
                errors.append(
                    f"{name}: asymmetric pair with '{partner}' (missing back-link)"
                )

        for role in entry.get("args", {}):
            if role not in ARG_ROLE_MAP:
                errors.append(f"{name}: unknown arg role '{role}'")
        for role in entry.get("targs", {}):
            if role not in TARG_ROLE_MAP:
                errors.append(f"{name}: unknown targ role '{role}'")

    if errors:
        print(
            f"Error: {len(errors)} validation failure(s) in API spec:", file=sys.stderr
        )
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        sys.exit(1)


def collect_exu_modes(
    instructions: dict[str, dict[str, Any]],
) -> tuple[list[str], dict[str, str]]:
    """Collects and interns all EXU modes, returning (id->name list, name->cpp expr)."""
    concrete_modes: set[str] = set()
    for entry in instructions.values():
        for tr in entry.get("exu", {}).values():
            for req in as_list(tr.get("require")):
                if req not in BUILTIN_EXU_MODES:
                    concrete_modes.add(req)
            if "set" in tr:
                mode = str(tr["set"])
                if mode not in BUILTIN_EXU_MODES:
                    concrete_modes.add(mode)

    mode_names: list[str] = ["none", "uninit", "idle", "clean", "started"]
    mode_expr: dict[str, str] = {
        name: cpp_expr for name, (_, cpp_expr) in BUILTIN_EXU_MODES.items()
    }
    used_idents: set[str] = set()

    for mode in sorted(concrete_modes):
        ident = mode_to_cpp_ident(mode)
        if ident in used_idents:
            raise ValueError(
                f"Duplicate C++ identifier '{ident}' for EXU mode '{mode}'"
            )
        used_idents.add(ident)
        mode_names.append(mode)
        mode_expr[mode] = f"exu_mode::{ident}"

    return mode_names, mode_expr


def generate_cpp(yaml_path: str, output_path: str) -> None:
    with open(yaml_path, "r", encoding="utf-8") as f:
        spec = yaml.safe_load(f)

    if not isinstance(spec, dict) or not isinstance(spec.get("instructions"), dict):
        print(
            f"Error: '{yaml_path}' must contain a v2.1 mapping under 'instructions'.",
            file=sys.stderr,
        )
        sys.exit(1)

    spec_version = str(spec.get("version", ""))
    tt_metal_commit = str(spec.get("tt_metal_commit", ""))
    instructions: dict[str, dict[str, Any]] = spec["instructions"]
    validate_spec(instructions)

    mode_names, mode_expr = collect_exu_modes(instructions)
    sorted_names = sorted(instructions.keys())

    # Flat constexpr backing pools populated as we walk sorted instructions.
    exu_req_pool: list[str] = []
    exu_tr_pool: list[tuple[str, int, int, str, bool]] = []
    res_tr_pool: list[tuple[str, str, str]] = []
    uninit_pool: list[str] = []
    pair_pool: list[str] = []
    arg_pool: list[tuple[str, int]] = []
    targ_pool: list[tuple[str, int]] = []

    # Per-instruction slice metadata: (name, entry, slices_dict)
    inst_records: list[tuple[str, dict[str, Any], dict[str, tuple[int, int]]]] = []

    for name in sorted_names:
        entry = instructions[name]
        slices: dict[str, tuple[int, int]] = {}

        exu_start = len(exu_tr_pool)
        for unit, tr in entry.get("exu", {}).items():
            reqs = as_list(tr.get("require"))
            req_start = len(exu_req_pool)
            for req in reqs:
                exu_req_pool.append(mode_expr[req])
            set_expr = mode_expr[str(tr["set"])] if "set" in tr else "ExuModeId::kNone"
            exu_tr_pool.append(
                (
                    EXU_UNIT_MAP[unit],
                    req_start,
                    len(reqs),
                    set_expr,
                    bool(tr.get("dirty", False)),
                )
            )
        slices["exu"] = (exu_start, len(exu_tr_pool) - exu_start)

        res_start = len(res_tr_pool)
        for res_name, res_tr in entry.get("resources", {}).items():
            req_expr = (
                RESOURCE_STATE_MAP[str(res_tr["require"])]
                if "require" in res_tr
                else "ResourceState::kNone"
            )
            set_expr = (
                RESOURCE_STATE_MAP[str(res_tr["set"])]
                if "set" in res_tr
                else "ResourceState::kNone"
            )
            res_tr_pool.append((RESOURCE_KIND_MAP[res_name], req_expr, set_expr))
        slices["resources"] = (res_start, len(res_tr_pool) - res_start)

        uninit_start = len(uninit_pool)
        for uninit_name in as_list(entry.get("uninit")):
            uninit_pool.append(uninit_name)
        slices["uninits"] = (uninit_start, len(uninit_pool) - uninit_start)

        pair_start = len(pair_pool)
        for pair_name in as_list(entry.get("pair")):
            pair_pool.append(pair_name)
        slices["pairs"] = (pair_start, len(pair_pool) - pair_start)

        arg_start = len(arg_pool)
        for role, idx in entry.get("args", {}).items():
            arg_pool.append((ARG_ROLE_MAP[role], int(idx)))
        slices["args"] = (arg_start, len(arg_pool) - arg_start)

        targ_start = len(targ_pool)
        for role, idx in entry.get("targs", {}).items():
            targ_pool.append((TARG_ROLE_MAP[role], int(idx)))
        slices["targs"] = (targ_start, len(targ_pool) - targ_start)

        inst_records.append((name, entry, slices))

    lines: list[str] = [
        "#pragma once",
        "",
        "// AUTO-GENERATED by scripts/generate_rules_db.py from specs/api_spec.yaml.",
        "// Do not edit by hand.",
        "",
        '#include "ttkl/kernel_api/rule.hpp"',
        "",
        "#include <algorithm>",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <span>",
        "#include <string_view>",
        "",
        "namespace tt::lint {",
        "",
        f"inline constexpr std::string_view kSpecVersion{{{c_str(spec_version)}}};",
        (
            "inline constexpr std::string_view kTtMetalCommit"
            f"{{{c_str(tt_metal_commit)}}};"
        ),
        f"inline constexpr std::size_t kInstructionCount{{{len(inst_records)}}};",
        f"inline constexpr std::size_t kExuModeCount{{{len(mode_names)}}};",
        "",
        "namespace exu_mode {",
    ]

    for mode_id, mode_name in enumerate(mode_names):
        if mode_id < 5:
            continue
        ident = mode_to_cpp_ident(mode_name)
        lines.append(
            f"inline constexpr ExuModeId {ident}{{static_cast<ExuModeId>({mode_id})}};"
        )

    lines.extend(
        [
            "}  // namespace exu_mode",
            "",
            (
                "inline constexpr std::array<std::string_view, kExuModeCount> "
                "kExuModeNames = {"
            ),
        ]
    )
    for mode_name in mode_names:
        lines.append(f"    {c_str(mode_name)},")
    lines.extend(
        [
            "};",
            "",
            "constexpr std::string_view exuModeName(ExuModeId id) {",
            "    const auto index = static_cast<std::size_t>(id);",
            "    if (index < kExuModeNames.size()) {",
            "        return kExuModeNames[index];",
            "    }",
            '    return "unknown";',
            "}",
            "",
            "constexpr ExuModeId findExuMode(std::string_view name) {",
            "    for (std::size_t i = 0; i < kExuModeNames.size(); ++i) {",
            "        if (kExuModeNames[i] == name) {",
            "            return static_cast<ExuModeId>(i);",
            "        }",
            "    }",
            "    return ExuModeId::kNone;",
            "}",
            "",
            "namespace detail {",
            "",
            "template <typename T, std::size_t N>",
            (
                "constexpr std::span<const T> slice("
                "const std::array<T, N>& pool, std::size_t offset, std::size_t count) {"
            ),
            "    return std::span<const T>{pool}.subspan(offset, count);",
            "}",
            "",
            (
                f"inline constexpr std::array<ExuModeId, {len(exu_req_pool)}> "
                "kExuRequirePool = {"
            ),
        ]
    )
    for req_expr in exu_req_pool:
        lines.append(f"    {req_expr},")
    lines.append("};")
    lines.append("")

    lines.append(
        f"inline constexpr std::array<ExuTransition, {len(exu_tr_pool)}> "
        "kExuTransitionPool = {"
    )
    for unit_expr, req_off, req_cnt, set_expr, is_dirty in exu_tr_pool:
        req_val = (
            f"slice(kExuRequirePool, {req_off}, {req_cnt})" if req_cnt > 0 else "{}"
        )
        fields = [f".unit = {unit_expr}", f".require = {req_val}"]
        if set_expr != "ExuModeId::kNone":
            fields.append(f".set = {set_expr}")
        if is_dirty:
            fields.append(".dirty = true")
        lines.append("    ExuTransition{" + ", ".join(fields) + "},")
    lines.append("};")
    lines.append("")

    lines.append(
        f"inline constexpr std::array<ResourceTransition, {len(res_tr_pool)}> "
        "kResourceTransitionPool = {"
    )
    for res_expr, req_expr, set_expr in res_tr_pool:
        fields = [f".resource = {res_expr}"]
        if req_expr != "ResourceState::kNone":
            fields.append(f".require = {req_expr}")
        if set_expr != "ResourceState::kNone":
            fields.append(f".set = {set_expr}")
        lines.append("    ResourceTransition{" + ", ".join(fields) + "},")
    lines.append("};")
    lines.append("")

    lines.append(
        f"inline constexpr std::array<std::string_view, {len(uninit_pool)}> "
        "kUninitPool = {"
    )
    for uninit_name in uninit_pool:
        lines.append(f"    {c_str(uninit_name)},")
    lines.append("};")
    lines.append("")

    lines.append(
        f"inline constexpr std::array<std::string_view, {len(pair_pool)}> "
        "kPairPool = {"
    )
    for pair_name in pair_pool:
        lines.append(f"    {c_str(pair_name)},")
    lines.append("};")
    lines.append("")

    lines.append(
        f"inline constexpr std::array<ArgBinding, {len(arg_pool)}> kArgBindingPool = {{"
    )
    for role_expr, arg_idx in arg_pool:
        lines.append(f"    ArgBinding{{.role = {role_expr}, .index = {arg_idx}}},")
    lines.append("};")
    lines.append("")

    lines.append(
        f"inline constexpr std::array<TArgBinding, {len(targ_pool)}> "
        "kTArgBindingPool = {"
    )
    for role_expr, targ_idx in targ_pool:
        lines.append(f"    TArgBinding{{.role = {role_expr}, .index = {targ_idx}}},")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace detail")
    lines.append("")

    lines.append(
        "// Sorted lexicographically by instruction name for constexpr O(log N) lookup."
    )
    lines.append(
        "inline constexpr std::array<Instruction, kInstructionCount> kInstructions = {"
    )

    def span_expr(pool_name: str, span_slice: tuple[int, int]) -> str:
        off, cnt = span_slice
        return f"detail::slice(detail::{pool_name}, {off}, {cnt})" if cnt > 0 else "{}"

    for name, entry, slices in inst_records:
        fields = [
            f".name = {c_str(name)}",
            f".header = {c_str(entry['header'])}",
            f".kind = {KIND_MAP[entry['kind']]}",
            f".threads = {format_threads(entry['threads'], name)}",
        ]
        if entry.get("arch"):
            fields.append(f".arch = {format_arch(entry['arch'], name)}")
        fields.append(f".exu = {span_expr('kExuTransitionPool', slices['exu'])}")
        fields.append(
            f".resources = {span_expr('kResourceTransitionPool', slices['resources'])}"
        )
        fields.append(f".uninits = {span_expr('kUninitPool', slices['uninits'])}")
        fields.append(f".pairs = {span_expr('kPairPool', slices['pairs'])}")
        fields.append(f".args = {span_expr('kArgBindingPool', slices['args'])}")
        fields.append(f".targs = {span_expr('kTArgBindingPool', slices['targs'])}")
        if entry.get("stateful", False):
            fields.append(".stateful = true")
        if not entry.get("in_loop", True):
            fields.append(".in_loop = false")
        if entry.get("deprecated", False):
            fields.append(".deprecated = true")
        if entry.get("experimental", False):
            fields.append(".experimental = true")

        lines.append("    Instruction{")
        for field in fields:
            lines.append(f"        {field},")
        lines.append("    },")

    lines.extend(
        [
            "};",
            "",
            (
                "// Constexpr O(log N) binary-search lookup returning nullptr "
                "if unrecognized."
            ),
            "constexpr const Instruction* findInstruction(std::string_view name) {",
            "    const auto* it = std::lower_bound(",
            "        kInstructions.begin(),",
            "        kInstructions.end(),",
            "        name,",
            (
                "        [](const Instruction& inst, std::string_view key) "
                "{ return inst.name < key; });"
            ),
            "    if (it != kInstructions.end() && it->name == name) {",
            "        return it;",
            "    }",
            "    return nullptr;",
            "}",
            "",
            "}  // namespace tt::lint",
            "",
        ]
    )

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    formatted = format_with_clang_format("\n".join(lines), output_path)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(formatted)

    print(
        f"Successfully generated {output_path} "
        f"({len(inst_records)} instructions, {len(mode_names)} EXU modes)."
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Convert Tenstorrent API YAML specification into a C++20 rules header."
        )
    )
    parser.add_argument(
        "--yaml",
        "-y",
        default="specs/api_spec.yaml",
        help="Path to input YAML file (default: specs/api_spec.yaml)",
    )
    parser.add_argument(
        "--output",
        "-o",
        default="tt_kernel_lint/include/ttkl/kernel_api/rules_db.hpp",
        help=(
            "Path to output C++ header file "
            "(default: tt_kernel_lint/include/ttkl/kernel_api/rules_db.hpp)"
        ),
    )
    args = parser.parse_args()

    if not os.path.isfile(args.yaml):
        print(
            f"Error: YAML specification file '{args.yaml}' not found.", file=sys.stderr
        )
        sys.exit(1)

    generate_cpp(args.yaml, args.output)


if __name__ == "__main__":
    main()
