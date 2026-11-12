#!/usr/bin/env python3
"""Test detector Apply deadlines, capture and connection admission owners.

Compile the actual settings-operation coordinator, AutoPush, protocol parser,
durable operation store and loop coordinator directly. Hardware effects remain
recording adapters; no operation state machine or freshness timeout is copied.
A second executable compiles the actual snapshot capture and full connection
callback with recording adapters for their hardware and persistence consumers.
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
    snapshot_owner = between(source, "V1DetectorSnapshot DriveRuntime::captureDetectorSnapshot(",
                             "SettingsOperationCoordinator::SnapshotPersistResult DriveRuntime::persistDetectorSnapshot(")
    snapshot_owner += between(source, "void DriveRuntime::onV1Connected()",
                              "void DriveRuntime::observeAlertTable(")
    (build / "detector_snapshot.inc").write_text(snapshot_owner, encoding="utf-8")
    restart_owner = between(source, "bool DriveRuntime::requestMaintenanceBootRestart()",
                             "bool DriveRuntime::usbMaintenanceAllowed()")
    (build / "maintenance_restart.inc").write_text(restart_owner, encoding="utf-8")
    header = (ROOT / "src/drive_runtime.h").read_text(encoding="utf-8")
    declarations = between(header, "    enum class SnapshotPurpose",
                           "    SettingsOperationCoordinator::Effects makeSettingsOperationEffects();")
    (build / "snapshot_declarations.inc").write_text(declarations, encoding="utf-8")
    compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
    deps = ROOT / ".pio/libdeps/native-sanitized"
    includes = [build, ROOT, ROOT / "test/mocks", ROOT / "include", ROOT / "src",
                deps / "Unity/src", deps / "ArduinoJson/src"]
    for fixture in ("recovery", "detector_snapshot", "restart", "ordinary_autopush_recovery"):
        executable = build / fixture
        fixture_includes = includes
        support = []
        if fixture == "ordinary_autopush_recovery":
            fixture_includes = [build, ROOT, ROOT / "src", ROOT / "include", ROOT / "test/mocks",
                                deps / "Unity/src", deps / "ArduinoJson/src"]
            support = [str(ROOT / "test/test_settings_persistence/display_nvs_persistence_tests.cpp")]
        subprocess.run(compiler + [
            "-std=c++17", "-DUNIT_TEST", f'-DPROJECT_DIR="{ROOT}"', "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
            *["-I" + str(path) for path in fixture_includes], str(FIXTURE / (fixture + ".cpp")), *support,
            "-x", "c++", str(deps / "Unity/src/unity.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True, env={**os.environ, "TMPDIR": str(build)})


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="settings-apply-lifecycle-") as temporary:
        build_and_run(Path(temporary))
