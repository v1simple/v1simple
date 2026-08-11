#!/usr/bin/env python3
"""Focused regressions for fixed-profile camera admission and smoke."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import camera_preflight as preflight_module  # noqa: E402
import run_window as run_window_module  # noqa: E402
from bench_identity import current_grader_fingerprint  # noqa: E402
from bench_policy import _validate_camera_smoke  # noqa: E402
from camera_artifacts import build_capture_manifest  # noqa: E402
from camera_grade import (  # noqa: E402
    DISPLAY_CROP_HEIGHT,
    DISPLAY_CROP_WIDTH,
    DISPLAY_CROP_X,
    DISPLAY_CROP_Y,
    REFERENCE_ANCHOR_X,
    REFERENCE_ANCHOR_Y,
    REGISTRATION_HEIGHT,
    REGISTRATION_WIDTH,
    CameraRegistrationError,
    detect_display_crop_registration,
)
from camera_preflight import run_camera_preflight, run_camera_smoke  # noqa: E402
from run_window import CameraPreflightFailure, collect_live  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


SCAN_GLYPHS = (
    ("11111", "10000", "10000", "11111", "00001", "00001", "11111"),
    ("11111", "10000", "10000", "10000", "10000", "10000", "11111"),
    ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
)
GLYPH_CELL_WIDTH = 5
GLYPH_CELL_HEIGHT = 7
GLYPH_GAP_CELLS = 2
LANDMARK_WIDTH = (4 * 5 + 3 * GLYPH_GAP_CELLS) * GLYPH_CELL_WIDTH
LANDMARK_HEIGHT = 7 * GLYPH_CELL_HEIGHT


def landmark_origin(offset_x: float, offset_y: float, width: int, height: int) -> tuple[int, int]:
    anchor_x = REGISTRATION_WIDTH * (
        DISPLAY_CROP_X + DISPLAY_CROP_WIDTH * (REFERENCE_ANCHOR_X + offset_x) / 480
    )
    anchor_y = REGISTRATION_HEIGHT * (
        DISPLAY_CROP_Y + DISPLAY_CROP_HEIGHT * (REFERENCE_ANCHOR_Y + offset_y) / 200
    )
    return round(anchor_x - (width - 1) / 2), round(anchor_y - (height - 1) / 2)


def registration_fixture(
    offset_x: float,
    offset_y: float,
    *,
    cell_width: int = GLYPH_CELL_WIDTH,
    cell_height: int = GLYPH_CELL_HEIGHT,
) -> bytes:
    frame = bytearray(REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3)
    landmark_width = (4 * 5 + 3 * GLYPH_GAP_CELLS) * cell_width
    landmark_height = 7 * cell_height
    x0, y0 = landmark_origin(offset_x, offset_y, landmark_width, landmark_height)
    cursor_cells = 0
    for glyph in SCAN_GLYPHS:
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
        cursor_cells += 5 + GLYPH_GAP_CELLS
    return bytes(frame)


def rectangle_fixture(
    offset_x: float,
    offset_y: float,
    *,
    width: int = LANDMARK_WIDTH,
    height: int = LANDMARK_HEIGHT,
    outline: bool = False,
) -> bytes:
    frame = bytearray(REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3)
    x0, y0 = landmark_origin(offset_x, offset_y, width, height)
    for y in range(y0, y0 + height):
        for x in range(x0, x0 + width):
            if outline and x not in {x0, x0 + width - 1} and y not in {y0, y0 + height - 1}:
                continue
            index = (y * REGISTRATION_WIDTH + x) * 3
            frame[index : index + 3] = bytes((255, 100, 10))
    return bytes(frame)


def obscured_fixture(offset_x: float, offset_y: float) -> bytes:
    frame = bytearray(registration_fixture(offset_x, offset_y))
    x0, y0 = landmark_origin(offset_x, offset_y, LANDMARK_WIDTH, LANDMARK_HEIGHT)
    band_y0 = y0 + LANDMARK_HEIGHT // 2 - 5
    for y in range(band_y0, band_y0 + 10):
        for x in range(x0, x0 + LANDMARK_WIDTH):
            index = (y * REGISTRATION_WIDTH + x) * 3
            frame[index : index + 3] = bytes((255, 100, 10))
    return bytes(frame)


def center_black_occluded_fixture(offset_x: float, offset_y: float) -> bytes:
    """Match the verifier reproduction: erase a centered 30px band of the 49px glyph."""
    frame = bytearray(registration_fixture(offset_x, offset_y))
    x0, y0 = landmark_origin(offset_x, offset_y, LANDMARK_WIDTH, LANDMARK_HEIGHT)
    occlusion_height = 30
    occlusion_y0 = y0 + (LANDMARK_HEIGHT - occlusion_height) // 2
    for y in range(occlusion_y0, occlusion_y0 + occlusion_height):
        for x in range(x0, x0 + LANDMARK_WIDTH):
            index = (y * REGISTRATION_WIDTH + x) * 3
            frame[index : index + 3] = bytes((0, 0, 0))
    return bytes(frame)


class FakeCamera:
    def __init__(
        self,
        out_dir: Path,
        expected_duration_s: int,
        *,
        start_ok: bool = True,
        smoke_capture_ok: bool = False,
        profile_updates: dict[str, Any] | None = None,
    ) -> None:
        self.out_dir = out_dir
        self.expected_duration_s = expected_duration_s
        self.camera_name = "Razer Kiyo"
        self.camera_device_index = 0
        self.ffmpeg = "ffmpeg"
        self.preflight_path = out_dir / "session_start_exp156.jpg"
        self.preflight_result_path = out_dir / "camera_preflight.json"
        self.result_path = out_dir / "camera_result.json"
        self.video_path = out_dir / "evidence_exp156.mp4"
        self.recording_started_monotonic = None
        self.errors: list[str] = []
        self.start_ok = start_ok
        self.smoke_capture_ok = smoke_capture_ok
        self.profile_updates = dict(profile_updates or {})
        self.running = False
        self.start_calls = 0
        self.abort_calls = 0
        self.stop_calls = 0

    def profile(self) -> dict[str, Any]:
        return {
            "auto_exposure_priority": 0,
            "focus_abs": 208,
            "video_exposure_time_abs": 156,
            "bright_exposure_time_abs": 5,
            "dim_exposure_time_abs": 1250,
            "framerate": 30,
            "input_pixel_format": "nv12",
            "video_size": "1280x720",
            **self.profile_updates,
        }

    def start(self) -> bool:
        self.start_calls += 1
        self.out_dir.mkdir(parents=True, exist_ok=True)
        if not self.start_ok:
            self.errors.append("camera unavailable")
            self._write_result("CAPTURE_FAILED")
            return False
        self.preflight_path.write_bytes(b"fixed session still")
        self.video_path.write_bytes(b"short video")
        self.running = True
        self._write_result("RECORDING")
        return True

    def _write_result(self, result: str) -> None:
        self.result_path.write_text(
            json.dumps(
                {
                    "schema_version": 2,
                    "kind": "bench_camera_evidence",
                    "result": result,
                    "camera_name": self.camera_name,
                    "camera_device_index": self.camera_device_index,
                    "profile": self.profile(),
                    "expected_duration_seconds": self.expected_duration_s,
                    "video": self.video_path.name if self.video_path.is_file() else "",
                    "video_duration_seconds": 3.5 if result == "CAPTURED" else 0.0,
                    "video_probe": {
                        "duration_seconds": 3.5,
                        "width": 1280,
                        "height": 720,
                        "average_frame_rate": 30.0,
                    }
                    if result == "CAPTURED"
                    else {},
                    "session_start_still": self.preflight_path.name if self.preflight_path.is_file() else "",
                    "bright_still": "",
                    "dim_still": "",
                    "profile_validation": {"result": "PASS"} if result == "CAPTURED" else {},
                    "errors": list(self.errors),
                }
            )
            + "\n",
            encoding="utf-8",
        )

    def abort(self, diagnostic_code: str) -> dict[str, Any]:
        self.abort_calls += 1
        self.running = False
        self.errors.append(diagnostic_code)
        self._write_result("CAPTURE_FAILED")
        return {"result": "CAPTURE_FAILED", "errors": list(self.errors)}

    def stop(self, collection_completed: bool) -> dict[str, Any]:
        self.stop_calls += 1
        self.running = False
        result = "CAPTURED" if self.smoke_capture_ok and collection_completed is False else "CAPTURE_FAILED"
        self._write_result(result)
        return {"result": result, "errors": list(self.errors)}


def with_calibrator(fake: Any, callback: Any) -> Any:
    original = preflight_module.calibrate_display_crop
    preflight_module.calibrate_display_crop = fake
    try:
        return callback()
    finally:
        preflight_module.calibrate_display_crop = original


def test_dynamic_fixture_records_crop_exposure_and_hash() -> None:
    expected_x, expected_y = 24.0, -6.0

    def calibrate(_path: Path, _ffmpeg: str) -> tuple[float, float, dict[str, Any]]:
        return detect_display_crop_registration(registration_fixture(expected_x, expected_y))

    with tempfile.TemporaryDirectory() as tmp:
        camera = FakeCamera(Path(tmp), 300)
        result = with_calibrator(calibrate, lambda: run_camera_preflight(camera))
        assert_true(result["result"] == "PASS", f"bounded preflight failed: {result}")
        transform = result["registration"]["transform"]
        assert_true(transform["kind"] == "dynamic_similarity", f"wrong transform: {transform}")
        assert_true(
            all(0.0 <= value <= 1.0 for value in transform["crop_fractions"]),
            f"dynamic crop escaped the frame: {transform}",
        )
        assert_true(result["camera"]["exposure_time_abs"] == 156, f"wrong exposure: {result}")
        expected_hash = hashlib.sha256(camera.preflight_path.read_bytes()).hexdigest()
        assert_true(result["source_still"]["sha256"] == expected_hash, "source still hash was lost")
        assert_true(camera.start_calls == 1 and camera.running, "passing preflight did not continue once")


def test_reseated_dut_position_and_scale_are_applied_before_capture() -> None:
    expected_x, expected_y = 67.0, 40.0

    _actual_x, _actual_y, registration = detect_display_crop_registration(
        registration_fixture(expected_x, expected_y, cell_width=6, cell_height=8)
    )

    assert_true(registration["result"] == "PASS", f"reseated DUT registration failed: {registration}")
    assert_true(
        registration["transform"]["scale"] > 1.05,
        f"closer DUT scale was not measured: {registration}",
    )
    assert_true(
        registration["transform"]["kind"] == "dynamic_similarity",
        f"dynamic transform was not recorded: {registration}",
    )


def expect_registration_code(frame: bytes, code: str) -> None:
    try:
        detect_display_crop_registration(frame)
    except CameraRegistrationError as exc:
        assert_true(exc.diagnostic["code"] == code, f"wrong diagnostic: {exc.diagnostic}")
        assert_true(
            "measured" in exc.diagnostic and "thresholds" in exc.diagnostic,
            "diagnostic lacks bounds",
        )
    else:
        raise AssertionError(f"registration unexpectedly passed instead of {code}")


def test_registration_refusal_codes_are_precise() -> None:
    expect_registration_code(
        registration_fixture(350.0, 0.0),
        "screen_crop_outside_frame",
    )
    expect_registration_code(
        registration_fixture(0.0, 220.0),
        "screen_crop_outside_frame",
    )
    expect_registration_code(
        bytes(REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3),
        "screen_landmark_not_found",
    )
    expect_registration_code(
        rectangle_fixture(0.0, 0.0, width=20, height=10),
        "screen_landmark_geometry_invalid",
    )
    for unreadable in (
        rectangle_fixture(0.0, 0.0),
        rectangle_fixture(0.0, 0.0, outline=True),
        obscured_fixture(0.0, 0.0),
    ):
        expect_registration_code(unreadable, "screen_landmark_unreadable")

    try:
        detect_display_crop_registration(center_black_occluded_fixture(0.0, 0.0))
    except CameraRegistrationError as exc:
        assert_true(exc.diagnostic["code"] == "screen_landmark_unreadable", f"{exc.diagnostic}")
        assert_true(
            exc.diagnostic["measured"]["maximum_blank_row_run_pixels"] == 30,
            f"centered occlusion topology was not measured exactly: {exc.diagnostic}",
        )
        assert_true(
            exc.diagnostic["thresholds"]["maximum_blank_row_run_pixels"] == 2,
            f"centered occlusion threshold was not recorded: {exc.diagnostic}",
        )
    else:
        raise AssertionError("centered 30px black occlusion passed SCAN readability")


def test_decode_and_start_failures_stop_cleanly() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        decode_camera = FakeCamera(Path(tmp) / "decode", 300)
        decoded = with_calibrator(
            lambda *_args: (_ for _ in ()).throw(RuntimeError("invalid JPEG")),
            lambda: run_camera_preflight(decode_camera),
        )
        assert_true(decoded["diagnostics"][0]["code"] == "preflight_decode_failed", f"{decoded}")
        assert_true(
            decode_camera.abort_calls == 1 and not decode_camera.running,
            "decode refusal leaked recorder",
        )

        start_camera = FakeCamera(Path(tmp) / "start", 300, start_ok=False)
        started = run_camera_preflight(start_camera)
        assert_true(started["diagnostics"][0]["code"] == "capture_start_failed", f"{started}")
        assert_true(not start_camera.running, "failed camera start remained active")


def test_profile_mismatch_refuses_before_camera_start() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        camera = FakeCamera(
            Path(tmp) / "exposure",
            300,
            profile_updates={"video_exposure_time_abs": 5},
        )
        result = run_camera_preflight(camera)
        diagnostic = result["diagnostics"][0]
        assert_true(result["result"] == "INCONCLUSIVE", f"profile mismatch passed: {result}")
        assert_true(diagnostic["code"] == "camera_profile_mismatch", f"wrong profile diagnostic: {result}")
        mismatch = diagnostic["measured"]["mismatched_fields"]["video_exposure_time_abs"]
        assert_true(mismatch == {"measured": 5, "expected": 156}, f"wrong exposure evidence: {result}")
        assert_true(camera.start_calls == 0 and not camera.running, "profile mismatch opened camera")

        wrong_camera = FakeCamera(Path(tmp) / "name", 300)
        wrong_camera.camera_name = "Uncalibrated Camera"
        wrong_result = run_camera_preflight(wrong_camera)
        wrong_diagnostic = wrong_result["diagnostics"][0]
        assert_true(
            wrong_diagnostic["code"] == "camera_profile_mismatch",
            f"wrong camera name passed: {wrong_result}",
        )
        assert_true(
            wrong_diagnostic["measured"]["camera_name"] == "Uncalibrated Camera"
            and wrong_diagnostic["thresholds"]["expected_camera_name"] == "Razer Kiyo",
            f"camera name diagnostic was imprecise: {wrong_result}",
        )
        assert_true(wrong_camera.start_calls == 0, "uncalibrated camera name opened camera")


def make_live_args() -> SimpleNamespace:
    return SimpleNamespace(
        port="fixture-port",
        upload=False,
        skip_web=False,
        post_upload_settle_seconds=0,
        replay_executable="fixture-replay",
        suite="core",
        blink_profile="steady",
        camera=True,
        duration_seconds=1,
        baud=115200,
        ready_timeout_seconds=1,
        completion_grace_seconds=1,
        export_idle_timeout_seconds=1,
        export_retries=0,
        export_recovery_idle_timeout_seconds=1,
    )


def test_collect_refusal_never_opens_product_path_and_pass_continues_once() -> None:
    events = {"serial": 0, "qstart": 0, "emulator_start": 0}
    cameras: list[FakeCamera] = []
    pending_profile_updates: list[dict[str, Any]] = []

    class FakeEmulator:
        def __init__(self, *_args: Any) -> None:
            self.started = False

        def start(self) -> None:
            events["emulator_start"] += 1
            self.started = True

        def health_problem(self) -> str:
            return ""

        def finish(self, completed: bool) -> dict[str, Any]:
            return {"completed": bool(completed and self.started), "mode": "idle"}

    class FakeSerial:
        def __init__(self, *_args: Any) -> None:
            events["serial"] += 1

        def close(self) -> None:
            pass

    originals = {
        "CameraCapture": run_window_module.CameraCapture,
        "V1Emulator": run_window_module.V1Emulator,
        "BenchSerial": run_window_module.BenchSerial,
        "wait_for_port": run_window_module.wait_for_port,
        "wait_ready": run_window_module.wait_ready,
        "start_and_wait": run_window_module.start_and_wait,
        "download_csv": run_window_module.download_csv,
    }
    run_window_module.CameraCapture = lambda out, duration: (  # type: ignore[assignment]
        cameras.append(
            FakeCamera(
                out,
                duration,
                profile_updates=pending_profile_updates.pop(0) if pending_profile_updates else None,
            )
        )
        or cameras[-1]
    )
    run_window_module.V1Emulator = FakeEmulator  # type: ignore[assignment]
    run_window_module.BenchSerial = FakeSerial  # type: ignore[assignment]
    run_window_module.wait_for_port = lambda *_args: "fixture-port"  # type: ignore[assignment]
    run_window_module.wait_ready = lambda *_args: {}  # type: ignore[assignment]

    def fake_start_and_wait(*_args: Any, after_started: Any, **_kwargs: Any) -> dict[str, Any]:
        events["qstart"] += 1
        after_started()
        return {"csvPath": "/perf/perf_boot_1.csv"}

    run_window_module.start_and_wait = fake_start_and_wait  # type: ignore[assignment]

    def fake_download(_q: Any, out_dir: Path, *_args: Any) -> Path:
        path = out_dir / "perf.csv"
        path.write_text("millis\n0\n", encoding="utf-8")
        return path

    run_window_module.download_csv = fake_download  # type: ignore[assignment]
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pending_profile_updates.append({"video_exposure_time_abs": 5})
            try:
                collect_live(make_live_args(), root / "profile-mismatch")
            except CameraPreflightFailure as exc:
                assert_true(
                    exc.preflight["diagnostics"][0]["code"] == "camera_profile_mismatch",
                    f"wrong collection profile diagnostic: {exc.preflight}",
                )
            else:
                raise AssertionError("non-reference profile entered collection")
            profile_camera = cameras[-1]
            assert_true(profile_camera.start_calls == 0 and not profile_camera.running, "camera opened")
            assert_true(
                events == {"serial": 0, "qstart": 0, "emulator_start": 0},
                f"profile mismatch entered product path: {events}",
            )

            refused = lambda *_args: detect_display_crop_registration(  # noqa: E731
                rectangle_fixture(0.0, 0.0)
            )
            try:
                with_calibrator(refused, lambda: collect_live(make_live_args(), root / "refused"))
            except CameraPreflightFailure as exc:
                assert_true(
                    exc.preflight["diagnostics"][0]["code"] == "screen_landmark_unreadable",
                    f"wrong unreadable collection diagnostic: {exc.preflight}",
                )
            else:
                raise AssertionError("failed preflight entered collection")
            refused_camera = cameras[-1]
            assert_true(
                not refused_camera.running and refused_camera.abort_calls == 1,
                "camera was not stopped",
            )
            assert_true(
                events == {"serial": 0, "qstart": 0, "emulator_start": 0},
                f"product path ran: {events}",
            )

            def calibrate(_path: Path, _ffmpeg: str) -> tuple[float, float, dict[str, Any]]:
                return detect_display_crop_registration(registration_fixture(0.0, 0.0))

            with_calibrator(calibrate, lambda: collect_live(make_live_args(), root / "passed"))
            passed_camera = cameras[-1]
            assert_true(
                passed_camera.start_calls == 1 and passed_camera.stop_calls == 1,
                "pass did not continue once",
            )
            assert_true(
                events == {"serial": 1, "qstart": 1, "emulator_start": 1},
                f"wrong pass lifecycle: {events}",
            )
    finally:
        for name, value in originals.items():
            setattr(run_window_module, name, value)


def test_capture_identity_owns_preflight_and_smoke_has_no_product_dependencies() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        camera_dir = root / "replay" / "camera"
        camera_dir.mkdir(parents=True)
        names = {
            "video": "evidence_exp156.mp4",
            "session_start_still": "session_start_exp156.jpg",
            "bright_still": "final_exp5.jpg",
            "dim_still": "final_exp1250.jpg",
        }
        for name in names.values():
            (camera_dir / name).write_bytes(name.encode("ascii"))
        preflight_path = camera_dir / "camera_preflight.json"
        preflight_path.write_text(
            json.dumps(
                {
                    "schema_version": 2,
                    "kind": "bench_camera_preflight",
                    "result": "PASS",
                    "registration": {
                        "transform": {
                            "kind": "dynamic_similarity",
                            "scale": 1.1,
                            "crop_fractions": [0.2, 0.3, 0.57, 0.42],
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        encounter = camera_dir.parent / "encounters.csv"
        encounter.write_text("millis,event,priority\n0,SAMPLE,1\n", encoding="utf-8")
        camera_result = {
            "result": "CAPTURED",
            "camera_name": "Razer Kiyo",
            "camera_device_index": 0,
            "profile": {"video_exposure_time_abs": 156},
            "expected_duration_seconds": 300,
            "video_duration_seconds": 300.0,
            "profile_validation": {"result": "PASS"},
            "errors": [],
            **names,
        }
        first = build_capture_manifest(
            camera_dir=camera_dir,
            camera_result=camera_result,
            suite="replay",
            product_fingerprint="a" * 64,
            scenario_fingerprint="b" * 64,
            encounter_csv_path=encounter,
            timing_anchor={"kind": "first_emitted_replay_sample", "video_seconds": 2.0},
        )
        assert_true(
            first["preflight"]["sha256"]
            == hashlib.sha256(preflight_path.read_bytes()).hexdigest(),
            "manifest lost preflight hash",
        )
        assert_true(
            first["preflight"]["registration"]["transform"]["crop_fractions"]
            == [0.2, 0.3, 0.57, 0.42],
            "manifest lost transform",
        )
        payload = json.loads(preflight_path.read_text(encoding="utf-8"))
        payload["registration"]["transform"]["crop_fractions"][0] = 0.21
        preflight_path.write_text(json.dumps(payload), encoding="utf-8")
        second = build_capture_manifest(
            camera_dir=camera_dir,
            camera_result=camera_result,
            suite="replay",
            product_fingerprint="a" * 64,
            scenario_fingerprint="b" * 64,
            encounter_csv_path=encounter,
            timing_anchor={"kind": "first_emitted_replay_sample", "video_seconds": 2.0},
        )
        assert_true(first["capture_id"] != second["capture_id"], "preflight change did not change capture_id")

        smoke_dir = root / "smoke"
        made: list[FakeCamera] = []

        def factory(out_dir: Path, duration: int) -> FakeCamera:
            camera = FakeCamera(out_dir, duration, smoke_capture_ok=True)
            made.append(camera)
            return camera

        smoke, code = with_calibrator(
            lambda _path, _ffmpeg: detect_display_crop_registration(registration_fixture(0.0, 0.0)),
            lambda: run_camera_smoke(smoke_dir, camera_factory=factory, sleep=lambda _seconds: None),
        )
        assert_true(code == 0 and smoke["result"] == "PASS", f"standalone smoke failed: {smoke}")
        assert_true(smoke["schema_version"] == 2, f"smoke schema did not own its bytes: {smoke}")
        assert_true(
            smoke["grader_fingerprint"] == current_grader_fingerprint(),
            "standalone smoke lost current grader ownership",
        )
        assert_true(
            smoke["camera"]
            == {
                "name": "Razer Kiyo",
                "device_index": 0,
                "profile": made[0].profile(),
            },
            f"standalone smoke lost fixed camera metadata: {smoke}",
        )
        assert_true(
            set(smoke["artifacts"])
            == {"preflight", "session_start_still", "video", "camera_result"},
            f"standalone smoke has incomplete ownership: {smoke}",
        )
        for name, entry in smoke["artifacts"].items():
            path = smoke_dir / entry["path"]
            assert_true(Path(entry["path"]).name == entry["path"], f"unsafe {name} path: {entry}")
            assert_true(entry["size_bytes"] == path.stat().st_size > 0, f"wrong {name} size: {entry}")
            assert_true(
                entry["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest(),
                f"wrong {name} hash: {entry}",
            )
        assert_true(
            smoke["preflight"]["path"] == smoke["artifacts"]["preflight"]["path"]
            and smoke["preflight"]["sha256"] == smoke["artifacts"]["preflight"]["sha256"],
            f"preflight summary disagrees with owned bytes: {smoke}",
        )
        _validate_camera_smoke(
            smoke,
            smoke_dir / "camera_smoke.json",
            current_grader=current_grader_fingerprint(),
        )
        assert_true(made[0].start_calls == 1 and made[0].stop_calls == 1, "smoke lifecycle was duplicated")


def main() -> int:
    test_dynamic_fixture_records_crop_exposure_and_hash()
    test_reseated_dut_position_and_scale_are_applied_before_capture()
    test_registration_refusal_codes_are_precise()
    test_decode_and_start_failures_stop_cleanly()
    test_profile_mismatch_refuses_before_camera_start()
    test_collect_refusal_never_opens_product_path_and_pass_continues_once()
    test_capture_identity_owns_preflight_and_smoke_has_no_product_dependencies()
    print("camera preflight tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
