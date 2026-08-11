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


# Video exposure in UVC units of 0.1 ms; 156 == 15.6 ms is the calibrated
# default and every archived run to date used it. The environment override is
# retained for explicit camera development, but fixed-profile preflight refuses
# any non-profile value before live product collection.
VIDEO_EXPOSURE = int(os.environ.get("BENCH_CAMERA_VIDEO_EXPOSURE", "156"))

FRAME_WIDTH = 480
FRAME_HEIGHT = 200
FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT
DISPLAY_CROP = "crop=iw*0.52:ih*0.38:iw*0.18:ih*0.25"
CALIBRATION_VIDEO_TIME_S = 3.0
CALIBRATION_PATCH = (150, 20, 260, 45)
CAMERA_OPEN_SETTLE_S = 0.5
CAMERA_PROFILE_SETTLE_S = 1.5


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
    """Own the ffmpeg process and evidence files for a single suite."""

    def __init__(self, out_dir: Path, expected_duration_s: int):
        self.out_dir = out_dir.resolve()
        self.expected_duration_s = expected_duration_s
        self.camera_name = os.environ.get("BENCH_CAMERA_NAME", "Razer Kiyo")
        self.camera_device_index = int(os.environ.get("BENCH_CAMERA_DEVICE_INDEX", "0"))
        self.focus = int(os.environ.get("BENCH_CAMERA_FOCUS", "208"))
        self.framerate = int(os.environ.get("BENCH_CAMERA_FRAMERATE", "30"))
        self.input_pixel_format = os.environ.get("BENCH_CAMERA_PIXEL_FORMAT", "nv12")
        self.video_size = os.environ.get("BENCH_CAMERA_VIDEO_SIZE", "1280x720")
        self.uvc_util = _find_uvc_util()
        self.ffmpeg = shutil.which("ffmpeg")
        self.ffprobe = shutil.which("ffprobe")
        self.imagesnap = shutil.which("imagesnap")
        self.video_path = self.out_dir / f"evidence_exp{VIDEO_EXPOSURE}.mp4"
        self.preflight_path = self.out_dir / f"session_start_exp{VIDEO_EXPOSURE}.jpg"
        self.bright_path = self.out_dir / "final_exp5.jpg"
        self.dim_path = self.out_dir / "final_exp1250.jpg"
        self.log_path = self.out_dir / "camera.log"
        self.result_path = self.out_dir / "camera_result.json"
        self.preflight_result_path = self.out_dir / "camera_preflight.json"
        self.process: subprocess.Popen[bytes] | None = None
        self.recording_started_monotonic: float | None = None
        self.log_handle: Any = None
        self.errors: list[str] = []

    def profile(self) -> dict[str, Any]:
        return {
            "auto_exposure_priority": 0,
            "focus_abs": self.focus,
            "video_exposure_time_abs": VIDEO_EXPOSURE,
            "bright_exposure_time_abs": 5,
            "dim_exposure_time_abs": 1250,
            "framerate": self.framerate,
            "input_pixel_format": self.input_pixel_format,
            "video_size": self.video_size,
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
        if not self.imagesnap:
            missing.append("imagesnap")
        if missing:
            raise RuntimeError("missing camera tools: " + ", ".join(missing))

    def _set_control(self, name: str, value: int) -> None:
        assert self.uvc_util is not None
        proc = subprocess.run(
            [str(self.uvc_util), "-I", str(self.camera_device_index), "-s", f"{name}={value}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            detail = proc.stdout.strip().splitlines()
            suffix = f": {detail[-1]}" if detail else ""
            raise RuntimeError(f"camera control {name} failed{suffix}")

    def _configure(self, exposure: int) -> None:
        controls = [
            ("auto-focus", 0),
            ("focus-abs", self.focus),
            ("auto-exposure-mode", 1),
            ("auto-exposure-priority", 0),
            ("auto-white-balance-temp", 0),
            ("white-balance-temp", 4000),
            ("backlight-compensation", 0),
            ("power-line-frequency", 2),
            ("gain", 0),
            ("sharpness", 128),
            ("exposure-time-abs", exposure),
        ]
        for name, value in controls:
            self._set_control(name, value)

    def _snapshot(self, exposure: int, path: Path) -> None:
        assert self.imagesnap is not None
        proc = subprocess.Popen(
            [self.imagesnap, "-q", "-w", "2", "-d", self.camera_name, str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        output = ""
        try:
            # imagesnap opens a new macOS camera consumer and can reset the
            # UVC profile. Configure only after it owns the camera, then use
            # the remainder of its two-second wait as the profile settle.
            time.sleep(CAMERA_OPEN_SETTLE_S)
            if proc.poll() is None:
                self._configure(exposure)
            output, _ = proc.communicate()
        except Exception:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2)
            raise
        if proc.returncode != 0 or not path.is_file() or path.stat().st_size == 0:
            detail = output.strip().splitlines()
            suffix = f": {detail[-1]}" if detail else ""
            raise RuntimeError(f"camera snapshot failed{suffix}")

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

    def start(self) -> bool:
        self.out_dir.mkdir(parents=True, exist_ok=True)
        try:
            self._require_tools()
            self._configure(VIDEO_EXPOSURE)
            self._snapshot(VIDEO_EXPOSURE, self.preflight_path)
            assert self.ffmpeg is not None
            self.log_handle = self.log_path.open("wb")
            command = [
                self.ffmpeg,
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "warning",
                "-y",
                "-f",
                "avfoundation",
                "-framerate",
                str(self.framerate),
                "-video_size",
                self.video_size,
                "-pixel_format",
                self.input_pixel_format,
                "-i",
                f"{self.camera_name}:none",
                "-an",
                "-c:v",
                "libx264",
                "-preset",
                "veryfast",
                "-crf",
                "20",
                "-pix_fmt",
                "yuv420p",
                str(self.video_path),
            ]
            self.process = subprocess.Popen(
                command,
                stdout=self.log_handle,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            self.recording_started_monotonic = time.monotonic()
            # Opening a new macOS camera consumer can reset UVC controls even
            # when the preflight still was calibrated. Reapply the complete
            # profile after ffmpeg owns the live stream, then allow it to
            # settle before the bench window can begin.
            time.sleep(CAMERA_OPEN_SETTLE_S)
            if self.process.poll() is not None:
                raise RuntimeError(f"camera recorder exited early with code {self.process.returncode}")
            self._configure(VIDEO_EXPOSURE)
            time.sleep(CAMERA_PROFILE_SETTLE_S)
            if self.process.poll() is not None:
                raise RuntimeError(f"camera recorder exited early with code {self.process.returncode}")
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
                process.wait(timeout=3)
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

    def _probe_video(self) -> dict[str, float | int]:
        assert self.ffprobe is not None
        proc = subprocess.run(
            [
                self.ffprobe,
                "-v",
                "error",
                "-show_entries",
                "format=duration:stream=width,height,avg_frame_rate",
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
            }
        except (KeyError, IndexError, TypeError, ValueError, ZeroDivisionError) as exc:
            raise RuntimeError("camera video probe is malformed") from exc

        return result

    def _validate_video_probe(self, result: dict[str, float | int]) -> None:
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
        video_probe: dict[str, float | int] = {}
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
                self._snapshot(5, self.bright_path)
                self._snapshot(1250, self.dim_path)
                self._set_control("exposure-time-abs", VIDEO_EXPOSURE)
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
