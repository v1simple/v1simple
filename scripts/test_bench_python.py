#!/usr/bin/env python3
"""Exercise the bench launcher across terminal environments without hardware."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class BenchPythonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        scripts = self.root / "scripts"
        scripts.mkdir()
        self.launcher = scripts / "bench_python.sh"
        shutil.copyfile(Path(__file__).with_name("bench_python.sh"), self.launcher)
        import numpy
        import PIL
        import serial
        import cv2
        from _stbt.config import get_config
        import venv
        venv_root = self.root / ".artifacts/bench/python"
        venv.EnvBuilder(with_pip=False).create(venv_root)
        self.python = venv_root / "bin/python3"
        site_dir = next(venv_root.glob("lib/python*/site-packages"))
        # Reuse installed packages in this disposable fixture without downloads.
        package_dirs = sorted({str(Path(module.__file__).parent.parent)
                               for module in (numpy, PIL, serial, cv2)})
        (site_dir / "fixture-packages.pth").write_text("\n".join(package_dirs) + "\n")
        versions = json.loads(subprocess.check_output([str(self.python), "-I", "-c",
            'import importlib.metadata,json; print(json.dumps({name:importlib.metadata.version(name) for name in ("numpy","Pillow","pyserial","opencv-python","stbt-core")}))'], text=True))
        (scripts / "requirements-bench.txt").write_text(
            "".join(f"{name}=={version}\n" for name, version in versions.items()))
        self.qualification = self.root / "qualification.json"
        self.runtime = {"numpy_version": versions["numpy"], "pillow_version": versions["Pillow"],
                        "opencv_version": cv2.__version__, "stbt_core_version": versions["stbt-core"],
                        "stbt_pyramid_levels": get_config("match", "pyramid_levels", type_=int)}
        self.qualification.write_text(json.dumps({"reader": {"runtime": self.runtime}}))
        foreign = self.root / "foreign"
        foreign.mkdir()
        self.marker = self.root / "foreign-python-called"
        executable = foreign / "python3"
        executable.write_text(f'#!/bin/sh\ntouch "{self.marker}"\nexit 88\n')
        executable.chmod(0o755)
        (foreign / "numpy.py").write_text('raise RuntimeError("ambient PYTHONPATH leaked")\n')
        self.env = dict(os.environ, PATH=str(foreign) + os.pathsep + os.environ["PATH"],
                        PYTHONPATH=str(foreign), PYTHONHOME=str(foreign))

    def run_launcher(self):
        return subprocess.run(["bash", str(self.launcher), str(self.qualification)],
                              env=self.env, text=True, capture_output=True, timeout=15)

    def test_owned_runtime_ignores_terminal_python_and_import_overrides(self):
        result = self.run_launcher()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(self.python))
        self.assertFalse(self.marker.exists())

    def test_different_qualification_reports_exact_dependency_before_collection(self):
        self.runtime["numpy_version"] = "0.0.0"
        self.qualification.write_text(json.dumps({"reader": {"runtime": self.runtime}}))
        result = self.run_launcher()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("numpy_version: qualified '0.0.0', running", result.stderr)
        self.assertFalse(self.marker.exists())

    def test_missing_qualification_refuses_without_installing_or_running_terminal_python(self):
        self.qualification.unlink()
        result = self.run_launcher()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("reader environment is not qualified", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse(self.marker.exists())


if __name__ == "__main__":
    unittest.main()
