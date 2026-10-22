#!/usr/bin/env python3
"""Run display scheduling through production drive-loop adapters and painters.

The coordinator, ALP parser/event bus, orchestration gates, connection cadence,
owner pipeline, touch reader, gesture recognition, profile admission and status
painters are real. Hardware observations, persistence admission and unrelated
loop phases are scripted; this proves transfer requests, not physical pixels.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test/fixtures/display_scheduler/scheduler.cpp"


def between(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise AssertionError(f"Missing or ambiguous boundary: {start!r}, {end!r}")
    first, last = source.index(start), source.index(end)
    if first >= last:
        raise AssertionError("Source boundaries reversed")
    return source[first:last]


def build_and_run(build: Path, orchestration_source: str | None = None) -> None:
    source = (ROOT / "src/drive_runtime.cpp").read_text(encoding="utf-8")
    owner = between(source, "void DriveRuntime::presentConnectionState(",
                    "void DriveRuntime::processPower(")
    owner += between(source, "DriveRuntime::DisplayEdges DriveRuntime::consumeDisplayEdges()",
                     "void DriveRuntime::finishSettingsRecapture(")
    # Use the actual dispatch watchdog value, not a fixture's approximation.
    owner = next(line for line in source.splitlines()
                 if line.startswith("constexpr uint32_t kConnectionStateProcessMaxGapMs =")) + "\n" + owner
    (build / "drive_display.inc").write_text(owner, encoding="utf-8")
    selection = between(source, "bool DriveRuntime::beginTripleTapProfileCycle(",
                        "void DriveRuntime::queueSettingsApply(")
    selection += between(source, "void DriveRuntime::processTapGesture(",
                         "void DriveRuntime::openBootReadyGate(")
    (build / "profile_cycle.inc").write_text(selection, encoding="utf-8")
    source = orchestration_source or (ROOT / "src/modules/display/display_orchestration_module.cpp").read_text(
        encoding="utf-8")
    orchestration = between(source, "void DisplayOrchestrationModule::begin(",
                            "void DisplayOrchestrationModule::syncQuietPresentation()")
    # Quiet policy is disabled in this fixture. All scheduling gates below it
    # are extracted unchanged, so the fixture cannot force a parsed refresh.
    orchestration += source[source.index("void DisplayOrchestrationModule::processEarly("):]
    (build / "orchestration.inc").write_text(orchestration, encoding="utf-8")
    source = (ROOT / "src/display.cpp").read_text(encoding="utf-8")
    (build / "display_context.inc").write_text(
        between(source, "void V1Display::setBleContext(", "void V1Display::flush()"), encoding="utf-8")
    compiler = shlex.split(os.environ.get("CXX", "clang++" if shutil.which("clang++") else "c++"))
    deps = ROOT / ".pio/libdeps/native-sanitized"
    includes = [build, ROOT, ROOT / "src", ROOT / "include", ROOT / "test/mocks",
                deps / "Unity/src", deps / "ArduinoJson/src"]
    flags = ["-std=c++17", "-DUNIT_TEST", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
             *["-I" + str(path) for path in includes]]
    executable = build / "scheduler"
    subprocess.run(compiler + flags + [str(FIXTURE), "-x", "c++", str(deps / "Unity/src/unity.c"),
                                     "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="display-scheduler-") as temporary:
        build_and_run(Path(temporary))
