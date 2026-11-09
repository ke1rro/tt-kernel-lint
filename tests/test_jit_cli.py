"""Check launcher mode routing without building or accessing hardware."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class JitCliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.log = self.directory / "arguments.txt"
        for name in ("cmake", "python3", "env"):
            stub = self.directory / name
            stub.write_text(
                '#!/bin/sh\nprintf "%s\\n" "$@" > "$ROUTE_LOG"\n'
                if name == "env"
                else "#!/bin/sh\nexit 0\n"
            )
            stub.chmod(0o755)
        self.environment = dict(
            os.environ,
            PATH=str(self.directory) + os.pathsep + os.environ["PATH"],
            ROUTE_LOG=str(self.log),
        )

    def launch(self, script, *args):
        self.log.unlink(missing_ok=True)
        return subprocess.run(
            [str(ROOT / script), *args],
            env=self.environment,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_new_names_defaults_and_compatibility_routing(self):
        modes = {
            (): "--jit-without-device",
            ("--jit-without-device",): "--jit-without-device",
            ("--jit-with-device",): "--jit-with-device",
            ("--device",): "--jit-with-device",
            ("--compile-only",): "--jit-with-device",
            ("--mock-compile-only",): "--jit-without-device",
        }
        for script in ("jit_example.sh",):
            for arguments, expected in modes.items():
                with self.subTest(script=script, arguments=arguments):
                    result = self.launch(script, *arguments)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    routed = self.log.read_text().splitlines()
                    self.assertEqual(
                        routed[-2:], ["./build-kernels/example_kernel", expected]
                    )

    def test_help_and_invalid_modes_do_not_launch_application(self):
        for script in ("jit_example.sh",):
            result = self.launch(script, "--help")
            self.assertEqual(result.returncode, 0)
            self.assertIn("--jit-without-device", result.stdout)
            self.assertIn("--jit-with-device", result.stdout)
            self.assertNotIn("--mock-compile-only", result.stdout)
            self.assertFalse(self.log.exists())
            for args in (("--unknown",), ("--jit-with-device", "--jit-without-device")):
                result = self.launch(script, *args)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(self.log.exists())

    def test_execution_launcher_rejects_jit_flags(self):
        for mode in ("--jit-without-device", "--jit-with-device"):
            result = self.launch("run_example_kernel.sh", mode)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(self.log.exists())
        result = self.launch("run_example_kernel.sh")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(
            self.log.read_text().splitlines()[-1], "./build-kernels/example_kernel"
        )


if __name__ == "__main__":
    unittest.main()
