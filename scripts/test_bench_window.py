#!/usr/bin/env python3
"""Focused process-lifecycle tests for the unified bench window collector."""

from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import camera_capture as camera_capture_module  # noqa: E402
import camera_grade as camera_grade_module  # noqa: E402
from camera_capture import (  # noqa: E402
    CALIBRATION_PATCH,
    FRAME_BYTES,
    FRAME_HEIGHT,
    FRAME_WIDTH,
    CameraCapture,
    evaluate_camera_profile_frames,
)
from camera_grade import (  # noqa: E402
    CAMERA_REFERENCE,
    DISPLAY_CROP_HEIGHT,
    DISPLAY_CROP_WIDTH,
    DISPLAY_CROP_X,
    DISPLAY_CROP_Y,
    EncounterObservation,
    FrameObservation,
    MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
    REFERENCE_ANCHOR_X,
    REFERENCE_ANCHOR_Y,
    REGISTRATION_HEIGHT,
    REGISTRATION_WIDTH,
    detect_display_crop_registration,
    find_replay_alignment,
    find_replay_offset,
    grade_idle,
    grade_replay,
    validate_camera_reference,
)
from run_window import (  # noqa: E402
    V1Emulator,
    camera_grade_required,
    encounter_csv_sd_path,
    wait_for_post_upload_settle,
)


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
        configured += "\necho 'status V1REPLAY_EVENT {\"state\":\"replay_started\",\"hostMonotonicSeconds\":12345.5}'"
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
        assert_true(
            result["replay_started_monotonic_seconds"] == 12345.5,
            f"first replay sample time was not recorded: {result}",
        )

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


def test_camera_profile_is_reapplied_after_recorder_open() -> None:
    events: list[str] = []

    class FakeProcess:
        returncode = None

        def poll(self) -> None:
            events.append("poll")
            return None

    class OrderingCameraCapture(CameraCapture):
        def _require_tools(self) -> None:
            pass

        def _configure(self, exposure: int) -> None:
            events.append(f"configure:{exposure}")

        def _snapshot(self, exposure: int, path: Path) -> None:
            events.append(f"snapshot:{exposure}")
            path.write_bytes(b"snapshot")

    original_popen = camera_capture_module.subprocess.Popen
    original_sleep = camera_capture_module.time.sleep
    try:
        camera_capture_module.subprocess.Popen = lambda *args, **kwargs: (  # type: ignore[assignment]
            events.append("recorder_open") or FakeProcess()
        )
        camera_capture_module.time.sleep = lambda seconds: events.append(  # type: ignore[assignment]
            f"sleep:{seconds}"
        )
        with tempfile.TemporaryDirectory() as tmp:
            camera = OrderingCameraCapture(Path(tmp), 300)
            camera.ffmpeg = "ffmpeg"
            assert_true(camera.start(), f"camera start failed: {camera.errors}")
            recorder_index = events.index("recorder_open")
            configure_indices = [index for index, event in enumerate(events) if event.startswith("configure:")]
            assert_true(len(configure_indices) == 2, f"camera profile was not applied twice: {events}")
            assert_true(
                configure_indices[-1] > recorder_index,
                f"camera profile was not reapplied after recorder ownership: {events}",
            )
            camera.process = None
            if camera.log_handle is not None:
                camera.log_handle.close()
                camera.log_handle = None
    finally:
        camera_capture_module.subprocess.Popen = original_popen
        camera_capture_module.time.sleep = original_sleep


def test_camera_profile_validation_rejects_recorder_brightness_drift() -> None:
    preflight = bytes([3]) * FRAME_BYTES
    stable_video = bytes([4]) * FRAME_BYTES
    stable = evaluate_camera_profile_frames(preflight, stable_video)
    assert_true(stable["result"] == "PASS", f"stable camera profile failed: {stable}")

    drifted_video = bytearray(stable_video)
    x0, y0, x1, y1 = CALIBRATION_PATCH
    for y in range(y0, y1):
        for x in range(x0, x1):
            drifted_video[y * FRAME_WIDTH + x] = 60
    drifted = evaluate_camera_profile_frames(preflight, bytes(drifted_video))
    assert_true(drifted["result"] == "FAIL", f"bright recorder handoff passed: {drifted}")
    assert_true("recorder handoff" in drifted["message"], f"wrong profile failure: {drifted}")


def test_only_captured_replay_video_is_mechanically_graded() -> None:
    captured = {"result": "CAPTURED"}
    assert_true(not camera_grade_required("core", captured), "core camera became a verdict")
    assert_true(not camera_grade_required("display", captured), "display camera became a verdict")
    assert_true(camera_grade_required("replay", captured), "captured replay camera was not graded")
    assert_true(
        not camera_grade_required("replay", {"result": "CAPTURE_FAILED"}),
        "failed replay capture was sent to the visual grader",
    )


