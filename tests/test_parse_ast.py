"""Context and argv tests use metadata fixtures, not a simulated SFPI SDK."""

import importlib.util
import json
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/parse_ast.py"
SPEC = importlib.util.spec_from_file_location("parse_ast", SCRIPT)
replay = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(replay)


class ReplayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.database = self.root / "compile_commands.json"
        self.source = self.root / "trisck.cc"
        self.source.touch()
        self.kernel = self.root / "add_compute.cpp"
        self.kernel.touch()

    def entry(self, variant="variant-a", role="math"):
        index = replay.ROLES[role]
        directory = self.root / "kernels/add_compute" / variant / f"trisc{index}"
        directory.mkdir(parents=True, exist_ok=True)
        (directory.parent / f"chlkc_{role}.cpp").write_text(
            f'#define TRISC_{role.upper()}\n#include "{self.kernel}"\n'
        )
        return {
            "directory": str(directory),
            "file": str(self.source),
            "arguments": [
                "clang++-20",
                "-c",
                str(self.source),
                "-I.",
                "-I..",
                "-DKERNEL_BUILD",
                f"-DUCK_CHLKC_{role.upper()}",
                f"-DCOMPILE_FOR_TRISC={index}",
                f'-DFULL_KERNEL_NAME="add_compute/{variant}/"',
            ],
        }

    def select(self, entries, kernel="add_compute", role="math"):
        return replay.select_context(entries, self.database, kernel, role)

    def test_role_and_firmware_selection(self):
        math = self.entry()
        firmware = dict(math, file=str(self.root / "trisc.cc"))
        selected = self.select([firmware, self.entry(role="pack"), math])
        self.assertEqual(selected[0], math)
        self.assertEqual(selected[2], Path(math["directory"]))

    def test_command_form_preserves_literal_macro_quotes(self):
        entry = self.entry()
        argv = entry.pop("arguments")
        entry["command"] = shlex.join(argv)
        self.assertEqual(replay.entry_arguments(entry), argv)
        self.assertEqual(self.select([entry])[0], entry)

    def test_ambiguous_variants_and_exact_identity(self):
        entries = [self.entry(), self.entry("variant-b")]
        with self.assertRaisesRegex(ValueError, "found 2"):
            self.select(entries)
        self.assertEqual(self.select(entries, "add_compute/variant-b/")[0], entries[1])
        with self.assertRaisesRegex(ValueError, "found 2"):
            self.select([entries[0], entries[0]])

    def test_missing_context_and_inconsistent_directory(self):
        with self.assertRaisesRegex(ValueError, "found 0"):
            self.select([self.entry()], "other")
        entry = self.entry()
        entry["directory"] = str(self.root / "wrong/trisc1")
        with self.assertRaisesRegex(ValueError, "disagree"):
            self.select([entry])

    def test_missing_wrapper_and_wrong_source(self):
        entry = self.entry()
        wrapper = Path(entry["directory"]).parent / "chlkc_math.cpp"
        wrapper.unlink()
        with self.assertRaises(FileNotFoundError):
            self.select([entry])
        wrapper.write_text('#define TRISC_MATH\n#include "other.cpp"\n')
        with self.assertRaisesRegex(ValueError, "Wrapper"):
            self.select([entry])

    def test_relative_translation_unit_and_include_working_directory(self):
        entry = self.entry()
        directory = Path(entry["directory"])
        (directory / "trisck.cc").touch()
        entry["file"] = "trisck.cc"
        self.assertEqual(self.select([entry])[2], directory)
        argv = replay.frontend_arguments(entry["arguments"], directory)
        self.assertIn("-I.", argv)
        self.assertIn("-I..", argv)
        # These stay relative because Clang is launched with this exact cwd.
        self.assertEqual(directory.parent, directory.joinpath("..").resolve())

    def test_output_operands_and_semantic_flags(self):
        argv = [
            "clang++",
            "-o",
            "object.o",
            "-MF",
            "deps.d",
            "-MT",
            "target",
            "-MQ",
            "quoted",
            "-MJ",
            "entry.json",
            "-oother.o",
            "-MFmore.d",
            "-MTother",
            "-MQother",
            "-MJother.json",
            "-MMD",
            "-c",
            "-include",
            "header.h",
            "-D",
            "SEMANTIC=1",
            "-std=c++20",
            "trisck.cc",
        ]
        self.assertEqual(
            replay.frontend_arguments(argv, self.root),
            [
                "clang++",
                "-include",
                "header.h",
                "-D",
                "SEMANTIC=1",
                "-std=c++20",
                "trisck.cc",
            ],
        )
        with self.assertRaisesRegex(ValueError, "operand"):
            replay.frontend_arguments(["clang++", "-MF"], self.root)

    def test_overlay_only_replaces_sfpi_include_search_paths(self):
        overlay = self.root / "overlay"
        for inc in (["-I../sfpi/include"], ["-isystem", "../sfpi/include"]):
            args = replay.frontend_arguments(
                [
                    "clang++",
                    *inc,
                    "-I../sfpi/compiler/include",
                    "-DNAME=sfpi/include",
                    "-include",
                    "../sfpi/include",
                    "-imacros../sfpi/include",
                ],
                self.root,
                overlay,
            )
            self.assertTrue(any(str(overlay) in arg for arg in args))
            self.assertIn("-I../sfpi/compiler/include", args)
            self.assertIn("-DNAME=sfpi/include", args)
            self.assertIn("../sfpi/include", args)
            self.assertIn("-imacros../sfpi/include", args)
        with self.assertRaisesRegex(ValueError, "No SFPI"):
            replay.frontend_arguments(["clang++", "-I."], self.root, overlay)

    def cli(self, *args):
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--capture",
                str(self.root),
                "--database",
                str(self.database),
                *args,
            ],
            capture_output=True,
            text=True,
        )

    def test_cli_missing_inputs_and_ambiguity(self):
        result = self.cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn("No such file", result.stderr)
        self.database.write_text(json.dumps([self.entry(), self.entry("variant-b")]))
        result = self.cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn("found 2", result.stderr)
        self.database.write_text(json.dumps([self.entry()]))
        result = self.cli("--compat-header", str(self.root / "absent.h"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("Missing compatibility", result.stderr)

    def test_failed_attempt_removes_previous_ast_even_on_invalid_input(self):
        output = self.root / "replay"
        output.mkdir()
        stale = ["ast.stdout", "ast.stderr", "ast.command.json", "replay.json"]
        for name in stale:
            (output / name).write_text("previous successful run")
        result = self.cli()
        self.assertEqual(result.returncode, 2)
        for name in stale:
            self.assertFalse((output / name).exists())
        entry = self.entry()
        entry["arguments"][0] = str(self.root / "no-compiler")
        self.database.write_text(json.dumps([entry]))
        (output / "ast.stdout").write_text("previous successful run")
        result = self.cli()
        self.assertEqual(result.returncode, 1)
        self.assertFalse((output / "ast.stdout").exists())

    def test_compiler_launch_failure_records_status(self):
        entry = self.entry()
        entry["arguments"][0] = str(self.root / "no-compiler")
        self.database.write_text(json.dumps([entry]))
        result = self.cli()
        self.assertEqual(result.returncode, 1)
        report = json.loads((self.root / "replay/replay.json").read_text())
        self.assertEqual(report["syntax_exit_code"], 127)
        self.assertIsNone(report["ast_exit_code"])
        command = json.loads((self.root / "replay/syntax.command.json").read_text())
        self.assertEqual(command["directory"], entry["directory"])
        self.assertEqual(command["arguments"][0], entry["arguments"][0])


if __name__ == "__main__":
    unittest.main()
