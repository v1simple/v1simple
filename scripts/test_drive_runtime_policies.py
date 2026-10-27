#!/usr/bin/env python3
"""Exercise drive policy consumers with real coordinator/owners and platform adapters.

ALP parses injected UART frames; slider-tail uses real touch, BLE queue and parser.
Slider-OBD keeps polling and speed freshness live through actual local input.
Speed uses canonical parser frames with recorded detector writes. Production
runtime adapters are extracted verbatim to retain their actual ordering/tail.
No radio, display panel, physical volume or power cutoff is exercised.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "test/fixtures/drive_runtime_policies"


def function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"Missing or ambiguous runtime boundary: {signature}")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise AssertionError(signature)


def run(build: Path, names: list[str], source_root: Path = ROOT) -> None:
    source = (source_root / "src/drive_runtime.cpp").read_text()
    alp = "\n\n".join(function(source, signature) for signature in [
        "void DriveRuntime::processAlpPresentationAndPower(",
        "void DriveRuntime::processPower(", "void DriveRuntime::processAlp("])
    (build / "alp_power.inc").write_text(alp)
    tail = "\n\n".join(function(source, signature) for signature in [
        "uint32_t DriveRuntime::finishLoop(", "void DriveRuntime::finishDriveLoop(",
        "bool DriveRuntime::preemptSettingsForLiveAlert(",
        "bool DriveRuntime::hasLiveAlertPresentation("])
    (build / "slider_tail.inc").write_text(tail.replace("DriveRuntime::", "Runtime::"))
    recovery_constant = next(line for line in source.splitlines()
                             if line.startswith("constexpr uint32_t kConnectionStateProcessMaxGapMs = "))
    recovery = recovery_constant + "\n" + "\n\n".join(function(source, signature) for signature in [
        "void DriveRuntime::processBleRuntime(", "DriveLoopDispatch DriveRuntime::processConnectionDispatch("])
    (build / "alert_recovery.inc").write_text(recovery.replace("DriveRuntime::", "RecoveryRuntime::"))
    policies = function(source, "bool DriveRuntime::processParsedPolicies(")
    (build / "slider_policies.inc").write_text(policies.replace("DriveRuntime::", "SliderRuntime::"))
    shared = (source_root / "src/main_setup_helpers.cpp").read_text()
    persistence = function(shared, "bool preparePersistenceForShutdown(")
    persistence = persistence.replace("ProductEventLog&", "ShutdownEvents&").replace("HealthJournal&", "ShutdownHealth&")
    (build / "shutdown_persistence.inc").write_text(persistence)
    (build / "shutdown_drive.inc").write_text(function(source, "bool DriveRuntime::preparePersistenceForShutdownPhase("))
    compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
    deps = ROOT / ".pio/libdeps/native-sanitized"
    flags = ["-std=c++17", "-DUNIT_TEST", f'-DPROJECT_DIR="{ROOT}"',
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
             *["-I" + str(p) for p in [build, source_root, source_root / "src", source_root / "include",
                                     ROOT, ROOT / "src", ROOT / "include",
                                     ROOT / "test/mocks", deps / "Unity/src", deps / "ArduinoJson/src"]]]
    failures = []
    for name in names:
        exe = build / name
        support = [str(ROOT / "test/test_settings_persistence/display_nvs_persistence_tests.cpp")] if name == "shutdown" else []
        subprocess.run(compiler + flags + [str(FIXTURES / f"{name}.cpp"), *support, "-x", "c++",
                       str(deps / "Unity/src/unity.c"), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], env={**os.environ, "TMPDIR": str(build)})
        if result.returncode:
            failures.append(name)
    if failures:
        raise SystemExit("Failed: " + ", ".join(failures))


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="drive-runtime-policies-") as temporary:
        run(Path(temporary), sys.argv[1:] or ["alp_power", "slider_speed", "slider_obd", "slider_tail", "alert_recovery", "shutdown"])
