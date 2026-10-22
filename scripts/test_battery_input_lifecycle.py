#!/usr/bin/env python3
"""Exercise actual battery boot/input/voltage/shutdown owners with hardware adapters.

GPIO, ADC, I2C and rail/sleep effects are scripted. This checks software ordering
and policy; it does not qualify physical supply sensing, drain or wake behavior.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test/fixtures/battery_input_lifecycle"


def method(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"Missing or ambiguous method: {signature}")
    start = source.index(signature)
    first = source.index("{", start)
    depth = 1
    end = first + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def write_owners(build: Path, source_root: Path = ROOT) -> bool:
    source = (source_root / "src/battery_manager.cpp").read_text()
    signatures = [
        "BatteryManager::BatteryManager()", "bool BatteryManager::begin()",
        "bool BatteryManager::isOnBattery()", "bool BatteryManager::hasBattery()",
        "void BatteryManager::update()", "bool BatteryManager::refreshVoltage()",
        "bool BatteryManager::hasVoltageReadingAfter(", "uint16_t BatteryManager::getVoltageMillivolts()",
        "uint8_t BatteryManager::getPercentage()", "bool BatteryManager::isCritical()",
        "bool BatteryManager::criticalProtectionRequired()", "bool BatteryManager::isPowerButtonPressed()",
        "bool BatteryManager::processPowerButton()", "bool BatteryManager::powerOff()",
    ]
    # Allows the identical input regression to run against the earlier source
    # without changing the checkout or replacing the owning method under test.
    old_classifier = "battery_source_policy::Result BatteryManager::observeSourceRound("
    if old_classifier in source:
        signatures.append(old_classifier)
    (build / "battery_owners.inc").write_text("\n\n".join(method(source, s) for s in signatures))
    return old_classifier in source


def build_and_run(build: Path, source_root: Path = ROOT) -> None:
    legacy_classifier = write_owners(build, source_root)
    compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
    includes = [build, source_root, source_root / "include", ROOT / "include", ROOT / "test/mocks"]
    flags = ["-std=c++17", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-fno-omit-frame-pointer", "-g", *["-I" + str(p) for p in includes]]
    if legacy_classifier:
        flags.append("-DLEGACY_GPIO_CLASSIFIER")
    executable = build / "battery-input"
    subprocess.run(compiler + flags + [str(FIXTURE / "input.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="battery-input-lifecycle-") as directory:
        build_and_run(Path(directory))