def camera_observation(
    time_s: float,
    *,
    alert: bool,
    frequency: int | None = None,
    direction: str = "UNKNOWN",
) -> FrameObservation:
    return FrameObservation(
        time_s=time_s,
        visible_pixels=100,
        frequency_pixels=100 if alert else 0,
        frequency_mhz=frequency,
        frequency_confidence=0.1 if frequency is not None else 0.0,
        frequency_signature=(),
        direction=direction,
        direction_confidence=1.0 if direction != "UNKNOWN" else 0.0,
    )


def matching_replay_fixture(
    offset: float,
) -> tuple[list[FrameObservation], list[EncounterObservation]]:
    observations: list[FrameObservation] = []
    for index in range(252 * 3):
        replay_time = index / 3
        active = (5 <= replay_time < 56) or (59 <= replay_time < 244)
        observations.append(
            camera_observation(
                offset + replay_time,
                alert=active,
                frequency=24150 if active else None,
                direction="FRONT" if active else "UNKNOWN",
            )
        )
    encounters = [
        EncounterObservation(
            time_s=float(second),
            encounter_id=1 if second <= 56 else 2,
            frequency_mhz=24150,
            direction="FRONT",
            event="SAMPLE",
        )
        for second in (*range(5, 57), *range(59, 245))
    ]
    return observations, encounters


def test_camera_reference_images_match_bound_signatures() -> None:
    validate_camera_reference()
    altered = json.loads(json.dumps(CAMERA_REFERENCE))
    altered["frequency_references"]["24150"]["signature"][0] += 1
    try:
        validate_camera_reference(altered)
    except RuntimeError as exc:
        assert_true("signature does not match its image" in str(exc), f"wrong signature error: {exc}")
    else:
        raise AssertionError("camera reference signature drift passed validation")


def registration_fixture(offset_x: float, offset_y: float) -> bytes:
    glyphs = (
        ("11111", "10000", "10000", "11111", "00001", "00001", "11111"),
        ("11111", "10000", "10000", "10000", "10000", "10000", "11111"),
        ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
        ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    )
    cell_width = 5
    cell_height = 7
    gap_cells = 2
    landmark_width = (4 * 5 + 3 * gap_cells) * cell_width
    landmark_height = 7 * cell_height
    frame = bytearray(REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3)
    anchor_x = REGISTRATION_WIDTH * (
        DISPLAY_CROP_X + DISPLAY_CROP_WIDTH * (REFERENCE_ANCHOR_X + offset_x) / FRAME_WIDTH
    )
    anchor_y = REGISTRATION_HEIGHT * (
        DISPLAY_CROP_Y + DISPLAY_CROP_HEIGHT * (REFERENCE_ANCHOR_Y + offset_y) / FRAME_HEIGHT
    )
    x0 = round(anchor_x - (landmark_width - 1) / 2)
    y0 = round(anchor_y - (landmark_height - 1) / 2)
    cursor_cells = 0
    for glyph in glyphs:
        for cell_y, row in enumerate(glyph):
            for cell_x, active in enumerate(row):
                if active != "1":
                    continue
                pixel_x0 = x0 + (cursor_cells + cell_x) * cell_width
                pixel_y0 = y0 + cell_y * cell_height
                for y in range(pixel_y0, pixel_y0 + cell_height):
                    for x in range(pixel_x0, pixel_x0 + cell_width):
                        index = (y * REGISTRATION_WIDTH + x) * 3
                        frame[index : index + 3] = bytes((255, 100, 10))
        cursor_cells += 5 + gap_cells
    return bytes(frame)


def test_camera_crop_registration_tracks_bounded_rig_movement() -> None:
    expected_x = 64.0
    expected_y = 8.0
    actual_x, actual_y, registration = detect_display_crop_registration(
        registration_fixture(expected_x, expected_y)
    )
    assert_true(abs(actual_x - expected_x) < 1.0, f"camera x registration drifted: {actual_x}")
    assert_true(abs(actual_y - expected_y) < 1.0, f"camera y registration drifted: {actual_y}")
    assert_true(registration["result"] == "PASS", "bounded camera registration did not pass")

    try:
        detect_display_crop_registration(registration_fixture(110.0, 0.0))
    except RuntimeError as exc:
        assert_true(
            "exceeds bounded crop translation" in str(exc),
            f"unexpected registration error: {exc}",
        )
    else:
        raise AssertionError("unbounded camera movement passed registration")


