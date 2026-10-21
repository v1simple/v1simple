#!/usr/bin/env python3
"""Exercise settings recovery through the actual periodic runtime owners.

One fixture exercises the real settings store, NVS recovery and synchronizer.
The other carries the real recovery/dispatch methods through the connection
coordinator and OBD runtime, with BLE links and peripheral replies scripted.
Maintenance also follows the real restore API through its post-restore callback,
periodic display application and a settings reload; panel writes are recorded.
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
    maintenance = (ROOT / "src/maintenance_runtime.cpp").read_text(encoding="utf-8")
    maintenance_owner = between(maintenance,
                                "void MaintenanceRuntime::initializeTouchAndDisplayControls()",
                                "bool MaintenanceRuntime::preservedPanicEvidencePresent(")
    maintenance_owner += between(maintenance, "void MaintenanceRuntime::tick(uint32_t nowMs)",
                                 "void MaintenanceRuntime::stop()")
    (build / "maintenance_recovery.inc").write_text(maintenance_owner, encoding="utf-8")
    runtimes = (ROOT / "src/wifi_runtimes.cpp").read_text(encoding="utf-8")
    callback = between(runtimes, "        // syncAfterRestore\n", "        // ctx\n")
    callback = callback.split("[](void* ctx) {", 1)[1].rsplit("},", 1)[0]
    (build / "maintenance_restore_sync.inc").write_text(
        "void actualPostRestore(void* ctx) {" + callback + "}\n", encoding="utf-8")
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
        "maintenance": [ROOT / "test/test_settings_persistence/display_nvs_persistence_tests.cpp"],
    }
    for name, support in fixtures.items():
        executable = build / name
        subprocess.run(compiler + flags + [str(FIXTURE / f"{name}.cpp"),
            *[str(path) for path in support], "-x", "c++", str(deps / "Unity/src/unity.c"),
            "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, env={**os.environ, "TMPDIR": str(build)})


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="settings-runtime-recovery-") as temporary:
        build_and_run(Path(temporary))
