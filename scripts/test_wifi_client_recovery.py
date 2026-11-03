#!/usr/bin/env python3
"""Compile the real station lifecycle and retain separate manager ordering checks.

The station owner, scan owner, enable transaction and policies compile directly.
Only manager AP/start and service-tail wiring is selected from its target-bound
translation unit; no station implementation or private state is reconstructed.
"""

from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test/fixtures/wifi_client_recovery"


def between(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise AssertionError(f"Owner boundary missing or ambiguous: {start!r}, {end!r}")
    first, last = source.index(start), source.index(end)
    if first >= last:
        raise AssertionError(f"Owner boundaries reversed: {start!r}, {end!r}")
    return source[first:last]


def render(name: str, replacements: dict[str, str]) -> str:
    source = (FIXTURE / name).read_text(encoding="utf-8")
    for marker, value in replacements.items():
        if source.count(marker) != 1:
            raise AssertionError(f"Fixture marker missing or ambiguous: {marker}")
        source = source.replace(marker, value)
    return source


def build_and_run(build: Path) -> None:
    client = (ROOT / "src/wifi_client.cpp").read_text(encoding="utf-8")
    lifecycle = (ROOT / "src/wifi_manager_lifecycle.cpp").read_text(encoding="utf-8")
    settings = (ROOT / "src/settings_types.h").read_text(encoding="utf-8")

    (build / "settings_types.h").write_text(render("settings_types.h.in", {
        "@SLOT_METADATA@": between(settings, "inline constexpr size_t kWifiStaSlotCount", "struct WifiStaPriorityUpdate"),
    }), encoding="utf-8")
    owner = '#include "owner.h"\n'
    owner += between(lifecycle, "static bool shouldUseApSta(", "static void getWifiRuntimeThresholds(")
    owner += between(lifecycle, "bool WiFiManager::startSetupMode(", "bool WiFiManager::stopSetupModeImmediate(")
    wrapper = "WifiStationLifecycle::Environment WiFiManager::stationEnvironment() const"
    if client.count(wrapper) != 1:
        raise AssertionError("Manager station adapter missing or ambiguous")
    owner += client[client.index(wrapper):]

    owner += between(client, "int WiFiManager::selectSlotForNetworkConnect(",
                     "WifiClientApiService::PriorityUpdateStatus WiFiManager::updateSavedNetworkPriorities(")
    owner += between(client, "bool WiFiManager::disableWifiClient()",
                     "WifiStationLifecycle::Environment WiFiManager::stationEnvironment() const")

    # Includes both maintenance harvest passes, HTTP servicing, staged connects,
    # timeout cadence and status checking in their actual order, plus the final }.
    owner += "\nvoid WiFiManager::runOwnerPass() { const unsigned long now = millis();\n"
    owner += between(lifecycle,
                     "    // Continue serving HTTP while STA remains online even after AP is retired.",
                     "// ============================================================================\n// API Endpoints")
    (build / "owner.cpp").write_text(owner, encoding="utf-8")

    # Preserve the slot policy's relative ../../settings_types.h include. The scan
    # owner and other production policy headers compile directly from src/.
    policy_dir = build / "modules/wifi"
    policy_dir.mkdir(parents=True)
    for name in ("wifi_sta_slot_policy.h", "wifi_sta_slot_policy.cpp"):
        shutil.copyfile(ROOT / "src/modules/wifi" / name, policy_dir / name)

    compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
    command = compiler + [
        "-std=c++17", "-DUNIT_TEST", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
        "-I" + str(build), "-I" + str(FIXTURE), "-I" + str(ROOT / "test/mocks"), "-I" + str(ROOT / "src"),
        str(FIXTURE / "recovery.cpp"), str(build / "owner.cpp"),
        str(ROOT / "src/modules/wifi/wifi_station_lifecycle.cpp"),
        str(ROOT / "src/modules/wifi/wifi_client_enable_transaction.cpp"),
        str(ROOT / "src/modules/wifi/wifi_scan_result_owner.cpp"),
        str(policy_dir / "wifi_sta_slot_policy.cpp"), "-o", str(build / "recovery"),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(build / "recovery")], check=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="wifi-client-recovery-") as temporary:
        build_and_run(Path(temporary))
