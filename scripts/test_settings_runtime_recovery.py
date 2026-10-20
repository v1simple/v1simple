#!/usr/bin/env python3
"""Exercise delayed settings recovery through the actual periodic runtime owner.

One fixture exercises the real settings store, NVS recovery and synchronizer.
The other carries the real recovery/dispatch methods through the connection
coordinator and OBD runtime, with BLE links and peripheral replies scripted.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test/fixtures/settings_runtime_recovery"


def between(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise AssertionError(f"Missing or ambiguous runtime boundary: {start!r}, {end!r}")
    first, last = source.index(start), source.index(end)
    if first >= last:
        raise AssertionError("Runtime boundaries reversed")
    return source[first:last]


def build_and_run(build: Path) -> None:
    source = (ROOT / "src/drive_runtime.cpp").read_text(encoding="utf-8")
    recovery = between(source, "void DriveRuntime::syncRecoveredSettings()",
                       "void DriveRuntime::finalizeBoot(")
    owner = recovery
    owner += between(source, "void DriveRuntime::processPeriodicMaintenance(",
                     "void DriveRuntime::logDisplayConfiguration(")
    (build / "runtime_recovery.inc").write_text(owner, encoding="utf-8")
    connection = recovery + between(source, "void DriveRuntime::processConnectionCycle(",
                                    "void DriveRuntime::processAlp(")
    (build / "connection_recovery.inc").write_text(connection, encoding="utf-8")
    compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
    deps = ROOT / ".pio/libdeps/native-sanitized"
    includes = [build, ROOT, ROOT / "src", ROOT / "include", ROOT / "test/mocks",
                deps / "Unity/src", deps / "ArduinoJson/src"]
    flags = [
        "-std=c++17", "-DUNIT_TEST", f'-DPROJECT_DIR="{ROOT}"',
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-fno-omit-frame-pointer", "-g", *["-I" + str(path) for path in includes],
    ]
    fixtures = {
        "recovery": [ROOT / "test/test_settings_persistence/display_nvs_persistence_tests.cpp"],
        "connection_admission": [],
    }
    for name, support in fixtures.items():
        executable = build / name
        subprocess.run(compiler + flags + [str(FIXTURE / f"{name}.cpp"),
            *[str(path) for path in support], "-x", "c++", str(deps / "Unity/src/unity.c"),
            "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="settings-runtime-recovery-") as temporary:
        build_and_run(Path(temporary))
