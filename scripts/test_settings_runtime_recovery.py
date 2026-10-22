#!/usr/bin/env python3
"""Exercise settings recovery through the actual periodic runtime owners.

One fixture exercises the real settings store, NVS recovery and synchronizer.
The other carries the real recovery/dispatch methods through the connection
coordinator and OBD runtime, with BLE links and peripheral replies scripted.
Maintenance also follows the real restore API through its post-restore callback,
periodic display application and a settings reload; panel writes are recorded.
Saved WiFi edits and listing exercise the actual callers against the settings
store, then carry unavailable-password metadata through the browser editor.
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
    wifi = (ROOT / "src/wifi_client.cpp").read_text(encoding="utf-8")
    (build / "wifi_credentials.inc").write_text(between(
        wifi, "std::vector<WifiClientApiService::SavedNetworkSlotPayload> WiFiManager::getSavedNetworkSlots() const",
        "bool WiFiManager::deleteSavedNetwork("), encoding="utf-8")
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
        "wifi_credentials": [ROOT / "test/test_settings_persistence/display_nvs_persistence_tests.cpp"],
    }
    for name, support in fixtures.items():
        executable = build / name
        subprocess.run(compiler + flags + [str(FIXTURE / f"{name}.cpp"),
            *[str(path) for path in support], "-x", "c++", str(deps / "Unity/src/unity.c"),
            "-o", str(executable)], check=True)
        editor_slot = build / "wifi_editor_slot.json"
        arguments = [str(editor_slot)] if name == "wifi_credentials" else []
        subprocess.run([str(executable), *arguments], check=True, env={**os.environ, "TMPDIR": str(build)})
        if name == "wifi_credentials":
            subprocess.run(["node", "--input-type=module", "-e", """
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const { openWifiEditorState, buildWifiEditorRequest } = await import(process.argv[1]);
const slot = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const editor = openWifiEditorState(slot.index, {
    mode: 'edit', ssid: slot.ssid, label: 'Renamed', priority: slot.priority,
    hasExistingPassword: slot.hasPassword
});
const request = buildWifiEditorRequest(editor);
assert.equal(request.ssid, slot.ssid);
assert.equal(request.label, 'Renamed');
assert.equal(Object.hasOwn(request, 'password'), false);
console.log('Unavailable WiFi password listing preserves blank editor credentials: PASS');
""", (ROOT / "interface/src/lib/features/settings/wifiNetworkUiModel.js").as_uri(),
                str(editor_slot)], check=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="settings-runtime-recovery-") as temporary:
        build_and_run(Path(temporary))
