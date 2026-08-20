#!/usr/bin/env python3
"""Advisory renderer/camera cadence investigation for replay arrow blink.

The caller owns and validates every input.  This module only derives bounded
facts; it has no verdict, threshold exit, or publication behavior.
"""

from __future__ import annotations

import csv
import math
import re
import shutil
import statistics
import subprocess
import sys
import threading
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from camera_grade import FRAME_HEIGHT, FRAME_WIDTH, _display_crop_filter, _orange


DECLARED_HALF_PERIOD_MS = 96.0
FRONT_ARROW_BOUNDS = (350, 22, 455, 78)
SHOWINFO_PTS = re.compile(r"\bpts_time:([-+0-9.eE]+)")


class BlinkCadenceError(RuntimeError):
    """Stable investigator refusal; never a product verdict."""


def _mean(values: list[float]) -> float | None:
    return sum(values) / len(values) if values else None


def _rounded(value: float | None) -> float | None:
    return round(value, 3) if value is not None else None


def summarize_boolean_timeline(
    samples: list[tuple[float, bool]],
    *,
    gap_limit_ms: float | None,
) -> dict[str, Any]:
    """Summarize accepted state transitions using mean interval as truth."""
    if len(samples) < 3:
        return {"status": "partial", "reason": "samples_insufficient"}
    times = [item[0] for item in samples]
    if any(not math.isfinite(value) for value in times) or any(
        current <= previous for previous, current in zip(times, times[1:])
    ):
        return {"status": "partial", "reason": "timestamps_invalid"}

    frame_gaps_ms = [(current - previous) * 1000.0 for previous, current in zip(times, times[1:])]
    maximum_gap_ms = max(frame_gaps_ms, default=0.0)
    if gap_limit_ms is not None and maximum_gap_ms > gap_limit_ms:
        return {
            "status": "partial",
            "reason": "pts_gap_exceeds_half_period",
            "sample_count": len(samples),
            "maximum_sample_gap_ms": _rounded(maximum_gap_ms),
            "sample_gap_limit_ms": _rounded(gap_limit_ms),
        }

    transitions = [
        (time_s, state)
        for (previous_time, previous_state), (time_s, state) in zip(samples, samples[1:])
        if state != previous_state
    ]
    if len(transitions) < 3:
        return {
            "status": "partial",
            "reason": "transitions_insufficient",
            "sample_count": len(samples),
            "transition_count": len(transitions),
            "maximum_sample_gap_ms": _rounded(maximum_gap_ms),
        }

    intervals: list[float] = []
    on_intervals: list[float] = []
    off_intervals: list[float] = []
    for (start_s, state), (end_s, _next_state) in zip(transitions, transitions[1:]):
        duration_ms = (end_s - start_s) * 1000.0
        intervals.append(duration_ms)
        (on_intervals if state else off_intervals).append(duration_ms)

    mean_half_period_ms = _mean(intervals)
    return {
        "status": "complete",
        "reason": "",
        "sample_count": len(samples),
        "transition_count": len(transitions),
        "interval_count": len(intervals),
        "mean_half_period_ms": _rounded(mean_half_period_ms),
        # Diagnostic only: alternating duty makes this distribution bimodal.
        "median_half_period_ms": _rounded(statistics.median(intervals)),
        "mean_on_ms": _rounded(_mean(on_intervals)),
        "mean_off_ms": _rounded(_mean(off_intervals)),
        "declared_half_period_ms": DECLARED_HALF_PERIOD_MS,
        "deviation_from_declared_percent": _rounded(
            abs(float(mean_half_period_ms) - DECLARED_HALF_PERIOD_MS)
            / DECLARED_HALF_PERIOD_MS
            * 100.0
        ),
        "maximum_sample_gap_ms": _rounded(maximum_gap_ms),
        "sample_gap_limit_ms": _rounded(gap_limit_ms),
    }


