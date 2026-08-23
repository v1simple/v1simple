#!/usr/bin/env python3
"""Admit a fixed-profile camera by locating the V1Simple display landmark."""

from __future__ import annotations

import math
import shutil
import subprocess
from pathlib import Path
from typing import Any

from artifact_privacy import sanitize_artifact_value
from camera_contract import MAX_DISPLAY_SCALE, MIN_DISPLAY_SCALE


FRAME_WIDTH = 480
FRAME_HEIGHT = 200
DISPLAY_CROP_WIDTH = 0.52
DISPLAY_CROP_HEIGHT = 0.38
DISPLAY_CROP_X = 0.18
DISPLAY_CROP_Y = 0.25

REGISTRATION_WIDTH = 960
REGISTRATION_HEIGHT = 540
REFERENCE_ANCHOR_X = 224.0
REFERENCE_ANCHOR_Y = 83.0
REFERENCE_LANDMARK_WIDTH = 138.0
REFERENCE_LANDMARK_HEIGHT = 51.0
MIN_LANDMARK_FILL_RATIO = 0.10
MAX_LANDMARK_FILL_RATIO = 0.72
MIN_LANDMARK_COLUMN_TEXTURE_RATIO = 0.30
MIN_LANDMARK_INTERNAL_GAP_RUNS = 2
MIN_LANDMARK_WIDEST_GAP_PIXELS = 2
MIN_LANDMARK_ROW_COVERAGE_RATIO = 0.85
MAX_LANDMARK_BLANK_ROW_RUN_PIXELS = 2


