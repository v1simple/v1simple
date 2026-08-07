#!/usr/bin/env python3
"""Focused process-lifecycle tests for the unified bench window collector."""

from __future__ import annotations

import os
import stat
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from camera_capture import CameraCapture  # noqa: E402
from run_window import V1Emulator, encounter_csv_sd_path, wait_for_post_upload_settle  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_dummy_emulator(
    path: Path,
    *,
    emit_complete: bool,
    blink_profile: str = "",
    blink_samples: int = 0,
) -> None:
    configured = "true"
    if blink_profile:
        source = "generated_multi_alert_assumption" if blink_profile == "scenario" else "explicit_control"
        configured = (
            "echo 'V1REPLAY_EVENT "
            f'{{"state":"configured","blinkProfile":"{blink_profile}",'
            f'"blinkSource":"{source}","blinkSamples":{blink_samples},'
            '"totalSamples":762,"cadenceHz":3}'
            "'"
        )
    marker = 'echo \'V1REPLAY_EVENT {"state":"complete"}\'' if emit_complete else "true"
    path.write_text(
        "#!/bin/sh\n"
        "echo argv=$*\n"
        f"{configured}\n"
        f"{marker}\n"
        "trap 'exit 0' TERM INT\n"
        "while :; do sleep 1; done\n",
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def test_idle_emulator_covers_and_stops_with_window() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(executable, emit_complete=False)
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        time.sleep(0.1)
        assert_true(emulator.health_problem() == "", "idle emulator exited before the window")
        result = emulator.finish(window_completed=True)
        assert_true(result["completed"] is True, f"idle emulator did not cover window: {result}")
        assert_true(result["mode"] == "idle", f"wrong core emulator mode: {result}")
        assert_true(result["managed_stop"] is True, f"runner did not own cleanup: {result}")
        assert_true(emulator.process is not None and emulator.process.poll() is not None, "emulator survived cleanup")


def test_failed_window_still_stops_emulator() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(executable, emit_complete=False)
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        time.sleep(0.1)
        result = emulator.finish(window_completed=False)
        assert_true(result["completed"] is False, f"failed collection was marked complete: {result}")
        assert_true(result["managed_stop"] is True, f"failed collection skipped cleanup: {result}")
        assert_true(emulator.process is not None and emulator.process.poll() is not None, "emulator survived failed collection")


def test_replay_requires_machine_completion_before_managed_stop() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=True,
            blink_profile="scenario",
            blink_samples=57,
        )
        emulator = V1Emulator(executable, root / "replay", "replay")
        emulator.start()
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline and not emulator._bench_completed():
            time.sleep(0.02)
        result = emulator.finish(window_completed=True)
        assert_true(result["completed"] is True, f"completion marker was not honored: {result}")
        assert_true(result["mode"] == "bench", f"wrong replay mode: {result}")

        missing_root = root / "missing"
        missing_executable = missing_root / "v1replay"
        missing_root.mkdir()
        write_dummy_emulator(
            missing_executable,
            emit_complete=False,
            blink_profile="scenario",
            blink_samples=57,
        )
        missing = V1Emulator(missing_executable, missing_root / "out", "replay")
        missing.start()
        time.sleep(0.1)
        missing_result = missing.finish(window_completed=True)
        assert_true(missing_result["completed"] is False, f"incomplete replay passed: {missing_result}")

        unconfigured_root = root / "unconfigured"
        unconfigured_executable = unconfigured_root / "v1replay"
        unconfigured_root.mkdir()
        write_dummy_emulator(unconfigured_executable, emit_complete=True)
        unconfigured = V1Emulator(unconfigured_executable, unconfigured_root / "out", "replay")
        unconfigured.start()
        time.sleep(0.1)
        unconfigured_result = unconfigured.finish(window_completed=True)
        assert_true(
            unconfigured_result["completed"] is False,
            f"replay without blink provenance passed: {unconfigured_result}",
        )


def test_replay_blink_profile_argv_and_result() -> None:
    for blink_profile, blink_samples in (
        ("scenario", 57),
        ("steady", 0),
        ("stress", 708),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            executable = root / "v1replay"
            out_dir = root / "replay"
            write_dummy_emulator(
                executable,
                emit_complete=True,
                blink_profile=blink_profile,
                blink_samples=blink_samples,
            )
            emulator = V1Emulator(executable, out_dir, "replay", blink_profile)
            emulator.start()
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline and not emulator._bench_completed():
                time.sleep(0.02)
            result = emulator.finish(window_completed=True)
            log = (out_dir / "v1replay.log").read_text(encoding="utf-8")
            expected_argv = f"argv=bench --machine-events --blink-profile {blink_profile}"
            assert_true(expected_argv in log, f"unexpected replay argv: {log!r}")
            assert_true(
                result["blink_profile"] == blink_profile,
                f"blink profile provenance was not recorded: {result}",
            )
            assert_true(result["blink_samples"] == blink_samples, f"wrong blink exposure: {result}")
            assert_true(
                result["blink_nominal_seconds"] == blink_samples / 3,
                f"wrong nominal blink duration: {result}",
            )


def test_razer_kiyo_default_uses_supported_full_hd_rate() -> None:
    previous = os.environ.pop("BENCH_CAMERA_FRAMERATE", None)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            camera = CameraCapture(Path(tmp), 300)
            assert_true(camera.framerate == 30, f"unsupported default camera rate: {camera.framerate}")
    finally:
        if previous is not None:
            os.environ["BENCH_CAMERA_FRAMERATE"] = previous


def test_post_upload_settle_is_interruptible_and_skippable() -> None:
    intervals: list[float] = []
    wait_for_post_upload_settle(3, sleep=intervals.append)
    assert_true(intervals == [1.0, 1.0, 1.0], f"settle interval was not split into short waits: {intervals}")

    intervals.clear()
    wait_for_post_upload_settle(0, sleep=intervals.append)
    assert_true(not intervals, f"zero-second settle should be skipped: {intervals}")


def test_encounter_csv_path_uses_perf_boot_identity() -> None:
    assert_true(
        encounter_csv_sd_path("/perf/perf_boot_61-cbab7c22.csv")
        == "/encounters/encounters_61-cbab7c22.csv",
        "tokenized encounter path did not follow the perf boot identity",
    )
    assert_true(
        encounter_csv_sd_path("/perf/perf_boot_61.csv") == "/encounters/encounters_61.csv",
        "legacy encounter path did not follow the perf boot identity",
    )
    for invalid in ("", "/perf/other.csv", "/perf/perf_boot_.csv", "/perf/perf_boot_1/extra.csv"):
        assert_true(encounter_csv_sd_path(invalid) == "", f"invalid perf path was accepted: {invalid}")


def main() -> int:
    test_idle_emulator_covers_and_stops_with_window()
    test_failed_window_still_stops_emulator()
    test_replay_requires_machine_completion_before_managed_stop()
    test_replay_blink_profile_argv_and_result()
    test_razer_kiyo_default_uses_supported_full_hd_rate()
    test_post_upload_settle_is_interruptible_and_skippable()
    test_encounter_csv_path_uses_perf_boot_identity()
    print("bench window tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
