#!/usr/bin/env python3
"""Manage calibrated macOS camera evidence for one bench window."""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


# The open-aperture AR0234 profile converges to 5 ms in aperture-priority mode.
# Keep the value in the contract and verify the live readback before recording;
# setting 5 ms in manual mode disables the camera's internal gain and is dark.
VIDEO_EXPOSURE = int(os.environ.get("BENCH_CAMERA_VIDEO_EXPOSURE", "50"))

FRAME_WIDTH = 480
FRAME_HEIGHT = 200
FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT
DISPLAY_CROP = "crop=iw*0.52:ih*0.38:iw*0.18:ih*0.25"
CALIBRATION_VIDEO_TIME_S = 3.0
CALIBRATION_PATCH = (150, 20, 260, 45)
CAMERA_PROFILE_SETTLE_S = 5.0
CAMERA_SESSION_READY_TIMEOUT_S = 15.0
CAMERA_RECORDING_READY_TIMEOUT_S = 15.0
CAMERA_PREFLIGHT_RECORD_SECONDS = 0.75


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _find_uvc_util() -> Path | None:
    configured = os.environ.get("BENCH_UVC_UTIL", "").strip()
    candidates: list[Path] = []
    if configured:
        candidates.append(Path(configured).expanduser())
    discovered = shutil.which("uvc-util")
    if discovered:
        candidates.append(Path(discovered))
    home = Path.home()
    candidates.extend(
        [
            home / "uvc-util" / "build" / "Release" / "uvc-util",
            home / "src" / "uvc-util" / "build" / "Release" / "uvc-util",
            home / "Developer" / "uvc-util" / "build" / "Release" / "uvc-util",
        ]
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    return None


def camera_profile_patch_stats(frame: bytes) -> dict[str, float | int]:
    """Summarize a display-background patch that stays dark in the fixed rig."""
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"camera calibration frame has {len(frame)} bytes; expected {FRAME_BYTES}")
    x0, y0, x1, y1 = CALIBRATION_PATCH
    values = sorted(frame[y * FRAME_WIDTH + x] for y in range(y0, y1) for x in range(x0, x1))
    return {
        "mean": round(sum(values) / len(values), 3),
        "median": values[len(values) // 2],
        "p90": values[int(len(values) * 0.9)],
    }


def evaluate_camera_profile_frames(preflight: bytes, video: bytes) -> dict[str, Any]:
    """Reject a recorder handoff that changes the calibrated live exposure."""
    preflight_stats = camera_profile_patch_stats(preflight)
    video_stats = camera_profile_patch_stats(video)
    median_limit = max(20, int(preflight_stats["median"]) * 4 + 4)
    passed = int(video_stats["median"]) <= median_limit
    return {
        "result": "PASS" if passed else "FAIL",
        "video_time_seconds": CALIBRATION_VIDEO_TIME_S,
        "normalized_size": f"{FRAME_WIDTH}x{FRAME_HEIGHT}",
        "patch": list(CALIBRATION_PATCH),
        "preflight": preflight_stats,
        "video": video_stats,
        "video_median_max": median_limit,
        "message": ""
        if passed
        else (
            "camera video calibration changed after recorder handoff "
            f"(background median {video_stats['median']} > {median_limit})"
        ),
    }


class CameraCapture:
    """Own the native recorder process and evidence files for a single suite."""

    def __init__(self, out_dir: Path, expected_duration_s: int):
        self.out_dir = out_dir.resolve()
        self.expected_duration_s = expected_duration_s
        self.camera_name = os.environ.get("BENCH_CAMERA_NAME", "Global Shutter Camera")
        self.camera_device_index = int(os.environ.get("BENCH_CAMERA_DEVICE_INDEX", "0"))
        self.focus = int(os.environ.get("BENCH_CAMERA_FOCUS", "306"))
        self.framerate = int(os.environ.get("BENCH_CAMERA_FRAMERATE", "200"))
        self.input_pixel_format = os.environ.get("BENCH_CAMERA_PIXEL_FORMAT", "nv12")
        self.video_size = os.environ.get("BENCH_CAMERA_VIDEO_SIZE", "1280x720")
        self.capture_backend = "avfoundation_native"
        self.uvc_util = _find_uvc_util()
        self.ffmpeg = shutil.which("ffmpeg")
        self.ffprobe = shutil.which("ffprobe")
        self.swift = shutil.which("swift")
        self.native_recorder = Path(__file__).with_name("camera_recorder.swift").resolve()
        self.video_path = self.out_dir / f"evidence_exp{VIDEO_EXPOSURE}.mov"
        self.native_preflight_path = self.out_dir / ".camera_preflight.mov"
        self.preflight_path = self.out_dir / f"session_start_exp{VIDEO_EXPOSURE}.jpg"
        self.bright_path = self.out_dir / "final_auto.jpg"
        self.dim_path = self.out_dir / "final_profile.jpg"
        self.log_path = self.out_dir / "camera.log"
        self.result_path = self.out_dir / "camera_result.json"
        self.preflight_result_path = self.out_dir / "camera_preflight.json"
        self.session_ready_path = self.out_dir / ".camera_session_ready.json"
        self.start_marker_path = self.out_dir / ".camera_start"
        self.preflight_ready_path = self.out_dir / ".camera_preflight_ready.json"
        self.preflight_stop_path = self.out_dir / ".camera_preflight_stop"
        self.preflight_finished_path = self.out_dir / ".camera_preflight_finished.json"
        self.recording_ready_path = self.out_dir / ".camera_recording_ready.json"
        self.process: subprocess.Popen[bytes] | None = None
        self.recording_started_monotonic: float | None = None
        self.log_handle: Any = None
        self.errors: list[str] = []
        self.recorder_session: dict[str, Any] = {}
        self.profile_readback: dict[str, Any] = {}

    def profile(self) -> dict[str, Any]:
        return {
            "auto_exposure_mode": 8,
            "auto_exposure_priority": 0,
            "focus_abs": self.focus,
            "video_exposure_time_abs": VIDEO_EXPOSURE,
            "gain": 0,
            "framerate": self.framerate,
            "input_pixel_format": self.input_pixel_format,
            "video_size": self.video_size,
            "capture_backend": self.capture_backend,
        }

    def _write_result(self, result: str, **extra: Any) -> None:
        payload: dict[str, Any] = {
            "schema_version": 2,
            "kind": "bench_camera_evidence",
            "result": result,
            "timestamp_utc": utc_now(),
            "camera_name": self.camera_name,
            "camera_device_index": self.camera_device_index,
            "profile": self.profile(),
            "profile_readback": self.profile_readback,
            "recorder_session": self.recorder_session,
            "expected_duration_seconds": self.expected_duration_s,
            "errors": self.errors,
        }
        payload.update(extra)
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self.result_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    def _require_tools(self) -> None:
        missing: list[str] = []
        if self.uvc_util is None:
            missing.append("uvc-util (set BENCH_UVC_UTIL)")
        if not self.ffmpeg:
            missing.append("ffmpeg")
        if not self.ffprobe:
            missing.append("ffprobe")
        if not self.swift:
            missing.append("swift")
        if not self.native_recorder.is_file():
            missing.append("scripts/bench/camera_recorder.swift")
        if missing:
            raise RuntimeError("missing camera tools: " + ", ".join(missing))

    def _set_control(self, name: str, value: int) -> None:
        assert self.uvc_util is not None
        proc = subprocess.run(
            [str(self.uvc_util), "-N", self.camera_name, "-s", f"{name}={value}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            detail = proc.stdout.strip().splitlines()
            suffix = f": {detail[-1]}" if detail else ""
            raise RuntimeError(f"camera control {name} failed{suffix}")

    def _get_control_value(self, name: str) -> int | bool:
        assert self.uvc_util is not None
        proc = subprocess.run(
            [str(self.uvc_util), "-N", self.camera_name, "-o", name],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        value = proc.stdout.strip().splitlines()[-1].strip() if proc.stdout.strip() else ""
        if proc.returncode != 0 or not value:
            raise RuntimeError(f"camera control {name} could not be read")
        if value.lower() in {"true", "false"}:
            return value.lower() == "true"
        try:
            return int(value)
        except ValueError as exc:
            raise RuntimeError(f"camera control {name} returned an invalid value: {value}") from exc

    def _configure(self, exposure: int) -> None:
        video_profile = exposure == VIDEO_EXPOSURE
        controls = [
            ("auto-focus", 0),
            ("focus-abs", self.focus),
            ("auto-exposure-mode", 1),
            ("auto-exposure-priority", 0),
            ("auto-white-balance-temp", 0),
            ("white-balance-temp", 4650),
            ("backlight-compensation", 72),
            ("power-line-frequency", 1),
            ("brightness", 128),
            ("gamma", 128),
            ("contrast", 64),
            ("saturation", 78),
            ("gain", 0 if video_profile else 190),
            ("sharpness", 128),
        ]
        if video_profile:
            # Exposure is read-only in aperture-priority mode on this camera.
            # Seed the fixed 5 ms value manually, then restore mode 8 before
            # any evidence is recorded.
            controls.insert(3, ("exposure-time-abs", VIDEO_EXPOSURE))
            controls.append(("auto-exposure-mode", 8))
        else:
            controls.append(("exposure-time-abs", exposure))
        for name, value in controls:
            self._set_control(name, value)

    def _validate_live_profile(self) -> dict[str, int | bool]:
        expected: dict[str, int | bool] = {
            "auto-focus": False,
            "focus-abs": self.focus,
            "auto-exposure-mode": 8,
            "auto-exposure-priority": 0,
            "exposure-time-abs": VIDEO_EXPOSURE,
            "gain": 0,
            "auto-white-balance-temp": False,
            "white-balance-temp": 4650,
        }
        measured = {name: self._get_control_value(name) for name in expected}
        mismatches = {
            name: {"expected": expected[name], "measured": value}
            for name, value in measured.items()
            if value != expected[name]
        }
        self.profile_readback = measured
        if mismatches:
            detail = ", ".join(
                f"{name}={values['measured']} (expected {values['expected']})"
                for name, values in mismatches.items()
            )
            raise RuntimeError(f"camera live profile mismatch: {detail}")
        return measured

    def _decode_profile_frame(self, path: Path, video_time_s: float | None = None) -> bytes:
        assert self.ffmpeg is not None
        command = [self.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error"]
        if video_time_s is not None:
            command.extend(["-ss", str(video_time_s)])
        command.extend(
            [
                "-i",
                str(path),
                "-frames:v",
                "1",
                "-vf",
                f"{DISPLAY_CROP},scale={FRAME_WIDTH}:{FRAME_HEIGHT}:flags=area",
                "-f",
                "rawvideo",
                "-pix_fmt",
                "gray",
                "-",
            ]
        )
        proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if proc.returncode != 0:
            detail = proc.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(f"camera calibration frame decode failed: {detail or f'exit {proc.returncode}'}")
        if len(proc.stdout) != FRAME_BYTES:
            raise RuntimeError(
                f"camera calibration frame has {len(proc.stdout)} decoded bytes; expected {FRAME_BYTES}"
            )
        return proc.stdout

    def _validate_recording_profile(self) -> dict[str, Any]:
        preflight = self._decode_profile_frame(self.preflight_path)
        video = self._decode_profile_frame(self.video_path, CALIBRATION_VIDEO_TIME_S)
        return evaluate_camera_profile_frames(preflight, video)

    def _extract_video_still(self, video_path: Path, still_path: Path, video_time_s: float) -> None:
        assert self.ffmpeg is not None
        proc = subprocess.run(
            [
                self.ffmpeg,
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "error",
                "-ss",
                str(video_time_s),
                "-i",
                str(video_path),
                "-frames:v",
                "1",
                "-y",
                str(still_path),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if proc.returncode != 0 or not still_path.is_file() or still_path.stat().st_size == 0:
            detail = proc.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(f"native camera still extraction failed: {detail or f'exit {proc.returncode}'}")

    def _wait_for_marker(self, path: Path, timeout_s: float, label: str) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if path.is_file():
                try:
                    return json.loads(path.read_text(encoding="utf-8"))
                except (json.JSONDecodeError, OSError):
                    pass
            if self.process is not None and self.process.poll() is not None:
                raise RuntimeError(f"camera recorder exited before {label} (code {self.process.returncode})")
            time.sleep(0.05)
        raise RuntimeError(f"camera recorder timed out waiting for {label}")

    def start(self) -> bool:
        self.out_dir.mkdir(parents=True, exist_ok=True)
        try:
            self._require_tools()
            assert self.swift is not None
            self.log_handle = self.log_path.open("wb")
            command = [
                self.swift,
                str(self.native_recorder),
                "--device-name",
                self.camera_name,
                "--video-size",
                self.video_size,
                "--framerate",
                str(self.framerate),
                "--pixel-format",
                self.input_pixel_format,
                "--output",
                str(self.video_path),
                "--preflight-output",
                str(self.native_preflight_path),
                "--session-ready",
                str(self.session_ready_path),
                "--start-marker",
                str(self.start_marker_path),
                "--preflight-ready",
                str(self.preflight_ready_path),
                "--preflight-stop",
                str(self.preflight_stop_path),
                "--preflight-finished",
                str(self.preflight_finished_path),
                "--recording-ready",
                str(self.recording_ready_path),
            ]
            self.process = subprocess.Popen(
                command,
                stdout=self.log_handle,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            self.recorder_session = self._wait_for_marker(
                self.session_ready_path,
                CAMERA_SESSION_READY_TIMEOUT_S,
                "native session readiness",
            )
            # Starting the AVFoundation session selects the exact 720p/200
            # format but may reset UVC controls. Apply and verify the profile
            # while that session owns the camera, before admitting the bench.
            self._configure(VIDEO_EXPOSURE)
            time.sleep(CAMERA_PROFILE_SETTLE_S)
            self._validate_live_profile()
            self.start_marker_path.write_text("start\n", encoding="utf-8")
            self._wait_for_marker(
                self.preflight_ready_path,
                CAMERA_RECORDING_READY_TIMEOUT_S,
                "native preflight recording readiness",
            )
            time.sleep(CAMERA_PREFLIGHT_RECORD_SECONDS)
            self.preflight_stop_path.write_text("stop\n", encoding="utf-8")
            self._wait_for_marker(
                self.preflight_finished_path,
                CAMERA_RECORDING_READY_TIMEOUT_S,
                "native preflight recording finalization",
            )
            self._wait_for_marker(
                self.recording_ready_path,
                CAMERA_RECORDING_READY_TIMEOUT_S,
                "recording readiness",
            )
            self.recording_started_monotonic = time.monotonic()
            self._extract_video_still(self.native_preflight_path, self.preflight_path, 0.5)
            self._write_result("RECORDING")
            return True
        except Exception as exc:  # noqa: BLE001 - retain evidence and keep the metrics run going
            self.errors.append(str(exc))
            self._stop_process()
            self._write_result("CAPTURE_FAILED")
            return False

    def _stop_process(self) -> None:
        process = self.process
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGINT)
                process.wait(timeout=20)
            except (OSError, subprocess.TimeoutExpired):
                if process.poll() is None:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except OSError:
                        pass
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        pass
        self.process = None
        if self.log_handle is not None:
            self.log_handle.close()
            self.log_handle = None

    def abort(self, diagnostic_code: str) -> dict[str, Any]:
        """Stop an admitted recorder without finalizing capture evidence."""
        message = f"camera preflight inconclusive: {diagnostic_code}"
        if message not in self.errors:
            self.errors.append(message)
        self._stop_process()
        payload = {
            "video": self.video_path.name if self.video_path.is_file() else "",
            "video_duration_seconds": 0.0,
            "session_start_still": self.preflight_path.name if self.preflight_path.is_file() else "",
            "bright_still": "",
            "dim_still": "",
            "visually_graded": False,
            "profile_validation": {},
        }
        self._write_result("CAPTURE_FAILED", **payload)
        return {"result": "CAPTURE_FAILED", **payload, "errors": list(self.errors)}

    def _probe_video(self) -> dict[str, Any]:
        assert self.ffprobe is not None
        proc = subprocess.run(
            [
                self.ffprobe,
                "-v",
                "error",
                "-show_entries",
                "format=duration:stream=width,height,avg_frame_rate,nb_frames,codec_name",
                "-of",
                "json",
                str(self.video_path),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise RuntimeError("camera video could not be verified")
        try:
            payload = json.loads(proc.stdout)
            stream = payload["streams"][0]
            numerator, denominator = str(stream["avg_frame_rate"]).split("/", 1)
            frame_rate = float(numerator) / float(denominator)
            result = {
                "duration_seconds": round(float(payload["format"]["duration"]), 3),
                "width": int(stream["width"]),
                "height": int(stream["height"]),
                "average_frame_rate": round(frame_rate, 3),
                "codec": str(stream.get("codec_name", "")),
            }
            if str(stream.get("nb_frames", "")).isdigit():
                result["frame_count"] = int(stream["nb_frames"])
        except (KeyError, IndexError, TypeError, ValueError, ZeroDivisionError) as exc:
            raise RuntimeError("camera video probe is malformed") from exc

        return result

    def _validate_video_probe(self, result: dict[str, Any]) -> None:
        """Require the recorded stream to match the requested source profile."""
        expected_width, expected_height = (int(value) for value in self.video_size.split("x", 1))
        if (result["width"], result["height"]) != (expected_width, expected_height):
            raise RuntimeError(
                "camera video size does not match the requested profile "
                f"({result['width']}x{result['height']} != {self.video_size})"
            )
        if result["average_frame_rate"] < self.framerate - 1.0:
            raise RuntimeError(
                "camera video frame rate is below the requested profile "
                f"({result['average_frame_rate']:.3f} < {self.framerate - 1.0:.1f})"
            )

    def stop(self, collection_completed: bool) -> dict[str, Any]:
        was_running = self.process is not None
        self._stop_process()
        duration = 0.0
        video_probe: dict[str, Any] = {}
        profile_validation: dict[str, Any] = {}
        if was_running:
            try:
                video_probe = self._probe_video()
                duration = float(video_probe["duration_seconds"])
                try:
                    self._validate_video_probe(video_probe)
                except RuntimeError as exc:
                    # Retain the measured probe and finish collecting the
                    # diagnostic stills even though this capture cannot pass.
                    self.errors.append(str(exc))
                profile_validation = self._validate_recording_profile()
                if profile_validation.get("result") != "PASS":
                    self.errors.append(str(profile_validation.get("message") or "camera calibration failed"))
                self._extract_video_still(
                    self.video_path,
                    self.bright_path,
                    max(0.0, duration - 0.5),
                )
                self._extract_video_still(
                    self.video_path,
                    self.dim_path,
                    max(0.0, duration - 1.0),
                )
                self._validate_live_profile()
                minimum = max(1.0, float(self.expected_duration_s) - 5.0) if collection_completed else 1.0
                if duration < minimum:
                    self.errors.append(
                        f"camera video is too short ({duration:.1f}s; need {minimum:.1f}s)"
                    )
            except Exception as exc:  # noqa: BLE001 - result artifact is the contract
                self.errors.append(str(exc))

        captured = (
            was_running
            and not self.errors
            and self.video_path.is_file()
            and self.preflight_path.is_file()
            and self.bright_path.is_file()
            and self.dim_path.is_file()
        )
        result = "CAPTURED" if captured else "CAPTURE_FAILED"
        payload = {
            "video": self.video_path.name if self.video_path.is_file() else "",
            "video_duration_seconds": round(duration, 3),
            "video_probe": video_probe,
            "session_start_still": self.preflight_path.name if self.preflight_path.is_file() else "",
            "bright_still": self.bright_path.name if self.bright_path.is_file() else "",
            "dim_still": self.dim_path.name if self.dim_path.is_file() else "",
            "visually_graded": False,
            "profile_validation": profile_validation,
        }
        self._write_result(result, **payload)
        return {"result": result, **payload, "errors": list(self.errors)}