def _classify_orange_counts(samples: list[tuple[float, int]]) -> tuple[list[tuple[float, bool]], dict[str, Any]]:
    if len(samples) < 20:
        raise BlinkCadenceError("camera_samples_insufficient")
    values = [count for _time_s, count in samples]
    centers = [float(min(values)), float(statistics.median(values)), float(max(values))]
    groups: list[list[int]] = []
    for _iteration in range(30):
        groups = [[], [], []]
        for value in values:
            index = min(range(3), key=lambda item: (abs(value - centers[item]), item))
            groups[index].append(value)
        if any(not group for group in groups):
            raise BlinkCadenceError("camera_blink_contrast_insufficient")
        updated = [sum(group) / len(group) for group in groups]
        if sum(abs(left - right) for left, right in zip(centers, updated)) < 1e-6:
            centers = updated
            break
        centers = updated
    low, middle, high = sorted(centers)
    if middle - low < max(20.0, middle * 0.20):
        raise BlinkCadenceError("camera_blink_contrast_insufficient")
    # The optical ON state is itself bimodal: a short fully illuminated plateau
    # followed by the panel's dim response tail.  Split darkness from both lit
    # clusters; using only the extreme high cluster invents extra transitions.
    low_threshold = low + (middle - low) * 0.35
    high_threshold = low + (middle - low) * 0.65
    state = samples[0][1] >= (low + middle) / 2.0
    classified: list[tuple[float, bool]] = []
    for time_s, count in samples:
        if state and count <= low_threshold:
            state = False
        elif not state and count >= high_threshold:
            state = True
        classified.append((time_s, state))
    return classified, {
        "orange_low_center": round(low, 3),
        "orange_response_center": round(middle, 3),
        "orange_high_center": round(high, 3),
        "off_threshold": round(low_threshold, 3),
        "on_threshold": round(high_threshold, 3),
    }


def summarize_camera_episode(
    classified: list[tuple[float, bool]],
    *,
    start_s: float,
    end_s: float,
) -> dict[str, Any]:
    selected = [item for item in classified if start_s <= item[0] <= end_s]
    overlap_gaps_ms = [
        (current[0] - previous[0]) * 1000.0
        for previous, current in zip(classified, classified[1:])
        if previous[0] < end_s and current[0] > start_s
    ]
    maximum_overlap_gap_ms = max(overlap_gaps_ms, default=0.0)
    if maximum_overlap_gap_ms > DECLARED_HALF_PERIOD_MS:
        return {
            "status": "partial",
            "reason": "pts_gap_exceeds_half_period",
            "sample_count": len(selected),
            "maximum_pts_gap_ms": _rounded(maximum_overlap_gap_ms),
            "pts_gap_limit_ms": DECLARED_HALF_PERIOD_MS,
        }
    result = summarize_boolean_timeline(selected, gap_limit_ms=DECLARED_HALF_PERIOD_MS)
    if "maximum_sample_gap_ms" in result:
        result["maximum_pts_gap_ms"] = result.pop("maximum_sample_gap_ms")
    if "sample_gap_limit_ms" in result:
        result["pts_gap_limit_ms"] = result.pop("sample_gap_limit_ms")
    return result


