#!/usr/bin/env python3
"""Compile the real maintenance STA owner against a deterministic WiFi boundary.

WiFiManager's target WebServer cannot compile under the native mocks. Extract
the relevant owner methods and service tail verbatim, using unique named
boundaries; keep only unrelated hardware/settings surfaces in the adapter.
No copied implementation is checked in, and a changed boundary fails loudly.
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
    manager = (ROOT / "src/wifi_manager.h").read_text(encoding="utf-8")
    lifecycle = (ROOT / "src/wifi_manager_lifecycle.cpp").read_text(encoding="utf-8")
    settings = (ROOT / "src/settings.h").read_text(encoding="utf-8")

    (build / "settings.h").write_text(render("settings.h.in", {
        "@SLOT_METADATA@": between(settings, "inline constexpr size_t kWifiStaSlotCount", "struct WifiStaPriorityUpdate"),
    }), encoding="utf-8")
    (build / "owner.h").write_text(render("owner.h.in", {
        "@CLIENT_STATE@": between(manager, "enum WifiClientState {", "inline bool wifiUiActiveSince"),
        "@CONNECTION_FIELDS@": between(manager, "    bool apInterfaceEnabled_ =", "    enum class WifiStopPhase"),
        "@RECONNECT_FIELDS@": between(manager, "    // WiFi reconnect failure tracking", "    // Low-DMA protection state"),
    }), encoding="utf-8")

    owner = '#include "owner.h"\n'
    for start, end in (
        ("namespace {", "String WiFiManager::getAPIPAddress()"),
        ("int WiFiManager::findConfiguredSlotBySsid(", "bool WiFiManager::upsertSavedNetwork("),
        ("bool WiFiManager::connectToNetwork(", "bool WiFiManager::enableWifiClientFromSavedCredentials("),
        ("void WiFiManager::disconnectFromNetwork()", "void WiFiManager::disconnectTrackedWifiActivity("),
    ):
        owner += between(client, start, end)
    tail_start = "void WiFiManager::processWifiClientConnectPhase()"
    if client.count(tail_start) != 1:
        raise AssertionError("Connection owner tail missing or ambiguous")
    owner += client[client.index(tail_start):]

    # Includes both maintenance harvest passes, HTTP servicing, staged connects,
    # timeout cadence and status checking in their actual order, plus the final }.
    owner += "\nvoid WiFiManager::runOwnerPass() { const unsigned long now = millis();\n"
    owner += between(lifecycle,
                     "    // Continue serving HTTP while STA remains online even after AP is retired.",
                     "// ============================================================================\n// API Endpoints")
    (build / "owner.cpp").write_text(owner, encoding="utf-8")

    # Preserve the slot policy's relative ../../settings.h include. The scan
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
        str(ROOT / "src/modules/wifi/wifi_scan_result_owner.cpp"),
        str(policy_dir / "wifi_sta_slot_policy.cpp"), "-o", str(build / "recovery"),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(build / "recovery")], check=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="wifi-client-recovery-") as temporary:
        build_and_run(Path(temporary))