def test_camera_crop_registration_falls_back_to_bright_still() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        camera_dir = Path(tmp)
        session_start = camera_dir / "session_start_exp156.jpg"
        bright = camera_dir / "final_exp5.jpg"
        session_start.write_bytes(b"session")
        bright.write_bytes(b"bright")
        original = camera_grade_module.calibrate_display_crop
        calls: list[str] = []

        def fake_calibrate(path: Path) -> tuple[float, float, dict[str, object]]:
            calls.append(path.name)
            if path == session_start:
                raise RuntimeError("display transition")
            return 4.0, 2.0, {"result": "PASS", "source_still": path.name}

        camera_grade_module.calibrate_display_crop = fake_calibrate
        try:
            offset_x, offset_y, registration = (
                camera_grade_module.calibrate_display_crop_from_evidence(
                    camera_dir,
                    {
                        "session_start_still": session_start.name,
                        "bright_still": bright.name,
                    },
                )
            )
        finally:
            camera_grade_module.calibrate_display_crop = original

        assert_true(calls == [session_start.name, bright.name], f"wrong fallback order: {calls}")
        assert_true((offset_x, offset_y) == (4.0, 2.0), "bright-still registration was lost")
        assert_true(registration["source_field"] == "bright_still", "fallback source was not recorded")


def test_camera_grade_rejects_visual_state_that_disagrees_with_log() -> None:
    offset = 2.0
    observations, encounters = matching_replay_fixture(offset)
    passed = grade_replay(observations, encounters, offset)
    assert_true(passed["result"] == "PASS", f"matching camera/log evidence failed: {passed}")
    adjustment = passed["alignment"]["hint_adjustment_seconds"]
    assert_true(
        abs(adjustment) < MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
        f"camera/log alignment escaped the timing hint: {passed}",
    )

    wrong = [
        FrameObservation(
            **{
                **item.__dict__,
                "frequency_mhz": 35500 if item.alert_visible else None,
                "direction": "REAR" if item.alert_visible else "UNKNOWN",
            }
        )
        for item in observations
    ]
    failed = grade_replay(wrong, encounters, offset)
    assert_true(failed["result"] == "FAIL", f"camera/log disagreement passed: {failed}")
    assert_true(
        failed["alignment"] == passed["alignment"],
        "frequency/direction answers changed alert/rest alignment",
    )

    residual_observations, residual_encounters = matching_replay_fixture(1.0)
    residual = grade_replay(residual_observations, residual_encounters, 2.0)
    assert_true(residual["result"] == "PASS", f"bounded -1s residual failed: {residual}")
    assert_true(
        residual["alignment"]["hint_adjustment_seconds"] == -1.0,
        f"host/video residual was not recorded: {residual}",
    )


def test_replay_alignment_requires_hint_and_rejects_boundary() -> None:
    observations, encounters = matching_replay_fixture(2.0)
    for invalid_hint in (None, float("nan"), float("inf")):
        alignment = find_replay_alignment(observations, encounters, invalid_hint)
        assert_true(alignment["result"] == "INCONCLUSIVE", f"invalid hint passed: {alignment}")
        assert_true(
            alignment["diagnostic"]["code"] == "timing_anchor_missing",
            f"wrong missing-hint diagnostic: {alignment}",
        )

    # Process launch precedes BLE transport readiness. The measured fresh-boot
    # delay remains valid while a larger clock error still reaches the bound.
    delayed_observations, delayed_encounters = matching_replay_fixture(13.0 / 3.0)
    delayed_offset, _ratio = find_replay_offset(delayed_observations, delayed_encounters, 2.0)
    delayed_adjustment = abs(delayed_offset - 2.0)
    assert_true(
        2.0 <= delayed_adjustment < MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
        f"measured BLE readiness delay escaped the timing guard: {delayed_offset}",
    )

    # A four-second clock error pushes the best available candidate to the
    # positive edge of the deliberately bounded three-second search window.
    boundary_observations, boundary_encounters = matching_replay_fixture(6.0)
    boundary = find_replay_alignment(boundary_observations, boundary_encounters, 2.0)
    assert_true(boundary["result"] == "INCONCLUSIVE", f"boundary alignment passed: {boundary}")
    assert_true(
        boundary["diagnostic"]["code"] == "alignment_search_boundary",
        f"wrong boundary diagnostic: {boundary}",
    )

    flat = [camera_observation(index / 3, alert=False) for index in range(920)]
    ambiguous = find_replay_alignment(flat, encounters, 2.0)
    assert_true(ambiguous["result"] == "INCONCLUSIVE", f"flat alignment passed: {ambiguous}")
    assert_true(
        ambiguous["diagnostic"]["code"] == "alignment_ambiguous",
        f"flat alignment had wrong diagnostic: {ambiguous}",
    )


