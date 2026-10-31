#!/usr/bin/env python3
"""Replay a captured Metalium compute TU through Clang, then dump its AST."""

import argparse
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROLES = {"unpack": 0, "math": 1, "pack": 2}


def entry_arguments(entry):
    """Prefer argv arrays, where embedded quotes in macro values are literal."""
    argv = entry.get("arguments")
    if argv is None:
        argv = shlex.split(entry.get("command", ""))
    if (
        not isinstance(argv, list)
        or not argv
        or not all(isinstance(arg, str) for arg in argv)
    ):
        raise ValueError("Compilation entry needs a nonempty arguments or command")
    return list(argv)


def defines(argv):
    result = {}
    it = iter(argv[1:])
    for arg in it:
        if arg == "-D":
            arg = "-D" + next(it)
        if arg.startswith("-D"):
            key, _, value = arg[2:].partition("=")
            result[key] = value.strip('"')
    return result


def entry_directory(entry, database):
    directory = Path(entry["directory"])
    if not directory.is_absolute():
        directory = database.parent / directory
    return directory.resolve()


def select_context(entries, database, kernel, role):
    """Require consistent identity and role, then verify the generated wrapper."""
    matches = []
    available = []
    for entry in entries:
        argv = entry_arguments(entry)
        macros = defines(argv)
        directory = entry_directory(entry, database)
        source = Path(entry["file"])
        identity = macros.get("FULL_KERNEL_NAME", "").strip("/")
        if source.name != "trisck.cc" or "KERNEL_BUILD" not in macros:
            continue
        available.append(f"{identity or '(no identity)'}: {directory}")
        if kernel.strip("/") not in (identity, identity.split("/")[0]):
            continue
        role_macros = {f"UCK_CHLKC_{name.upper()}" for name in ROLES}
        if role_macros.intersection(macros) != {f"UCK_CHLKC_{role.upper()}"}:
            continue
        if macros.get("COMPILE_FOR_TRISC") != str(ROLES[role]):
            continue
        if "FW_BUILD" in macros:
            continue
        expected = Path("kernels") / identity / f"trisc{ROLES[role]}"
        if not directory.as_posix().endswith("/" + expected.as_posix()):
            raise ValueError(
                f"Kernel identity and compilation directory disagree: {directory}"
            )
        matches.append((entry, argv, directory, source, identity))
    if len(matches) != 1:
        contexts = [str(item[2]) for item in matches] or available
        raise ValueError(
            f"Expected one {kernel} {role} context, found {len(matches)}. "
            "Use --kernel NAME/VARIANT to narrow variants. Contexts:\n"
            + "\n".join(contexts)
        )
    entry, argv, directory, source, identity = matches[0]
    source = source if source.is_absolute() else directory / source
    if not source.is_file():
        raise ValueError(f"Missing translation unit: {source}")
    wrapper = directory.parent / f"chlkc_{role}.cpp"
    text = wrapper.read_text()
    includes = re.findall(r'^\s*#include\s+"([^"]+)"', text, re.MULTILINE)
    included = [
        Path(name) for name in includes if Path(name).stem == identity.split("/")[0]
    ]
    if f"#define TRISC_{role.upper()}" not in text or len(included) != 1:
        raise ValueError(
            f"Wrapper does not identify the requested kernel and role: {wrapper}"
        )
    user_source = included[0]
    if not user_source.is_absolute():
        user_source = wrapper.parent / user_source
    if not user_source.is_file():
        raise ValueError(f"Missing included kernel source: {user_source}")
    return entry, argv, directory, wrapper


