#!/usr/bin/env python3
"""
scripts/generate_rules_db.py

Generates a C++ header (tt_kernel_lint/include/ttkl/kernel_api/rules_db.h)
containing constexpr Instruction tables
and fast O(1) lookup maps from specs/api_spec.yaml.
Uses types defined in ttkl/kernel_api/common.h and ttkl/kernel_api/rule.h.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

import yaml

CATEGORY_MAP = {
    "startup": "Category::kStartup",
    "cleanup": "Category::kCleanup",
    "reg_lifecycle": "Category::kRegLifecycle",
    "cb_lifecycle": "Category::kCbLifecycle",
    "dataflow_noc": "Category::kDataflowNoc",
    "reconfig": "Category::kReconfig",
    "matmul": "Category::kMatmul",
    "binary_eltwise": "Category::kBinaryEltwise",
    "bcast_eltwise": "Category::kBcastEltwise",
    "reduce": "Category::kReduce",
    "tilize": "Category::kTilize",
    "untilize": "Category::kUntilize",
    "pack": "Category::kPack",
    "unary_datacopy": "Category::kUnaryDatacopy",
    "custom_mm": "Category::kCustomMm",
    "sfpu_stateful": "Category::kSfpuStateful",
    "sfpu_stateless": "Category::kSfpuStateless",
}

THREAD_MAP = {
    "TRISC0_UNPACK": "ThreadKind::kTrisc0Unpack",
    "TRISC1_MATH": "ThreadKind::kTrisc1Math",
    "TRISC2_PACK": "ThreadKind::kTrisc2Pack",
    "DM_BRISC": "ThreadKind::kBrisc",
    "DM_NCRISC": "ThreadKind::kNcrisc",
    "BRISC": "ThreadKind::kBrisc",
    "NCRISC": "ThreadKind::kNcrisc",
}

STATE_MAP = {
    "ANY": "State::kAny",
    "None": "State::kAny",
    "null": "State::kAny",
    "": "State::kAny",
    "UNINITIALIZED": "State::kUninitialized",
    "CANONICAL_IDLE": "State::kCanonicalIdle",
    "MATH_IDLE": "State::kMathIdle",
    "MATH_DST_HELD": "State::kMathDstHeld",
    "PACK_IDLE": "State::kPackIdle",
    "PACK_DST_HELD": "State::kPackDstHeld",
    "CB_TILES_AVAILABLE": "State::kCbTilesAvailable",
    "CB_SLOTS_RESERVED": "State::kCbSlotsReserved",
    "NOC_READ_IN_FLIGHT": "State::kNocReadInFlight",
    "NOC_WRITE_IN_FLIGHT": "State::kNocWriteInFlight",
    "IN_TILIZE_MODE": "State::kInTilizeMode",
    "IN_UNTILIZE_MODE": "State::kInUntilizeMode",
    "IN_REDUCE_MODE": "State::kInReduceMode",
    "IN_UNARY_BCAST_MODE": "State::kInUnaryBcastMode",
    "IN_CUSTOM_MM_DENSE_MODE": "State::kInCustomMmDenseMode",
}


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


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


def c_str(s: str | None) -> str:
    """Escapes string as a C++ double-quoted string literal."""
    if s is None:
        return '""'
    return json.dumps(str(s))


def format_threads(threads: list[str]) -> str:
    if not threads:
        return "ThreadKind::kNone"
    cpp_threads = [THREAD_MAP.get(t, f"ThreadKind::{t}") for t in threads]
    return " | ".join(cpp_threads)


def format_state(state: str | None) -> str:
    if state is None:
        return "State::kAny"
    s = str(state)
    return STATE_MAP.get(s, f"State::{s}")


def generate_cpp(yaml_path: str, output_path: str) -> None:
    with open(yaml_path, "r", encoding="utf-8") as f:
        spec = yaml.safe_load(f)

    instructions = spec.get("instructions", [])
    count = len(instructions)

    lines = [
        "#pragma once",
        "",
        '#include "ttkl/kernel_api/rule.h"',
        "#include <array>",
        "#include <cstddef>",
        "#include <string_view>",
        "#include <unordered_map>",
        "",
        "namespace tt_lint {",
        "",
        f"inline constexpr std::size_t kInstructionCount{{{count}}};",
        "",
        "inline constexpr std::array<Instruction, kInstructionCount> kInstructions = {",
    ]

    for idx, inst in enumerate(instructions):
        name = inst.get("name", "")
        signature = inst.get("signature", "")
        category = CATEGORY_MAP.get(inst.get("category", ""), "Category::kStartup")
        threads = format_threads(inst.get("thread_affinity", []))

        state_machine = inst.get("state_machine", {})
        pre_state = format_state(state_machine.get("precondition_state"))
        post_state = format_state(state_machine.get("postcondition_state"))
        must_pair = c_str(state_machine.get("must_pair_with"))

        allowed_in_loop = "true" if inst.get("allowed_in_loop", True) else "false"
        failure_mode = c_str(inst.get("failure_mode", ""))
        description = c_str(inst.get("description", ""))

        comma = "," if idx < count - 1 else ""
        lines.append("    Instruction{")
        lines.append(f"        .name = {c_str(name)},")
        lines.append(f"        .signature = {c_str(signature)},")
        lines.append(f"        .must_pair_with = {must_pair},")
        lines.append(f"        .failure_mode = {failure_mode},")
        lines.append(f"        .description = {description},")
        lines.append(f"        .category = {category},")
        lines.append(f"        .thread_affinity = {threads},")
        lines.append(f"        .precondition_state = {pre_state},")
        lines.append(f"        .postcondition_state = {post_state},")
        lines.append(f"        .allowed_in_loop = {allowed_in_loop}")
        lines.append(f"    }}{comma}")

    lines.extend(
        [
            "};",
            "",
            (
                "// Returns an in-memory hash map initialized once for O(1) "
                "lookup by function name."
            ),
            (
                "inline const std::unordered_map<std::string_view, "
                "const Instruction*>& getInstructionMap() {"
            ),
            "    static const auto kMap = []() {",
            "        std::unordered_map<std::string_view, const Instruction*> map;",
            "        map.reserve(kInstructions.size());",
            "        for (const auto& inst : kInstructions) {",
            "            map[inst.name] = &inst;",
            "        }",
            "        return map;",
            "    }();",
            "    return kMap;",
            "}",
            "",
            (
                "// Fast O(1) lookup returning nullptr if the function is "
                "not a recognized Tensix API."
            ),
            "inline const Instruction* findInstruction(std::string_view name) {",
            "    const auto& map = getInstructionMap();",
            "    auto it = map.find(name);",
            "    return it != map.end() ? it->second : nullptr;",
            "}",
            "",
            "} // namespace tt_lint",
            "",
        ]
    )

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(format_with_clang_format("\n".join(lines), output_path))

    print(f"Successfully generated {output_path} ({count} instructions).")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert Tenstorrent API YAML specification into C++ rules header."
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
        default="tt_kernel_lint/include/ttkl/kernel_api/rules_db.h",
        help=(
            "Path to output C++ header file "
            "(default: tt_kernel_lint/include/ttkl/kernel_api/rules_db.h)"
        ),
    )
    args = parser.parse_args()

    if not os.path.isfile(args.yaml):
        alt_yaml = "specs/kernel_api_formal_spec.yaml"
        if os.path.isfile(alt_yaml):
            args.yaml = alt_yaml
        else:
            print(
                f"Error: YAML specification file '{args.yaml}' not found.",
                file=sys.stderr,
            )
            sys.exit(1)

    generate_cpp(args.yaml, args.output)


if __name__ == "__main__":
    main()
