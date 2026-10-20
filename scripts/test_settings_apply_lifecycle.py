#!/usr/bin/env python3
"""Test detector Apply deadlines across the actual drive-loop owners.

Compile AutoPush, its protocol parser, the durable operation store and the loop
coordinator directly. Extract the outer operation method and periodic owner
ordering verbatim; only unrelated hardware/recapture collaborators are modeled.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test/fixtures/settings_apply_lifecycle"


def between(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise AssertionError(f"Owner boundary missing or ambiguous: {start!r}, {end!r}")
    first, last = source.index(start), source.index(end)
    if first >= last:
        raise AssertionError(f"Owner boundaries reversed: {start!r}, {end!r}")
    return source[first:last]


def build_and_run(build: Path) -> None:
    source = (ROOT / "src/drive_runtime.cpp").read_text(encoding="utf-8")
    periodic = "void DriveRuntime::processPeriodicMaintenance("
    owner = between(source, "void DriveRuntime::processSettingsOperation(uint32_t nowMs)", periodic)
    owner += between(source, periodic, "    const bool hardPressure = bleBackpressure") + "}\n"
    owner += between(source, "void DriveRuntime::start(", "    initializeStorageAndProfiles();").replace(
        "esp_reset_reason_t", "int") + "}\n"
    owner += between(source, "void DriveRuntime::attemptFactoryReset(",
                     "bool DriveRuntime::handleSettingsOperationStableConnection()")
    (build / "outer_operation.inc").write_text(owner, encoding="utf-8")
    compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
    deps = ROOT / ".pio/libdeps/native-sanitized"
    includes = [build, ROOT, ROOT / "test/mocks", ROOT / "include", ROOT / "src",
                deps / "Unity/src", deps / "ArduinoJson/src"]
    executable = build / "recovery"
    subprocess.run(compiler + [
        "-std=c++17", "-DUNIT_TEST", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
        *["-I" + str(path) for path in includes], str(FIXTURE / "recovery.cpp"),
        str(deps / "Unity/src/unity.c"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="settings-apply-lifecycle-") as temporary:
        build_and_run(Path(temporary))
