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
# default and every archived run to date used it. Override only to answer a
# question the default cannot: a full-canvas flush takes ~17.7 ms, so at 15.6 ms
# exposure a frame that straddles a flush blends old and new content and is
# indistinguishable from a frame that merely straddles a fast blink. Dropping to
# 5 (0.5 ms) freezes the panel mid-write, at the cost of a much darker image.
# Camera-only: firmware, metrics and the perf CSV are untouched, so runs at a
# different exposure stay comparable on every logged number.
VIDEO_EXPOSURE = int(os.environ.get("BENCH_CAMERA_VIDEO_EXPOSURE", "156"))


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


class CameraCapture:
    """Own the ffmpeg process and evidence files for a single suite."""

    def __init__(self, out_dir: Path, expected_duration_s: int):
        self.out_dir = out_dir.resolve()
        self.expected_duration_s = expected_duration_s
        self.camera_name = os.environ.get("BENCH_CAMERA_NAME", "Razer Kiyo")
        self.camera_device_index = int(os.environ.get("BENCH_CAMERA_DEVICE_INDEX", "0"))
        self.focus = int(os.environ.get("BENCH_CAMERA_FOCUS", "208"))
        self.framerate = int(os.environ.get("BENCH_CAMERA_FRAMERATE", "30"))
        self.video_size = os.environ.get("BENCH_CAMERA_VIDEO_SIZE", "1920x1080")
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
        self.process: subprocess.Popen[bytes] | None = None
        self.log_handle: Any = None
        self.errors: list[str] = []

    def _write_result(self, result: str, **extra: Any) -> None:
        payload: dict[str, Any] = {
            "schema_version": 1,
            "kind": "bench_camera_evidence",
            "result": result,
            "timestamp_utc": utc_now(),
            "camera_name": self.camera_name,
            "camera_device_index": self.camera_device_index,
            "profile": {
                "focus_abs": self.focus,
                "video_exposure_time_abs": VIDEO_EXPOSURE,
                "bright_exposure_time_abs": 5,
                "dim_exposure_time_abs": 1250,
                "framerate": self.framerate,
                "video_size": self.video_size,
            },
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
        self._set_control("exposure-time-abs", exposure)
        proc = subprocess.run(
            [self.imagesnap, "-q", "-w", "2", "-d", self.camera_name, str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if proc.returncode != 0 or not path.is_file() or path.stat().st_size == 0:
            detail = proc.stdout.strip().splitlines()
            suffix = f": {detail[-1]}" if detail else ""
            raise RuntimeError(f"camera snapshot failed{suffix}")

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
            time.sleep(2)
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

    def _probe_video(self) -> float:
        assert self.ffprobe is not None
        proc = subprocess.run(
            [
                self.ffprobe,
                "-v",
                "error",
                "-show_entries",
                "format=duration",
                "-of",
                "default=noprint_wrappers=1:nokey=1",
                str(self.video_path),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise RuntimeError("camera video could not be verified")
        return float(proc.stdout.strip())

    def stop(self, collection_completed: bool) -> dict[str, Any]:
        was_running = self.process is not None
        self._stop_process()
        duration = 0.0
        if was_running:
            try:
                duration = self._probe_video()
                self._snapshot(5, self.bright_path)
                self._snapshot(1250, self.dim_path)
                self._set_control("exposure-time-abs", VIDEO_EXPOSURE)
                minimum = max(1.0, float(self.expected_duration_s) - 5.0) if collection_completed else 1.0
                if duration < minimum:
                    raise RuntimeError(f"camera video is too short ({duration:.1f}s; need {minimum:.1f}s)")
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
            "session_start_still": self.preflight_path.name if self.preflight_path.is_file() else "",
            "bright_still": self.bright_path.name if self.bright_path.is_file() else "",
            "dim_still": self.dim_path.name if self.dim_path.is_file() else "",
            "visually_graded": False,
        }
        self._write_result(result, **payload)
        return {"result": result, **payload, "errors": list(self.errors)}
