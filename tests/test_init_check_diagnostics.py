"""Exercise diagnostic presentation with a real, explicitly supplied JIT capture."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL = """#include "api/compute/common.h"
#include "api/compute/tile_move_copy.h"
#include "api/compute/eltwise_binary.h"
#include "api/compute/compute_kernel_api.h"
void kernel_main() {
    compute_kernel_hw_startup(0, 1, 16);
    add_init(0, 1);
    add_tiles(0, 1, 0, 0, 0);
}
"""


class InitCheckDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        capture = os.environ.get("TTKL_TEST_CAPTURE_LOG")
        if not capture:
            raise unittest.SkipTest(
                "set TTKL_TEST_CAPTURE_LOG to a real TT-Metal JIT log"
            )
        cls.original_log = Path(capture).resolve()
        cls.binary = Path(
            os.environ.get("TTKL_INIT_CHECK", ROOT / "build/tt-init-check")
        )
        if not cls.binary.is_file() or not cls.original_log.is_file():
            raise unittest.SkipTest(
                "build tt-init-check and supply an existing capture"
            )

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.kernel = self.directory / "kernel.cpp"
        self.kernel.write_text(KERNEL)
        self.capture = self.directory / "capture"
        original = self.original_log.parent
        # Copy generated headers and wrappers, not device binaries or the SDK.
        for source in original.rglob("*"):
            target = self.capture / source.relative_to(original)
            if source.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            elif source.suffix in {".h", ".hpp", ".cpp", ".cc"}:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
                if source.name.startswith("chlkc_") and source.suffix == ".cpp":
                    text = target.read_text().replace(
                        str(ROOT / "example_kernel/device/compute/add_compute.cpp"),
                        str(self.kernel),
                    )
                    target.write_text(text)
        self.log = self.capture / "jit.log"
        self.log.write_text(
            self.original_log.read_text().replace(str(original), str(self.capture))
        )

    def check(self, quiet=True):
        command = [str(self.binary), "--input-log", str(self.log), "--role", "math"]
        if quiet:
            command.append("--quiet-sdk-warnings")
        return subprocess.run(command, capture_output=True, text=True, check=False)

    def full_diagnostics(self, result):
        match = re.search(r"^Clang diagnostics: (.+)$", result.stderr, re.M)
        self.assertIsNotNone(match, result.stderr)
        path = Path(match[1])
        self.assertEqual(path.parent, self.capture)
        return path.read_text()

    def test_known_sdk_warnings_are_saved_without_terminal_noise(self):
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("No initialization issues found", result.stdout)
        self.assertNotIn("warning:", result.stderr)
        self.assertNotIn("warnings generated", result.stderr)
        raw = self.full_diagnostics(result)
        self.assertIn("unknown attribute 'rvtt_l1_ptr'", raw)
        self.assertIn("unknown attribute 'rvtt_reg_ptr'", raw)
        self.assertIn("cannot be applied to a type", raw)
        self.assertIn("loop header and in the loop body", raw)
        self.assertIn("note: expanded from macro", raw)

    def test_default_output_still_shows_sdk_warnings(self):
        result = self.check(quiet=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("warning: unknown attribute 'rvtt_l1_ptr'", result.stderr)
        self.assertNotIn("Clang diagnostics:", result.stderr)

    def test_missing_initializer_still_reports_a_finding_and_fails(self):
        self.kernel.write_text(KERNEL.replace("    add_init(0, 1);\n", ""))
        result = self.check()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(str(self.kernel), result.stdout)
        self.assertIn("add_tiles has no matching add_init", result.stdout)
        self.assertNotIn("No initialization issues found", result.stdout)
        self.assertNotIn("warning:", result.stderr)

    def test_wrong_initializer_still_reports_a_finding_and_fails(self):
        self.kernel.write_text(KERNEL.replace("add_init(0, 1)", "mul_init(0, 1)"))
        result = self.check()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("active: mul_init", result.stdout)
        self.assertNotIn("No initialization issues found", result.stdout)

    def test_same_attribute_warning_in_user_code_stays_visible(self):
        self.kernel.write_text("[[gnu::rvtt_l1_ptr]] void user_function();\n" + KERNEL)
        result = self.check()
        self.assertIn("warning: unknown attribute 'rvtt_l1_ptr'", result.stderr)
        self.assertIn(str(self.kernel), result.stderr)
        self.assertIn("unknown attribute 'rvtt_l1_ptr'", self.full_diagnostics(result))

    def test_same_loop_warning_in_user_code_stays_visible(self):
        self.kernel.write_text(
            KERNEL.replace(
                "    add_tiles(0, 1, 0, 0, 0);",
                "    for (int t = 0; t < 8; t++) { add_tiles(0, 1, 0, 0, 0); t++; }",
            )
        )
        result = self.check()
        self.assertIn("loop header and in the loop body", result.stderr)
        self.assertIn(str(self.kernel), result.stderr)

    def test_parser_errors_stay_visible_and_never_print_success(self):
        self.kernel.write_text(KERNEL + "void invalid( {\n")
        result = self.check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("error:", result.stderr)
        self.assertIn("analysis could not complete", result.stderr)
        self.assertNotIn("No initialization issues found", result.stdout)
        self.assertIn("error:", self.full_diagnostics(result))

    def test_sdk_warnings_promoted_to_errors_stay_visible_and_fail(self):
        self.kernel.write_text(
            '#pragma clang diagnostic error "-Wunknown-attributes"\n' + KERNEL
        )
        result = self.check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("error: unknown attribute 'rvtt_", result.stderr)
        self.assertIn("analysis could not complete", result.stderr)
        self.assertNotIn("No initialization issues found", result.stdout)

    def test_translation_failure_stays_visible(self):
        self.log.write_text(
            self.log.read_text().replace("-mcpu=tt-wh-tensix", "-mcpu=unknown-cpu")
        )
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown-cpu", result.stderr)
        self.assertIn("analysis could not complete", result.stderr)
        self.assertNotIn("No initialization issues found", result.stdout)


if __name__ == "__main__":
    unittest.main()
