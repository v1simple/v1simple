#!/usr/bin/env python3
"""Run real battery/power owners through both runtime consumers with advancing ADC time.

Complete drive power and maintenance tick methods are extracted unchanged.
GPIO, ADC, display, storage, WiFi and rail effects are adapters; no hardware runs.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

from test_battery_input_lifecycle import write_owners
from test_drive_runtime_policies import function

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test/fixtures/power_runtime_lifecycle/confirmation.cpp"


def build_and_run(build: Path, source_root: Path = ROOT) -> None:
    write_owners(build, source_root)
    drive = (source_root / "src/drive_runtime.cpp").read_text()
    maintenance = (source_root / "src/maintenance_runtime.cpp").read_text()
    (build / "runtime_power.inc").write_text(
        function(drive, "void DriveRuntime::processPower(") + "\n" +
        function(maintenance, "void MaintenanceRuntime::tick("))
    compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
    deps = ROOT / ".pio/libdeps/native-sanitized"
    includes = [build, source_root, source_root / "include", ROOT, ROOT / "include",
                ROOT / "test/mocks", deps / "Unity/src"]
    flags = ["-std=c++17", "-DUNIT_TEST", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
             *["-I" + str(path) for path in includes]]
    failures = []
    for flavor in ["portable", "car"]:
        executable = build / flavor
        flavor_flags = ["-DCAR_MODE_PWR_SHORT=1"] if flavor == "car" else []
        subprocess.run(compiler + flags + flavor_flags + [str(FIXTURE), "-x", "c++",
                       str(deps / "Unity/src/unity.c"), "-o", str(executable)], check=True)
        if subprocess.run([str(executable)]).returncode:
            failures.append(flavor)
    if failures:
        raise AssertionError("Failed power lifecycle: " + ", ".join(failures))


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="power-runtime-lifecycle-") as directory:
        build_and_run(Path(directory))