def frontend_arguments(argv, directory, overlay=None):
    """Drop build outputs, preserving semantic flags and their operand boundaries."""
    out = [argv[0]]
    replaced = False
    i = 1
    with_operand = {"-o", "-MF", "-MT", "-MQ", "-MJ"}
    path_options = {"-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros"}
    while i < len(argv):
        arg = argv[i]
        if arg in with_operand:
            if i + 1 == len(argv):
                raise ValueError(f"Missing operand for {arg}")
            i += 2
            continue
        if any(
            arg.startswith(flag) and arg != flag for flag in with_operand
        ) and not arg.startswith(("-objc", "-object", "-openmp")):
            i += 1
            continue
        if arg in {"-c", "-MD", "-MMD", "-MP", "-MG", "-M", "-MM", "-fsyntax-only"}:
            i += 1
            continue
        option = next(
            (
                flag
                for flag in sorted(path_options, key=len, reverse=True)
                if arg == flag or arg.startswith(flag)
            ),
            None,
        )
        if option:
            separated = arg == option
            if separated and i + 1 == len(argv):
                raise ValueError(f"Missing operand for {arg}")
            path = argv[i + 1] if separated else arg[len(option) :]
            resolved = (directory / path).resolve()
            if (
                overlay
                and resolved.name == "include"
                and resolved.parent.name == "sfpi"
            ):
                path = str(overlay)
                replaced = True
            out.extend([option, path] if separated else [option + path])
            i += 2 if separated else 1
        else:
            out.append(arg)
            i += 1
    if overlay and not replaced:
        raise ValueError(
            "No SFPI include directory found to replace with --sfpi-overlay"
        )
    return out


def run_stage(argv, directory, output, stage):
    """Keep argument arrays and raw compiler output, including failures."""
    (output / f"{stage}.command.json").write_text(
        json.dumps({"directory": str(directory), "arguments": argv}, indent=2) + "\n"
    )
    with (output / f"{stage}.stdout").open("wb") as stdout:
        with (output / f"{stage}.stderr").open("wb") as stderr:
            try:
                status = subprocess.run(
                    argv, cwd=directory, stdout=stdout, stderr=stderr
                ).returncode
            except OSError as exc:
                stderr.write(str(exc).encode())
                status = 127
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument(
        "--database", type=Path, help="Default: CAPTURE/clang/compile_commands.json"
    )
    parser.add_argument(
        "--kernel", default="add_compute", help="Kernel name or NAME/VARIANT"
    )
    parser.add_argument("--role", choices=ROLES, default="math")
    parser.add_argument(
        "--compat-header", type=Path, help="Clang-only force-included header"
    )
    parser.add_argument(
        "--sfpi-overlay", type=Path, help="Experimental copy of SFPI include/"
    )
    parser.add_argument("--ast-filter", default="kernel_main")
    parser.add_argument("--output-dir", type=Path, help="Default: CAPTURE/replay")
    args = parser.parse_args()
    try:
        capture = args.capture.resolve()
        database = (args.database or capture / "clang/compile_commands.json").resolve()
        entries = json.loads(database.read_text())
        if not isinstance(entries, list):
            raise ValueError("Compilation database must be a JSON array")
        entry, argv, directory, wrapper = select_context(
            entries, database, args.kernel, args.role
        )
        overlay = args.sfpi_overlay.resolve() if args.sfpi_overlay else None
        if overlay and not (overlay / "sfpi.h").is_file():
            raise ValueError(f"Missing SFPI overlay header: {overlay / 'sfpi.h'}")
        argv = frontend_arguments(argv, directory, overlay)
        if args.compat_header:
            compat = args.compat_header.resolve()
            if not compat.is_file():
                raise ValueError(f"Missing compatibility header: {compat}")
            argv += ["-include", str(compat)]
        output = (args.output_dir or capture / "replay").resolve()
        output.mkdir(parents=True, exist_ok=True)
        report = {
            "database": str(database),
            "entry": entry,
            "wrapper": str(wrapper),
            "compat_header": (
                str(args.compat_header.resolve()) if args.compat_header else None
            ),
            "sfpi_overlay": str(overlay) if overlay else None,
            "syntax_exit_code": None,
            "ast_exit_code": None,
        }
        report_path = output / "replay.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        print(
            f"Working directory: {directory}\nTranslation unit: {entry['file']}\nOutput: {output}",
            flush=True,
        )
        status = run_stage(argv + ["-fsyntax-only"], directory, output, "syntax")
        report["syntax_exit_code"] = status
        print(f"Exit code: {status}", flush=True)
        if status == 0:
            status = run_stage(
                argv
                + [
                    "-fsyntax-only",
                    "-Xclang",
                    "-ast-dump",
                    "-Xclang",
                    f"-ast-dump-filter={args.ast_filter}",
                ],
                directory,
                output,
                "ast",
            )
            report["ast_exit_code"] = status
            report["ast_size_bytes"] = (output / "ast.stdout").stat().st_size
            print(
                f"AST exit code: {status}\nAST size: {report['ast_size_bytes']} bytes"
            )
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        return 0 if status == 0 else 1
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as exc:
        print(f"parse_ast: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