def _read_exact(stream: Any, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def decode_front_arrow_samples(
    video_path: Path,
    *,
    start_s: float,
    end_s: float,
    crop_registration: dict[str, Any],
    ffmpeg: str | None = None,
) -> tuple[list[tuple[float, int]], dict[str, Any]]:
    executable = ffmpeg or shutil.which("ffmpeg")
    if not executable:
        raise BlinkCadenceError("ffmpeg_missing")
    if not (math.isfinite(start_s) and math.isfinite(end_s) and 0 <= start_s < end_s):
        raise BlinkCadenceError("camera_interval_invalid")
    x0, y0, x1, y1 = FRONT_ARROW_BOUNDS
    width, height = x1 - x0, y1 - y0
    frame_bytes = width * height * 3
    select = f"select=between(t\\,{start_s:.6f}\\,{end_s:.6f})"
    filters = (
        f"{select},{_display_crop_filter(0.0, 0.0, crop_registration)},"
        f"scale={FRAME_WIDTH}:{FRAME_HEIGHT}:flags=area,"
        f"crop={width}:{height}:{x0}:{y0},showinfo"
    )
    process = subprocess.Popen(
        [
            executable,
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "info",
            "-i",
            str(video_path),
            "-vf",
            filters,
            "-fps_mode",
            "passthrough",
            "-an",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb24",
            "-",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdout is not None and process.stderr is not None
    pts: list[float] = []
    stderr_tail: list[str] = []

    def drain_stderr() -> None:
        for raw in process.stderr:
            line = raw.decode("utf-8", errors="replace")
            match = SHOWINFO_PTS.search(line)
            if match:
                try:
                    pts.append(float(match.group(1)))
                except ValueError:
                    pass
            if len(stderr_tail) >= 8:
                stderr_tail.pop(0)
            stderr_tail.append(line.strip())

    thread = threading.Thread(target=drain_stderr, daemon=True)
    thread.start()
    counts: list[int] = []
    while True:
        frame = _read_exact(process.stdout, frame_bytes)
        if not frame:
            break
        if len(frame) != frame_bytes:
            process.kill()
            thread.join(timeout=2.0)
            raise BlinkCadenceError("camera_frame_partial")
        counts.append(
            sum(
                _orange(frame[offset], frame[offset + 1], frame[offset + 2])
                for offset in range(0, len(frame), 3)
            )
        )
    returncode = process.wait()
    thread.join(timeout=2.0)
    if returncode != 0:
        raise BlinkCadenceError("camera_decode_failed")
    if len(pts) != len(counts) or not counts:
        raise BlinkCadenceError("camera_pts_frame_mismatch")
    return list(zip(pts, counts)), {"decoded_frame_count": len(counts)}


def _load_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(line for line in handle if not line.startswith("#")))
    if not rows:
        raise BlinkCadenceError("csv_empty")
    return rows


def analyze_blink_cadence(
    *,
    display_commit_path: Path,
    encounter_path: Path,
    video_path: Path,
    grade: dict[str, Any],
    ffmpeg: str | None = None,
) -> dict[str, Any]:
    try:
        display_rows = _load_rows(display_commit_path)
        encounter_rows = _load_rows(encounter_path)
        flash_rows = [row for row in display_rows if int(row["flash_bits"]) != 0]
        if not flash_rows:
            raise BlinkCadenceError("renderer_flash_episode_missing")
        priority_rows = [row for row in encounter_rows if int(row["priority"]) == 1]
        if not priority_rows:
            raise BlinkCadenceError("encounter_anchor_missing")
        anchor_millis = int(priority_rows[0]["millis"])
        offset_s = float(grade["alignment"]["selected_video_offset_seconds"])
        registration = grade.get("crop_registration")
        if grade.get("alignment", {}).get("result") != "PASS" or not isinstance(registration, dict):
            raise BlinkCadenceError("camera_alignment_missing")

        flash_start_ms = int(flash_rows[0]["millis"])
        flash_end_ms = int(flash_rows[-1]["millis"])
        replay_start_s = 5.0 + (flash_start_ms - anchor_millis) / 1000.0
        replay_end_s = 5.0 + (flash_end_ms - anchor_millis) / 1000.0
        video_start_s = offset_s + replay_start_s
        video_end_s = offset_s + replay_end_s

        pushed = [row for row in flash_rows if int(row["pushes"]) > 0]
        renderer_samples = [(int(row["millis"]) / 1000.0, bool(int(row["blink_phase"]))) for row in pushed]
        renderer = summarize_boolean_timeline(renderer_samples, gap_limit_ms=None)
        renderer.pop("maximum_sample_gap_ms", None)
        renderer.pop("sample_gap_limit_ms", None)
        renderer.update(
            {
                "flash_commit_count": len(flash_rows),
                "physical_push_count": len(pushed),
                "partial_dispatch_count": sum(row["dispatch"] == "PARTIAL" for row in flash_rows),
                "reported_dropped_commits": max(int(row["dropped_commits"]) for row in display_rows),
            }
        )

        guard_s = DECLARED_HALF_PERIOD_MS / 1000.0
        orange_samples, decode = decode_front_arrow_samples(
            video_path,
            start_s=max(0.0, video_start_s - guard_s),
            end_s=video_end_s + guard_s,
            crop_registration=registration,
            ffmpeg=ffmpeg,
        )
        classified, calibration = _classify_orange_counts(orange_samples)
        camera = summarize_camera_episode(classified, start_s=video_start_s, end_s=video_end_s)
        camera.update(decode)
        camera["classification"] = calibration
        return {
            "status": "complete" if renderer.get("status") == camera.get("status") == "complete" else "partial",
            "reason": camera.get("reason") or renderer.get("reason") or "",
            "declared_half_period_ms": DECLARED_HALF_PERIOD_MS,
            "episode": {
                "replay_start_seconds": round(replay_start_s, 3),
                "replay_end_seconds": round(replay_end_s, 3),
                "video_start_seconds": round(video_start_s, 3),
                "video_end_seconds": round(video_end_s, 3),
                "dut_start_millis": flash_start_ms,
                "dut_end_millis": flash_end_ms,
            },
            "renderer": renderer,
            "camera": camera,
        }
    except (BlinkCadenceError, KeyError, TypeError, ValueError, OSError, csv.Error) as exc:
        reason = str(exc) if isinstance(exc, BlinkCadenceError) else "input_invalid"
        return {"status": "partial", "reason": reason, "declared_half_period_ms": DECLARED_HALF_PERIOD_MS}