def test_replay_camera_abstains_for_unreadable_or_ambiguous_answers() -> None:
    observations, encounters = matching_replay_fixture(2.0)
    unreadable = [
        FrameObservation(
            **{
                **item.__dict__,
                "frequency_mhz": None,
                "frequency_confidence": 0.0,
                "direction": "UNKNOWN",
                "direction_confidence": 0.0,
            }
        )
        for item in observations
    ]
    unreadable_grade = grade_replay(unreadable, encounters, 2.0)
    assert_true(
        unreadable_grade["result"] == "INCONCLUSIVE",
        f"unreadable answers became a product failure: {unreadable_grade}",
    )
    unreadable_codes = {item["code"] for item in unreadable_grade["diagnostics"]}
    assert_true(
        {"frequency_observations_insufficient", "direction_observations_insufficient"}
        <= unreadable_codes,
        f"missing unreadable diagnostics: {unreadable_grade}",
    )

    frequencies = (24150, 34700, 35500)
    directions = ("FRONT", "SIDE", "REAR")
    contradictory = [
        FrameObservation(
            **{
                **item.__dict__,
                "frequency_mhz": frequencies[index % len(frequencies)] if item.alert_visible else None,
                "direction": directions[index % len(directions)] if item.alert_visible else "UNKNOWN",
            }
        )
        for index, item in enumerate(observations)
    ]
    contradictory_grade = grade_replay(contradictory, encounters, 2.0)
    assert_true(
        contradictory_grade["result"] == "INCONCLUSIVE",
        f"contradictory nearby frames searched for a favorable answer: {contradictory_grade}",
    )
    assert_true(
        any(item["code"] == "encounter_classification_ambiguous" for item in contradictory_grade["diagnostics"]),
        f"contradictory consensus diagnostic missing: {contradictory_grade}",
    )


def test_replay_semantic_consensus_counts_unreadable_samples() -> None:
    observations, encounters = matching_replay_fixture(2.0)
    high_coverage = grade_replay(observations, encounters, 2.0)
    assert_true(high_coverage["result"] == "PASS", f"high-coverage match failed: {high_coverage}")

    alert_index = 0
    sparse: list[FrameObservation] = []
    for item in observations:
        readable = item.alert_visible and alert_index % 5 == 0
        if item.alert_visible:
            alert_index += 1
        sparse.append(
            FrameObservation(
                **{
                    **item.__dict__,
                    "frequency_mhz": item.frequency_mhz if readable else None,
                    "frequency_confidence": item.frequency_confidence if readable else 0.0,
                    "direction": item.direction if readable else "UNKNOWN",
                    "direction_confidence": item.direction_confidence if readable else 0.0,
                }
            )
        )
    sparse_grade = grade_replay(sparse, encounters, 2.0)
    assert_true(
        sparse_grade["result"] == "INCONCLUSIVE",
        f"sparse favorable semantic readings qualified: {sparse_grade}",
    )
    assert_true(
        sparse_grade["alignment"] == high_coverage["alignment"],
        "semantic readability changed alert/rest alignment",
    )
    assert_true(
        any(
            item["code"] == "encounter_classification_ambiguous"
            for item in sparse_grade["diagnostics"]
        ),
        f"sparse semantic diagnostic missing: {sparse_grade}",
    )

    confidently_wrong = [
        FrameObservation(
            **{
                **item.__dict__,
                "frequency_mhz": 35500 if item.alert_visible else None,
                "direction": "REAR" if item.alert_visible else "UNKNOWN",
            }
        )
        for item in observations
    ]
    wrong_grade = grade_replay(confidently_wrong, encounters, 2.0)
    assert_true(wrong_grade["result"] == "FAIL", f"confident semantic mismatch abstained: {wrong_grade}")


def test_idle_camera_grade_rejects_unlogged_alerts() -> None:
    idle = [camera_observation(float(second), alert=False) for second in range(300)]
    assert_true(grade_idle(idle, 0.0, 300.0)["result"] == "PASS", "visible idle display failed")
    unexpected = [camera_observation(float(second), alert=second > 20) for second in range(300)]
    result = grade_idle(unexpected, 0.0, 300.0)
    assert_true(result["result"] == "FAIL", f"unlogged camera alerts passed: {result}")


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
    test_camera_profile_is_reapplied_after_recorder_open()
    test_camera_profile_validation_rejects_recorder_brightness_drift()
    test_only_captured_replay_video_is_mechanically_graded()
    test_camera_reference_images_match_bound_signatures()
    test_camera_crop_registration_tracks_bounded_rig_movement()
    test_camera_crop_registration_falls_back_to_bright_still()
    test_camera_grade_rejects_visual_state_that_disagrees_with_log()
    test_replay_alignment_requires_hint_and_rejects_boundary()
    test_replay_camera_abstains_for_unreadable_or_ambiguous_answers()
    test_replay_semantic_consensus_counts_unreadable_samples()
    test_idle_camera_grade_rejects_unlogged_alerts()
    test_post_upload_settle_is_interruptible_and_skippable()
    test_encounter_csv_path_uses_perf_boot_identity()
    print("bench window tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
