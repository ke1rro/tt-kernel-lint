#!/usr/bin/env python3
"""Verify the pinned SFPI installation and optionally create a Clang-only overlay."""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = Path(__file__).resolve().parent / "compat/sfpi-7.90.0.json"
COMPILER = "compiler/bin/riscv-tt-elf-g++"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def header_hashes(include):
    return {
        str(p.relative_to(include)): sha256(p)
        for p in include.rglob("*")
        if p.is_file()
    }


def selected_root(metal):
    """Match JitBuildEnv::init's local-before-system selection."""
    for root in (metal / "runtime/sfpi", Path("/opt/tenstorrent/sfpi")):
        if (root / COMPILER).is_file():
            return root.resolve()
    raise ValueError("No SFPI compiler in TT-Metal runtime or /opt/tenstorrent/sfpi")


def verify(metal, root, spec):
    manifest = dict(
        re.findall(
            r"^(sfpi_\w+)='([^']*)'$",
            (metal / "tt_metal/sfpi-version").read_text(),
            re.M,
        )
    )
    for key, value in spec["manifest"].items():
        if manifest.get(key) != value:
            raise ValueError(
                f"Unsupported TT-Metal SFPI manifest: {key} must be {value}"
            )
    result = subprocess.run(
        [str(root / COMPILER), "--version"], check=True, capture_output=True, text=True
    )
    banner = next(iter(result.stdout.splitlines()), "")
    if f"tenstorrent/sfpi:{spec['version']}[{spec['build']}]" not in banner:
        raise ValueError(f"SFPI compiler mismatch: {banner}")
    actual = header_hashes(root / "include")
    if actual != spec["headers_sha256"]:
        names = sorted(set(actual) | set(spec["headers_sha256"]))
        changed = [
            name
            for name in names
            if actual.get(name) != spec["headers_sha256"].get(name)
        ]
        raise ValueError(
            "SFPI headers mismatch (modified or mixed installation): "
            + ", ".join(changed)
        )
    return {
        "version": spec["version"],
        "build": spec["build"],
        "sfpi_root": str(root),
        "compiler_banner": banner,
        "compiler_sha256": sha256(root / COMPILER),
        "headers_sha256": actual,
        "compatibility_spec_sha256": sha256(SPEC),
    }


def make_overlay(root, destination, spec, report):
    if destination.exists():
        raise ValueError(
            f"Overlay destination already exists: {destination}. "
            "Choose a fresh directory."
        )
    patch = spec["patch"]
    original = (root / "include" / patch["file"]).read_text()
    if original.count(patch["before"]) != 1 or patch["after"] in original:
        raise ValueError("Cannot verify original SFPI constructor transformation")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent) as tmp:
        stage = Path(tmp) / "overlay"
        shutil.copytree(root / "include", stage / "include")
        # Recheck copied bytes before applying the sole semantic adjustment.
        copied = header_hashes(stage / "include")
        if copied != spec["headers_sha256"]:
            raise ValueError("SFPI headers changed while copying")
        # Patch the verified staged content rather than rereading the installation.
        staged_header = stage / "include" / patch["file"]
        staged_header.write_text(
            staged_header.read_text().replace(patch["before"], patch["after"])
        )
        shutil.copyfile(
            SPEC.with_name("sfpi-7.90.0-clang.hpp"), stage / "sfpi_clang_compat.hpp"
        )
        report = dict(
            report, patched_sfpi_sha256=sha256(stage / "include" / patch["file"])
        )
        (stage / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
        stage.rename(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metal-root", type=Path, default=ROOT / "tt-metal")
    parser.add_argument(
        "--sfpi-root",
        type=Path,
        help="Explicit captured installation instead of runtime selection",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Create a new overlay here, leaving the SDK intact",
    )
    args = parser.parse_args()
    try:
        metal = args.metal_root.resolve()
        root = args.sfpi_root.resolve() if args.sfpi_root else selected_root(metal)
        spec = json.loads(SPEC.read_text())
        report = verify(metal, root, spec)
        if args.output_dir:
            make_overlay(root, args.output_dir.resolve(), spec, report)
        print(json.dumps(report, indent=2))
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"check_sfpi: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
