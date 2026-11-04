"""Verify version gates and exact patching with small text fixtures, without an SDK."""

import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/check_sfpi.py"
SPEC = importlib.util.spec_from_file_location("check_sfpi", SCRIPT)
sfpi = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sfpi)


class SfpiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.metal = self.root / "metal"
        (self.metal / "tt_metal").mkdir(parents=True)
        self.manifest = self.metal / "tt_metal/sfpi-version"
        self.manifest.write_text("sfpi_version='7.90.0'\nsfpi_build='1068'\n")
        self.sdk = self.metal / "runtime/sfpi"
        (self.sdk / "include").mkdir(parents=True)
        self.compiler = self.sdk / sfpi.COMPILER
        self.compiler.parent.mkdir(parents=True)
        self.compiler.write_text("compiler hash fixture")
        self.header = self.sdk / "include/sfpi.h"
        self.header.write_text("original constructor\n")
        self.spec = {
            "version": "7.90.0",
            "build": "1068",
            "manifest": {"sfpi_version": "7.90.0", "sfpi_build": "1068"},
            "headers_sha256": {"sfpi.h": sfpi.sha256(self.header)},
            "patch": {
                "file": "sfpi.h",
                "before": "original constructor",
                "after": "explicit constructor",
            },
        }
        banner = subprocess.CompletedProcess(
            [], 0, "riscv-tt-elf-g++ (tenstorrent/sfpi:7.90.0[1068]) 15.1.0\n", ""
        )
        self.run = patch.object(sfpi.subprocess, "run", return_value=banner).start()
        self.addCleanup(patch.stopall)

    def verify(self):
        return sfpi.verify(self.metal, self.sdk, self.spec)

    def test_version_and_build_gates(self):
        self.verify()
        self.run.return_value.stdout = (
            "riscv-tt-elf-g++ (tenstorrent/sfpi:7.90.0[999]) 15.1.0\n"
        )
        with self.assertRaisesRegex(ValueError, "compiler mismatch"):
            self.verify()
        self.run.return_value.stdout = ""
        with self.assertRaisesRegex(ValueError, "compiler mismatch"):
            self.verify()
        self.manifest.write_text("sfpi_version='7.67.0'\n")
        with self.assertRaisesRegex(ValueError, "manifest"):
            self.verify()

    def test_mixed_and_already_modified_headers_rejected(self):
        for content in ("different release", "explicit constructor\n"):
            self.header.write_text(content)
            with self.assertRaisesRegex(ValueError, "headers mismatch"):
                self.verify()

    def test_extra_or_missing_headers_rejected(self):
        extra = self.sdk / "include/subdir/extra.h"
        extra.parent.mkdir()
        extra.touch()
        with self.assertRaisesRegex(ValueError, "extra.h"):
            self.verify()
        extra.unlink()
        self.header.unlink()
        with self.assertRaisesRegex(ValueError, "sfpi.h"):
            self.verify()

    def test_overlay_preserves_original_and_records_hashes(self):
        report = self.verify()
        dest = self.root / "overlay"
        sfpi.make_overlay(self.sdk, dest, self.spec, report)
        self.assertEqual(self.header.read_text(), "original constructor\n")
        self.assertEqual(
            (dest / "include/sfpi.h").read_text(), "explicit constructor\n"
        )
        saved = json.loads((dest / "manifest.json").read_text())
        self.assertEqual(
            saved["patched_sfpi_sha256"], sfpi.sha256(dest / "include/sfpi.h")
        )
        with self.assertRaisesRegex(ValueError, "already exists"):
            sfpi.make_overlay(self.sdk, dest, self.spec, report)

    def test_nonunique_transformation_rejected_without_output(self):
        self.header.write_text("original constructor\noriginal constructor\n")
        with self.assertRaisesRegex(ValueError, "transformation"):
            sfpi.make_overlay(self.sdk, self.root / "overlay", self.spec, {})
        self.assertFalse((self.root / "overlay").exists())

    def test_local_runtime_precedence(self):
        self.assertEqual(sfpi.selected_root(self.metal), self.sdk.resolve())


if __name__ == "__main__":
    unittest.main()
