#!/usr/bin/env python3
"""Exercise the raw bench Python launcher without hardware."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import venv


class BenchPythonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        scripts = self.root / "scripts"
        scripts.mkdir()
        self.launcher = scripts / "bench_python.sh"
        shutil.copyfile(Path(__file__).with_name("bench_python.sh"), self.launcher)
        self.launcher.chmod(0o755)
        shutil.copyfile(Path(__file__).with_name("requirements-bench.txt"),
                        scripts / "requirements-bench.txt")

        import serial

        environment = self.root / ".artifacts/bench-runtime/python"
        venv.EnvBuilder(with_pip=False).create(environment)
        self.python = environment / "bin/python3"
        site_dir = next(environment.glob("lib/python*/site-packages"))
        site_dir.joinpath("fixture-packages.pth").write_text(
            str(Path(serial.__file__).parent.parent) + "\n", encoding="utf-8"
        )

        foreign = self.root / "foreign"
        foreign.mkdir()
        self.marker = self.root / "foreign-python-called"
        executable = foreign / "python3"
        executable.write_text(
            f'#!/bin/sh\ntouch "{self.marker}"\nexit 88\n', encoding="utf-8"
        )
        executable.chmod(0o755)
        self.environment = dict(
            os.environ,
            PATH=str(foreign) + os.pathsep + os.environ["PATH"],
            PYTHONPATH=str(foreign),
            PYTHONHOME=str(foreign),
        )

    def test_owned_runtime_ignores_terminal_python_and_import_overrides(self):
        result = subprocess.run(
            ["bash", str(self.launcher)],
            env=self.environment,
            text=True,
            capture_output=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(self.python))
        self.assertFalse(self.marker.exists())


if __name__ == "__main__":
    unittest.main()