class CameraRegistrationError(RuntimeError):
    """Registration refusal with machine-readable camera-only diagnostics."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        measured: dict[str, Any] | None = None,
        thresholds: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.diagnostic = {
            "code": code,
            "message": message,
            "measured": dict(measured or {}),
            "thresholds": dict(thresholds or {}),
        }


def _orange(red: int, green: int, blue: int) -> bool:
    return red >= 70 and red >= green * 1.35 and green >= blue * 1.15


def _decode_registration_frame(path: Path, ffmpeg: str | None = None) -> bytes:
    executable = ffmpeg or shutil.which("ffmpeg")
    if not executable:
        raise RuntimeError("ffmpeg is required to register camera evidence")
    process = subprocess.run(
        [
            executable,
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(path),
            "-frames:v",
            "1",
            "-vf",
            f"scale={REGISTRATION_WIDTH}:{REGISTRATION_HEIGHT}:flags=area",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb24",
            "-",
        ],
        check=False,
        capture_output=True,
    )
    if process.returncode != 0:
        detail = process.stderr.decode("utf-8", errors="replace").strip()
        error = sanitize_artifact_value(
            detail or f"exit {process.returncode}",
            run_dir=path.parent,
        )
        raise RuntimeError(f"camera registration still decode failed: {error}")
    expected_bytes = REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3
    if len(process.stdout) != expected_bytes:
        raise RuntimeError(
            f"camera registration still has {len(process.stdout)} decoded bytes; "
            f"expected {expected_bytes}"
        )
    return process.stdout


def detect_display_crop_registration(
    frame: bytes,
) -> tuple[float, float, dict[str, Any]]:
    """Locate SCAN and return a bounded dynamic close-crop registration."""
    expected_bytes = REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3
    if len(frame) != expected_bytes:
        raise ValueError(
            f"camera registration frame has {len(frame)} bytes; expected {expected_bytes}"
        )

    roi_x0 = int(REGISTRATION_WIDTH * 0.08)
    roi_x1 = int(REGISTRATION_WIDTH * 0.92)
    roi_y0 = int(REGISTRATION_HEIGHT * 0.08)
    roi_y1 = int(REGISTRATION_HEIGHT * 0.92)
    active_columns: list[tuple[int, int]] = []
    for x in range(roi_x0, roi_x1):
        count = 0
        for y in range(roi_y0, roi_y1):
            offset = (y * REGISTRATION_WIDTH + x) * 3
            if _orange(frame[offset], frame[offset + 1], frame[offset + 2]):
                count += 1
        if count:
            active_columns.append((x, count))

    groups: list[list[tuple[int, int]]] = []
    for x, count in active_columns:
        if not groups or x - groups[-1][-1][0] > 20:
            groups.append([])
        groups[-1].append((x, count))
    if not groups:
        raise CameraRegistrationError(
            "screen_landmark_not_found",
            "camera registration still has no orange display landmark",
            measured={"active_columns": 0},
            thresholds={"minimum_active_columns": 1},
        )
    group = max(groups, key=lambda items: sum(count for _x, count in items))
    anchor_x0 = group[0][0]
    anchor_x1 = group[-1][0]
    anchor_ys: list[int] = []
    for y in range(roi_y0, roi_y1):
        for x in range(anchor_x0, anchor_x1 + 1):
            offset = (y * REGISTRATION_WIDTH + x) * 3
            if _orange(frame[offset], frame[offset + 1], frame[offset + 2]):
                anchor_ys.append(y)
    if not anchor_ys:
        raise CameraRegistrationError(
            "screen_landmark_geometry_invalid",
            "camera registration still has no complete display landmark",
            measured={
                "landmark_width_pixels": anchor_x1 - anchor_x0 + 1,
                "landmark_height_pixels": 0,
            },
            thresholds={"width_pixels": [65, 225], "height_pixels": [24, 90]},
        )
    anchor_y0 = min(anchor_ys)
    anchor_y1 = max(anchor_ys)
    anchor_width = anchor_x1 - anchor_x0 + 1
    anchor_height = anchor_y1 - anchor_y0 + 1
    if not (65 <= anchor_width <= 225 and 24 <= anchor_height <= 90):
        raise CameraRegistrationError(
            "screen_landmark_geometry_invalid",
            "camera registration display landmark has unexpected geometry "
            f"({anchor_width}x{anchor_height})",
            measured={
                "landmark_width_pixels": anchor_width,
                "landmark_height_pixels": anchor_height,
            },
            thresholds={"width_pixels": [65, 225], "height_pixels": [24, 90]},
        )

    column_counts: list[int] = []
    for x in range(anchor_x0, anchor_x1 + 1):
        count = 0
        for y in range(anchor_y0, anchor_y1 + 1):
            offset = (y * REGISTRATION_WIDTH + x) * 3
            count += int(_orange(frame[offset], frame[offset + 1], frame[offset + 2]))
        column_counts.append(count)
    orange_pixels = sum(column_counts)
    fill_ratio = orange_pixels / (anchor_width * anchor_height)
    partial_columns = sum(0 < count < anchor_height for count in column_counts)
    column_texture_ratio = partial_columns / anchor_width
    gap_lengths: list[int] = []
    current_gap = 0
    for count in column_counts:
        if count == 0:
            current_gap += 1
        elif current_gap:
            gap_lengths.append(current_gap)
            current_gap = 0
    if current_gap:
        gap_lengths.append(current_gap)
    internal_gap_runs = len(gap_lengths)
    widest_gap = max(gap_lengths, default=0)
    row_counts: list[int] = []
    for y in range(anchor_y0, anchor_y1 + 1):
        count = 0
        for x in range(anchor_x0, anchor_x1 + 1):
            offset = (y * REGISTRATION_WIDTH + x) * 3
            count += int(_orange(frame[offset], frame[offset + 1], frame[offset + 2]))
        row_counts.append(count)
    row_coverage_ratio = sum(count > 0 for count in row_counts) / anchor_height
    blank_row_runs: list[int] = []
    current_blank_rows = 0
    for count in row_counts:
        if count == 0:
            current_blank_rows += 1
        elif current_blank_rows:
            blank_row_runs.append(current_blank_rows)
            current_blank_rows = 0
    if current_blank_rows:
        blank_row_runs.append(current_blank_rows)
    maximum_blank_row_run = max(blank_row_runs, default=0)
    readable = (
        MIN_LANDMARK_FILL_RATIO <= fill_ratio <= MAX_LANDMARK_FILL_RATIO
        and column_texture_ratio >= MIN_LANDMARK_COLUMN_TEXTURE_RATIO
        and internal_gap_runs >= MIN_LANDMARK_INTERNAL_GAP_RUNS
        and widest_gap >= MIN_LANDMARK_WIDEST_GAP_PIXELS
        and row_coverage_ratio >= MIN_LANDMARK_ROW_COVERAGE_RATIO
        and maximum_blank_row_run <= MAX_LANDMARK_BLANK_ROW_RUN_PIXELS
    )
    if not readable:
        raise CameraRegistrationError(
            "screen_landmark_unreadable",
            "camera registration landmark lacks readable SCAN glyph structure",
            measured={
                "orange_pixels": orange_pixels,
                "fill_ratio": round(fill_ratio, 4),
                "column_texture_ratio": round(column_texture_ratio, 4),
                "internal_gap_runs": internal_gap_runs,
                "widest_gap_pixels": widest_gap,
                "row_coverage_ratio": round(row_coverage_ratio, 4),
                "maximum_blank_row_run_pixels": maximum_blank_row_run,
            },
            thresholds={
                "fill_ratio": [MIN_LANDMARK_FILL_RATIO, MAX_LANDMARK_FILL_RATIO],
                "minimum_column_texture_ratio": MIN_LANDMARK_COLUMN_TEXTURE_RATIO,
                "minimum_internal_gap_runs": MIN_LANDMARK_INTERNAL_GAP_RUNS,
                "minimum_widest_gap_pixels": MIN_LANDMARK_WIDEST_GAP_PIXELS,
                "minimum_row_coverage_ratio": MIN_LANDMARK_ROW_COVERAGE_RATIO,
                "maximum_blank_row_run_pixels": MAX_LANDMARK_BLANK_ROW_RUN_PIXELS,
            },
        )

    anchor_x = (anchor_x0 + anchor_x1) / 2.0
    anchor_y = (anchor_y0 + anchor_y1) / 2.0
    scale_x = anchor_width / REFERENCE_LANDMARK_WIDTH
    scale_y = anchor_height / REFERENCE_LANDMARK_HEIGHT
    scale = math.sqrt(scale_x * scale_y)
    aspect_scale_ratio = scale_x / scale_y
    if not (
        MIN_DISPLAY_SCALE <= scale <= MAX_DISPLAY_SCALE
        and 0.75 <= aspect_scale_ratio <= 1.35
    ):
        raise CameraRegistrationError(
            "screen_scale_outside_bounds",
            "camera registration display scale or aspect is outside dynamic bounds",
            measured={
                "scale": round(scale, 4),
                "scale_x": round(scale_x, 4),
                "scale_y": round(scale_y, 4),
                "aspect_scale_ratio": round(aspect_scale_ratio, 4),
            },
            thresholds={
                "scale": [MIN_DISPLAY_SCALE, MAX_DISPLAY_SCALE],
                "aspect_scale_ratio": [0.75, 1.35],
            },
        )

    crop_width = DISPLAY_CROP_WIDTH * scale
    crop_height = DISPLAY_CROP_HEIGHT * scale
    crop_x = (
        anchor_x / REGISTRATION_WIDTH
        - crop_width * REFERENCE_ANCHOR_X / FRAME_WIDTH
    )
    crop_y = (
        anchor_y / REGISTRATION_HEIGHT
        - crop_height * REFERENCE_ANCHOR_Y / FRAME_HEIGHT
    )
    if (
        crop_x < 0.0
        or crop_y < 0.0
        or crop_x + crop_width > 1.0
        or crop_y + crop_height > 1.0
    ):
        raise CameraRegistrationError(
            "screen_crop_outside_frame",
            "dynamic display crop is not fully contained by the camera frame",
            measured={
                "crop_fractions": [
                    round(crop_x, 6),
                    round(crop_y, 6),
                    round(crop_width, 6),
                    round(crop_height, 6),
                ],
            },
            thresholds={
                "minimum": 0.0,
                "maximum": 1.0,
                "full_crop_required": True,
            },
        )

    offset_x = (crop_x - DISPLAY_CROP_X) / DISPLAY_CROP_WIDTH * FRAME_WIDTH
    offset_y = (crop_y - DISPLAY_CROP_Y) / DISPLAY_CROP_HEIGHT * FRAME_HEIGHT
    return offset_x, offset_y, {
        "result": "PASS",
        "normalized_still_size": f"{REGISTRATION_WIDTH}x{REGISTRATION_HEIGHT}",
        "landmark_bounds": [anchor_x0, anchor_y0, anchor_x1, anchor_y1],
        "landmark_readability": {
            "orange_pixels": orange_pixels,
            "fill_ratio": round(fill_ratio, 4),
            "column_texture_ratio": round(column_texture_ratio, 4),
            "internal_gap_runs": internal_gap_runs,
            "widest_gap_pixels": widest_gap,
            "row_coverage_ratio": round(row_coverage_ratio, 4),
            "maximum_blank_row_run_pixels": maximum_blank_row_run,
        },
        "reference_anchor": [REFERENCE_ANCHOR_X, REFERENCE_ANCHOR_Y],
        "offset_pixels": [round(offset_x, 3), round(offset_y, 3)],
        "transform": {
            "kind": "dynamic_similarity",
            "scale": round(scale, 6),
            "scale_xy": [round(scale_x, 6), round(scale_y, 6)],
            "crop_fractions": [
                round(crop_x, 8),
                round(crop_y, 8),
                round(crop_width, 8),
                round(crop_height, 8),
            ],
        },
        "bounds": {
            "scale": [MIN_DISPLAY_SCALE, MAX_DISPLAY_SCALE],
            "aspect_scale_ratio": [0.75, 1.35],
            "full_crop_required": True,
        },
    }


def calibrate_display_crop(
    path: Path,
    ffmpeg: str | None = None,
) -> tuple[float, float, dict[str, Any]]:
    offset_x, offset_y, registration = detect_display_crop_registration(
        _decode_registration_frame(path, ffmpeg)
    )
    registration["source_still"] = path.name
    return offset_x, offset_y, registration
