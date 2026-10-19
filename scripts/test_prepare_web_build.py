#!/usr/bin/env python3
"""Exercise first-build ordering with real deployment code and isolated tool stubs."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class FreshBuildTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="fresh build ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in (
            "build.sh", "scripts/prepare_web_build.py", "interface/package.json",
            "interface/scripts/deploy.js", "interface/scripts/audio-manifest.js",
            "interface/scripts/littlefs-capacity.py", "platformio.ini", "partitions_v1.csv",
        ):
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        (self.root / "scripts/platformio_ca_bundle.sh").write_text(":\n")
        (self.root / "scripts/check_platformio_core_version.py").write_text("raise SystemExit(0)\n")
        for name in ("interface/build", "interface/node_modules", "config", "bin", "core"):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        (self.root / "interface/build/index.html").write_text("fixture")
        (self.root / "config/audio_asset_manifest.json").write_text('{"files":[]}')
        self.log = self.root / "calls.jsonl"
        self.python = self.root / "bin/selected-python"
        self.pio = self.root / "bin/selected-pio"
        self.write_tool(self.python, f'''#!{sys.executable}
import json, os, subprocess, sys
from pathlib import Path
with Path({str(self.log)!r}).open('a') as log:
    log.write(json.dumps(['python', *sys.argv[1:]]) + '\\n')
if sys.argv[1] == '-c':
    print(os.environ.get('FIXTURE_LITTLEFS_VERSION', '0.18.0'))
    raise SystemExit(0)
environment = os.environ.copy()
environment['PYTHONPATH'] = {str(self.root / "fake-python")!r}
raise SystemExit(subprocess.call([{sys.executable!r}, '-S', *sys.argv[1:]], env=environment))
''')
        self.write_tool(self.pio, f'''#!{sys.executable}
import json, os, sys
from pathlib import Path
with Path({str(self.log)!r}).open('a') as log:
    log.write(json.dumps(['pio', *sys.argv[1:]]) + '\\n')
if sys.argv[1:] == ['system', 'info', '--json-output']:
    print(json.dumps({{"python_exe": {{"value": {str(self.python)!r}}}, "core_dir": {{"value": {str(self.root / "core")!r}}}}}))
elif sys.argv[1:3] == ['pkg', 'install']:
    if os.environ.get('FIXTURE_PACKAGE_FAILURE'): raise SystemExit(42)
    core = Path({str(self.root / "core")!r})
    builder = core / 'platforms/espressif32/builder/main.py'
    builder.parent.mkdir(parents=True)
    builder.write_text('env["FS_BLOCK"] = int("0x1000", 16)')
    sdk = core / 'packages/framework-arduinoespressif32-libs/esp32s3/qio_opi/include/sdkconfig.h'
    sdk.parent.mkdir(parents=True)
    sdk.write_text('#define CONFIG_LITTLEFS_OBJ_NAME_LEN 64')
elif sys.argv[1:2] != ['run']:
    raise SystemExit('unexpected PIO action')
''')
        node = shutil.which("node")
        self.assertIsNotNone(node, "the deployment fixture needs an installed Node executable")
        self.write_tool(self.root / "bin/npm", f'''#!{sys.executable}
import json, subprocess, sys
from pathlib import Path
with Path({str(self.log)!r}).open('a') as log:
    log.write(json.dumps(['npm', *sys.argv[1:]]) + '\\n')
if sys.argv[1:] == ['run', 'deploy:built']:
    raise SystemExit(subprocess.call([{node!r}, 'scripts/deploy.js']))
if sys.argv[1:] != ['run', 'build']: raise SystemExit('unexpected npm action')
''')
        package = self.root / "fake-python/littlefs"
        package.mkdir(parents=True)
        (package / "__init__.py").write_text('''from io import BytesIO
class LittleFS:
    def __init__(self, **kwargs): pass
    def format(self): pass
    def mount(self): pass
    def unmount(self): pass
    def makedirs(self, *args, **kwargs): pass
    def setattr(self, *args): pass
    def open(self, *args): return BytesIO()
''')
        (package / "errors.py").write_text("class LittleFSError(Exception): pass\n")
        self.environment = os.environ.copy()
        self.environment.pop("PYTHONPATH", None)
        self.environment.pop("PYTHONHOME", None)
        self.environment.update(PATH=f"{self.root / 'bin'}{os.pathsep}{self.environment['PATH']}", PIO_CMD=str(self.pio))

    @staticmethod
    def write_tool(path: Path, source: str) -> None:
        path.write_text(source)
        path.chmod(0o755)

    def run_build(self, *arguments: str) -> tuple[subprocess.CompletedProcess[str], list[list[str]]]:
        result = subprocess.run(["bash", str(self.root / "build.sh"), *arguments], cwd=self.root,
                                env=self.environment, text=True, capture_output=True, timeout=30)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, calls

    def test_fresh_build_prepares_packages_before_real_deployment(self) -> None:
        result, calls = self.run_build("--env", "esp32-s3-car-install")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("reserve preserved", result.stdout)
        self.assertEqual(calls[0], ["pio", "system", "info", "--json-output"])
        self.assertEqual(calls[1][:2], ["python", "-c"])
        self.assertEqual(calls[2], ["pio", "pkg", "install", "-e", "esp32-s3-car-install"])
        self.assertEqual(calls[3:5], [["npm", "run", "build"], ["npm", "run", "deploy:built"]])
        self.assertEqual(calls[5], ["pio", "system", "info", "--json-output"])
        self.assertTrue(calls[6][1].endswith("littlefs-capacity.py"), calls)
        self.assertEqual(calls[7], ["pio", "run", "-e", "esp32-s3-car-install"])

    def test_missing_or_wrong_littlefs_fails_before_frontend_or_packages(self) -> None:
        for version in ("", "0.17.0"):
            with self.subTest(version=version):
                self.environment["FIXTURE_LITTLEFS_VERSION"] = version
                result, calls = self.run_build()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(shlex.quote(str(self.python)), result.stderr)
                self.assertIn("-m pip install littlefs-python==0.18.0", result.stderr)
                self.assertEqual(len(calls), 2)
                self.log.unlink()

    def test_project_package_failure_blocks_deployment(self) -> None:
        self.environment["FIXTURE_PACKAGE_FAILURE"] = "1"
        result, calls = self.run_build()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls[-1], ["pio", "pkg", "install", "-e", "waveshare-349"])
        self.assertFalse(any(call[0] == "npm" for call in calls))

    def test_skip_web_does_not_require_web_preparation(self) -> None:
        self.environment["FIXTURE_LITTLEFS_VERSION"] = ""
        result, calls = self.run_build("--skip-web")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls, [["pio", "run", "-e", "waveshare-349"],
                                 ["pio", "run", "-e", "waveshare-349", "-t", "size"]])


if __name__ == "__main__":
    unittest.main(verbosity=2)
