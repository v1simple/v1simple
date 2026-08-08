#!/usr/bin/env python3
"""Mechanically compare bench camera frames with the recorded display state.

The camera rig is deliberately fixed and calibrated.  This grader samples the
known display viewport through ffmpeg, reduces each frame to a few UI features,
and compares replay frames with the independently recorded encounter CSV.  It
does not add performance metrics or attempt general-purpose computer vision.
"""

from __future__ import annotations

import argparse
import collections
import csv
import hashlib
import json
import math
import shutil
import subprocess
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from bench_identity import current_grader_fingerprint
from camera_artifacts import (
    CAPTURE_MANIFEST_NAME,
    GRADE_SCHEMA_VERSION,
    camera_result_view,
    capture_input_hashes,
    load_capture_manifest,
    publish_grade,
    resolve_manifest_artifact,
)
from camera_contract import (
    MAX_DISPLAY_CROP_OFFSET_X,
    MAX_DISPLAY_CROP_OFFSET_Y,
    MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
    MIN_ALERT_COMPARISONS,
    MIN_ALERT_MATCH_RATIO,
    MIN_DIRECTION_COMPARISONS,
    MIN_DIRECTION_MATCH_RATIO,
    MIN_FREQUENCY_COMPARISONS,
    MIN_FREQUENCY_MATCH_RATIO,
    MIN_TIMELINE_MATCH_RATIO,
    REGISTRATION_SOURCE_FIELDS,
    camera_evidence_contract,
)


FRAME_WIDTH = 480
FRAME_HEIGHT = 200
FRAME_RATE = 3
FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 3
REFERENCE_PATH = Path(__file__).with_name("camera_reference.json")

# The calibrated camera view.  Coordinates are fractions of the full camera
# frame, then normalized by ffmpeg to FRAME_WIDTH x FRAME_HEIGHT.
DISPLAY_CROP_WIDTH = 0.52
DISPLAY_CROP_HEIGHT = 0.38
DISPLAY_CROP_X = 0.18
DISPLAY_CROP_Y = 0.25

# The session-start and final dim stills show the centered SCAN label
# independently of the replay log. Register that stable display landmark to the
# verified reference layout before collection or grading. The hard limits reject
# unreadable framing and camera movement too large for translation alone.
REGISTRATION_WIDTH = 960
REGISTRATION_HEIGHT = 540
REFERENCE_ANCHOR_X = 224.0
REFERENCE_ANCHOR_Y = 83.0
MIN_LANDMARK_FILL_RATIO = 0.10
MAX_LANDMARK_FILL_RATIO = 0.72
MIN_LANDMARK_COLUMN_TEXTURE_RATIO = 0.30
MIN_LANDMARK_INTERNAL_GAP_RUNS = 2
MIN_LANDMARK_WIDEST_GAP_PIXELS = 2
MIN_LANDMARK_ROW_COVERAGE_RATIO = 0.85
MAX_LANDMARK_BLANK_ROW_RUN_PIXELS = 2

ALIGNMENT_END_SECONDS = 251.0
MIN_ALIGNMENT_COVERAGE_RATIO = 0.95
MIN_ALIGNMENT_UNIQUENESS_MARGIN = 0.005
MIN_DISPLAY_VISIBLE_RATIO = 0.95
MAX_AMBIGUOUS_ENCOUNTER_RATIO = 0.10
CONSENSUS_MIN_RATIO = 0.60


@dataclass(frozen=True)
class FrameObservation:
    time_s: float
    visible_pixels: int
    frequency_pixels: int
    frequency_mhz: int | None
    frequency_confidence: float
    frequency_signature: tuple[int, ...]
    direction: str
    direction_confidence: float

    @property
    def alert_visible(self) -> bool:
        return self.frequency_pixels >= 80


@dataclass(frozen=True)
class EncounterObservation:
    time_s: float
    encounter_id: int
    frequency_mhz: int
    direction: str
    event: str


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


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _orange(red: int, green: int, blue: int) -> bool:
    return red >= 70 and red >= green * 1.35 and green >= blue * 1.15


def _visible(red: int, green: int, blue: int) -> bool:
    return max(red, green, blue) >= 35 and max(red, green, blue) - min(red, green, blue) >= 12


def _count_pixels(
    frame: bytes,
    bounds: tuple[int, int, int, int],
    predicate: Any,
) -> int:
    x0, y0, x1, y1 = bounds
    count = 0
    for y in range(y0, y1):
        row = y * FRAME_WIDTH * 3
        for x in range(x0, x1):
            offset = row + x * 3
            if predicate(frame[offset], frame[offset + 1], frame[offset + 2]):
                count += 1
    return count


def _orange_centroid_y(frame: bytes, bounds: tuple[int, int, int, int]) -> tuple[int, float]:
    x0, y0, x1, y1 = bounds
    count = 0
    y_total = 0
    for y in range(y0, y1):
        row = y * FRAME_WIDTH * 3
        for x in range(x0, x1):
            offset = row + x * 3
            if _orange(frame[offset], frame[offset + 1], frame[offset + 2]):
                count += 1
                y_total += y
    return count, (y_total / count if count else 0.0)


def frequency_signature(frame: bytes) -> tuple[int, ...]:
    values: list[int] = []
    for bin_y in range(5):
        for bin_x in range(15):
            x0 = 135 + bin_x * 12
            y0 = 55 + bin_y * 12
            values.append(_count_pixels(frame, (x0, y0, x0 + 12, y0 + 12), _orange))
    return tuple(values)


def load_camera_reference(path: Path = REFERENCE_PATH) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError("camera reference is malformed")
    return payload


CAMERA_REFERENCE = load_camera_reference()


def load_frequency_references(payload: dict[str, Any] = CAMERA_REFERENCE) -> dict[int, tuple[int, ...]]:
    raw = payload.get("frequency_references")
    if not isinstance(raw, dict) or not raw:
        raise RuntimeError("camera reference has no frequency references")
    references: dict[int, tuple[int, ...]] = {}
    for key, reference in raw.items():
        if not isinstance(reference, dict) or not isinstance(reference.get("signature"), list):
            raise RuntimeError("camera reference frequency entry is malformed")
        references[int(key)] = tuple(int(value) for value in reference["signature"])
    if any(len(values) != 75 for values in references.values()):
        raise RuntimeError("camera reference frequency signatures are malformed")
    return references


FREQUENCY_REFERENCES = load_frequency_references()


def _decode_reference_frame(path: Path, ffmpeg: str | None = None) -> bytes:
    executable = ffmpeg or shutil.which("ffmpeg")
    if not executable:
        raise RuntimeError("ffmpeg is required to validate camera reference images")
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
        raise RuntimeError(f"camera reference image decode failed: {detail or f'exit {process.returncode}'}")
    if len(process.stdout) != FRAME_BYTES:
        raise RuntimeError(
            f"camera reference image has {len(process.stdout)} decoded bytes; expected {FRAME_BYTES}"
        )
    return process.stdout


def validate_camera_reference(
    payload: dict[str, Any] = CAMERA_REFERENCE,
    path: Path = REFERENCE_PATH,
    ffmpeg: str | None = None,
) -> None:
    verified_by = payload.get("verified_by")
    verified_utc = payload.get("verified_utc")
    if not isinstance(verified_by, str) or not verified_by.strip():
        raise RuntimeError("camera reference has no visual verifier")
    if not isinstance(verified_utc, str):
        raise RuntimeError("camera reference has no visual verification time")
    try:
        verification_time = datetime.fromisoformat(verified_utc.replace("Z", "+00:00"))
    except ValueError as exc:
        raise RuntimeError("camera reference visual verification time is malformed") from exc
    if verification_time.tzinfo is None:
        raise RuntimeError("camera reference visual verification time has no timezone")

    raw = payload.get("frequency_references")
    if not isinstance(raw, dict) or not raw:
        raise RuntimeError("camera reference has no frequency references")
    references = load_frequency_references(payload)
    for frequency, signature in references.items():
        reference = raw.get(str(frequency))
        if not isinstance(reference, dict):
            raise RuntimeError(f"camera reference {frequency} entry is malformed")
        expected_text = f"{frequency // 1000}.{frequency % 1000:03d}"
        if reference.get("display_text") != expected_text:
            raise RuntimeError(f"camera reference {frequency} display text is malformed")
        image_name = reference.get("image")
        image_sha256 = reference.get("image_sha256")
        if not isinstance(image_name, str) or Path(image_name).name != image_name:
            raise RuntimeError(f"camera reference {frequency} image path is malformed")
        if not isinstance(image_sha256, str) or len(image_sha256) != 64:
            raise RuntimeError(f"camera reference {frequency} image hash is malformed")
        image_path = path.parent / image_name
        try:
            image_bytes = image_path.read_bytes()
        except OSError as exc:
            raise RuntimeError(f"camera reference {frequency} image is missing") from exc
        if hashlib.sha256(image_bytes).hexdigest() != image_sha256:
            raise RuntimeError(f"camera reference {frequency} image hash does not match")
        if frequency_signature(_decode_reference_frame(image_path, ffmpeg)) != signature:
            raise RuntimeError(f"camera reference {frequency} signature does not match its image")


def validate_camera_profile(camera_result: dict[str, Any]) -> None:
    if camera_result.get("camera_name") != CAMERA_REFERENCE.get("camera_name"):
        raise RuntimeError("camera does not match the calibrated visual reference")
    actual = camera_result.get("profile") if isinstance(camera_result.get("profile"), dict) else {}
    expected = CAMERA_REFERENCE.get("profile")
    if not isinstance(expected, dict) or any(actual.get(key) != value for key, value in expected.items()):
        raise RuntimeError("camera profile does not match the calibrated visual reference")


def _signature_distance(left: tuple[int, ...], right: tuple[int, ...]) -> float:
    left_total = sum(left)
    right_total = sum(right)
    if left_total <= 0 or right_total <= 0 or len(left) != len(right):
        return 1.0
    return 0.5 * sum(
        abs(left_value / left_total - right_value / right_total)
        for left_value, right_value in zip(left, right)
    )


def identify_frequency(signature: tuple[int, ...]) -> tuple[int | None, float]:
    ranked = sorted(
        (_signature_distance(signature, reference), frequency)
        for frequency, reference in FREQUENCY_REFERENCES.items()
    )
    if len(ranked) < 2:
        return None, 0.0
    best_distance, best_frequency = ranked[0]
    confidence = ranked[1][0] - best_distance
    if best_distance > 0.24 or confidence < 0.025:
        return None, max(0.0, confidence)
    return best_frequency, confidence


def observe_frame(frame: bytes, time_s: float) -> FrameObservation:
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"camera frame has {len(frame)} bytes; expected {FRAME_BYTES}")

    # Main orange frequency digits.  The resting display has only dim dashes in
    # this region, making this a stable alert/no-alert discriminator.
    frequency_pixels = _count_pixels(frame, (135, 50, 315, 118), _orange)
    signature = frequency_signature(frame) if frequency_pixels >= 80 else ()
    frequency_mhz, frequency_confidence = identify_frequency(signature) if signature else (None, 0.0)
    visible_pixels = _count_pixels(frame, (15, 20, 465, 170), _visible)

    # The three arrow glyphs are vertically separated.  The centroid is more
    # stable than rectangular band counts because the large front triangle
    # naturally extends toward the side-arrow band.
    arrow_count, arrow_y = _orange_centroid_y(frame, (350, 22, 455, 158))
    direction = "UNKNOWN"
    confidence = 0.0
    if frequency_pixels >= 80 and arrow_count >= 35:
        if arrow_y < 78.0:
            direction = "FRONT"
            confidence = min(1.0, (78.0 - arrow_y) / 18.0)
        elif arrow_y < 108.0:
            direction = "SIDE"
            confidence = min(1.0, min(arrow_y - 78.0, 108.0 - arrow_y) / 15.0)
        else:
            direction = "REAR"
            confidence = min(1.0, (arrow_y - 108.0) / 18.0)
    return FrameObservation(
        time_s,
        visible_pixels,
        frequency_pixels,
        frequency_mhz,
        round(frequency_confidence, 4),
        signature,
        direction,
        round(confidence, 4),
    )


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
        error = detail or f"exit {process.returncode}"
        raise RuntimeError(f"camera registration still decode failed: {error}")
    expected_bytes = REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3
    if len(process.stdout) != expected_bytes:
        raise RuntimeError(
            f"camera registration still has {len(process.stdout)} decoded bytes; expected {expected_bytes}"
        )
    return process.stdout


def detect_display_crop_registration(frame: bytes) -> tuple[float, float, dict[str, Any]]:
    """Locate the centered SCAN label and return bounded output-pixel offsets."""
    expected_bytes = REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3
    if len(frame) != expected_bytes:
        raise ValueError(f"camera registration frame has {len(frame)} bytes; expected {expected_bytes}")

    roi_x0 = int(REGISTRATION_WIDTH * 0.25)
    roi_x1 = int(REGISTRATION_WIDTH * 0.72)
    roi_y0 = int(REGISTRATION_HEIGHT * 0.25)
    roi_y1 = int(REGISTRATION_HEIGHT * 0.62)
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
        if not groups or x - groups[-1][-1][0] > 12:
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
            measured={"landmark_width_pixels": anchor_x1 - anchor_x0 + 1, "landmark_height_pixels": 0},
            thresholds={"width_pixels": [80, 180], "height_pixels": [30, 80]},
        )
    anchor_y0 = min(anchor_ys)
    anchor_y1 = max(anchor_ys)
    anchor_width = anchor_x1 - anchor_x0 + 1
    anchor_height = anchor_y1 - anchor_y0 + 1
    if not (80 <= anchor_width <= 180 and 30 <= anchor_height <= 80):
        raise CameraRegistrationError(
            "screen_landmark_geometry_invalid",
            "camera registration display landmark has unexpected geometry "
            f"({anchor_width}x{anchor_height})",
            measured={"landmark_width_pixels": anchor_width, "landmark_height_pixels": anchor_height},
            thresholds={"width_pixels": [80, 180], "height_pixels": [30, 80]},
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
    expected_x = REGISTRATION_WIDTH * (
        DISPLAY_CROP_X + DISPLAY_CROP_WIDTH * REFERENCE_ANCHOR_X / FRAME_WIDTH
    )
    expected_y = REGISTRATION_HEIGHT * (
        DISPLAY_CROP_Y + DISPLAY_CROP_HEIGHT * REFERENCE_ANCHOR_Y / FRAME_HEIGHT
    )
    offset_x = (anchor_x - expected_x) / (REGISTRATION_WIDTH * DISPLAY_CROP_WIDTH) * FRAME_WIDTH
    offset_y = (anchor_y - expected_y) / (REGISTRATION_HEIGHT * DISPLAY_CROP_HEIGHT) * FRAME_HEIGHT
    if abs(offset_x) > MAX_DISPLAY_CROP_OFFSET_X or abs(offset_y) > MAX_DISPLAY_CROP_OFFSET_Y:
        raise CameraRegistrationError(
            "screen_outside_translation_bounds",
            "camera registration exceeds bounded crop translation "
            f"({offset_x:.1f}px, {offset_y:.1f}px)",
            measured={
                "offset_x_pixels": round(offset_x, 3),
                "offset_y_pixels": round(offset_y, 3),
            },
            thresholds={
                "maximum_absolute_offset_x_pixels": MAX_DISPLAY_CROP_OFFSET_X,
                "maximum_absolute_offset_y_pixels": MAX_DISPLAY_CROP_OFFSET_Y,
            },
        )
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
        "maximum_offset_pixels": [MAX_DISPLAY_CROP_OFFSET_X, MAX_DISPLAY_CROP_OFFSET_Y],
    }


def calibrate_display_crop(path: Path, ffmpeg: str | None = None) -> tuple[float, float, dict[str, Any]]:
    offset_x, offset_y, registration = detect_display_crop_registration(
        _decode_registration_frame(path, ffmpeg)
    )
    registration["source_still"] = path.name
    return offset_x, offset_y, registration


def calibrate_display_crop_from_evidence(
    camera_dir: Path,
    camera_result: dict[str, Any],
) -> tuple[float, float, dict[str, Any]]:
    """Register from low-exposure stills, with a transition-safe fallback."""
    failures: list[str] = []
    for field in REGISTRATION_SOURCE_FIELDS:
        name = str(camera_result.get(field) or "")
        path = camera_dir / name
        if not name or not path.is_file():
            failures.append(f"{field} is missing")
            continue
        try:
            offset_x, offset_y, registration = calibrate_display_crop(path)
            registration["source_field"] = field
            return offset_x, offset_y, registration
        except RuntimeError as exc:
            failures.append(f"{field}: {exc}")
    raise RuntimeError("camera registration failed for low-exposure stills: " + "; ".join(failures))


def _display_crop_filter(offset_x: float, offset_y: float) -> str:
    crop_x = DISPLAY_CROP_X + offset_x / FRAME_WIDTH * DISPLAY_CROP_WIDTH
    crop_y = DISPLAY_CROP_Y + offset_y / FRAME_HEIGHT * DISPLAY_CROP_HEIGHT
    return (
        f"crop=iw*{DISPLAY_CROP_WIDTH}:ih*{DISPLAY_CROP_HEIGHT}:"
        f"iw*{crop_x:.8f}:ih*{crop_y:.8f}"
    )


def extract_observations(
    video_path: Path,
    ffmpeg: str | None = None,
    crop_offset_x: float = 0.0,
    crop_offset_y: float = 0.0,
) -> list[FrameObservation]:
    executable = ffmpeg or shutil.which("ffmpeg")
    if not executable:
        raise RuntimeError("ffmpeg is required to grade camera evidence")
    command = [
        executable,
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(video_path),
        "-vf",
        f"{_display_crop_filter(crop_offset_x, crop_offset_y)},"
        f"scale={FRAME_WIDTH}:{FRAME_HEIGHT}:flags=area,fps={FRAME_RATE}",
        "-an",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert process.stdout is not None
    observations: list[FrameObservation] = []
    index = 0
    while True:
        frame = process.stdout.read(FRAME_BYTES)
        if not frame:
            break
        if len(frame) != FRAME_BYTES:
            process.kill()
            raise RuntimeError("ffmpeg returned a partial camera frame")
        observations.append(observe_frame(frame, index / FRAME_RATE))
        index += 1
    stderr = process.stderr.read().decode("utf-8", errors="replace") if process.stderr else ""
    returncode = process.wait()
    if returncode != 0:
        raise RuntimeError(f"ffmpeg camera decode failed: {stderr.strip() or f'exit {returncode}'}")
    if not observations:
        raise RuntimeError("camera video contains no decodable frames")
    return observations


def load_encounters(path: Path) -> list[EncounterObservation]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        rows = [line for line in handle if not line.startswith("#")]
    parsed = list(csv.DictReader(rows))
    usable = [
        row
        for row in parsed
        if str(row.get("event") or "") in {"START", "SAMPLE", "END"}
        and int(row.get("priority") or 0) == 1
    ]
    if not usable:
        raise RuntimeError("encounter CSV contains no display observations")
    first_millis = int(usable[0]["millis"])
    # The deterministic replay begins with five seconds of idle display.  The
    # first logged encounter therefore provides an independent replay clock
    # anchor without relying on camera file timestamps.
    result: list[EncounterObservation] = []
    for row in usable:
        result.append(
            EncounterObservation(
                time_s=5.0 + (int(row["millis"]) - first_millis) / 1000.0,
                encounter_id=int(row["encounter_id"]),
                frequency_mhz=int(row["frequency_mhz"]),
                direction=str(row["direction"] or "").upper(),
                event=str(row["event"] or ""),
            )
        )
    return result


def _nearest(
    observations: list[FrameObservation],
    time_s: float,
    radius_s: float,
) -> list[FrameObservation]:
    return [item for item in observations if abs(item.time_s - time_s) <= radius_s]


def _expected_active_at(replay_time_s: float, encounters: list[EncounterObservation]) -> bool:
    intervals: dict[int, tuple[float, float]] = {}
    for encounter in encounters:
        start, end = intervals.get(encounter.encounter_id, (encounter.time_s, encounter.time_s))
        intervals[encounter.encounter_id] = (
            min(start, encounter.time_s),
            max(end, encounter.time_s),
        )
    return any(start <= replay_time_s <= end for start, end in intervals.values())


def find_replay_alignment(
    observations: list[FrameObservation],
    encounters: list[EncounterObservation],
    hint_s: float | None,
) -> dict[str, Any]:
    if hint_s is None or not math.isfinite(hint_s):
        return {
            "result": "INCONCLUSIVE",
            "selection_basis": "alert_rest_only",
            "diagnostic": {
                "code": "timing_anchor_missing",
                "message": "replay camera grade requires a finite replay timing anchor",
            },
        }
    adjustment_steps = round(MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S * FRAME_RATE)
    candidates = [hint_s + delta / FRAME_RATE for delta in range(-adjustment_steps, adjustment_steps + 1)]
    checkpoint_count = round((ALIGNMENT_END_SECONDS - 1.0) * FRAME_RATE) + 1
    checkpoints = [1.0 + index / FRAME_RATE for index in range(checkpoint_count)]
    scored: list[dict[str, Any]] = []
    for offset in candidates:
        active_matches = 0
        active_compared = 0
        rest_matches = 0
        rest_compared = 0
        compared = 0
        for replay_time in checkpoints:
            nearby = _nearest(observations, offset + replay_time, 0.2)
            if not nearby:
                continue
            expected = _expected_active_at(replay_time, encounters)
            actual = max(item.frequency_pixels for item in nearby) >= 80
            if expected:
                active_compared += 1
                active_matches += int(actual)
            else:
                rest_compared += 1
                rest_matches += int(not actual)
            compared += 1
        active_ratio = active_matches / active_compared if active_compared else 0.0
        rest_ratio = rest_matches / rest_compared if rest_compared else 0.0
        scored.append(
            {
                "offset": offset,
                "score": (active_ratio + rest_ratio) / 2.0,
                "coverage": compared / len(checkpoints),
                "active_compared": active_compared,
                "rest_compared": rest_compared,
            }
        )

    best_score = max((item["score"] for item in scored), default=-1.0)
    equivalent = [item for item in scored if math.isclose(item["score"], best_score, abs_tol=1e-12)]
    selected = min(equivalent, key=lambda item: (abs(item["offset"] - hint_s), item["offset"]))
    frame_seconds = 1.0 / FRAME_RATE
    competitors = [
        item for item in scored if abs(item["offset"] - selected["offset"]) > frame_seconds + 1e-9
    ]
    runner_up_score = max((item["score"] for item in competitors), default=best_score)
    margin = best_score - runner_up_score
    equivalent_min = min(item["offset"] for item in equivalent)
    equivalent_max = max(item["offset"] for item in equivalent)
    broad_plateau = equivalent_max - equivalent_min > frame_seconds + 1e-9
    adjustment = selected["offset"] - hint_s
    boundary = abs(adjustment) >= MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S - frame_seconds / 2.0
    inadequate_coverage = (
        selected["coverage"] < MIN_ALIGNMENT_COVERAGE_RATIO
        or selected["active_compared"] == 0
        or selected["rest_compared"] == 0
    )
    diagnostic: dict[str, Any] | None = None
    if inadequate_coverage:
        diagnostic = {
            "code": "alignment_coverage_insufficient",
            "message": "replay camera alignment lacks active/rest frame coverage",
            "measured": selected["coverage"],
            "minimum": MIN_ALIGNMENT_COVERAGE_RATIO,
        }
    elif boundary:
        diagnostic = {
            "code": "alignment_search_boundary",
            "message": "replay camera alignment landed at the timing-hint search boundary",
            "adjustment_seconds": round(adjustment, 3),
        }
    elif broad_plateau or margin < MIN_ALIGNMENT_UNIQUENESS_MARGIN:
        diagnostic = {
            "code": "alignment_ambiguous",
            "message": "replay camera alignment has no unique alert/rest solution",
            "uniqueness_margin": round(margin, 6),
            "minimum_margin": MIN_ALIGNMENT_UNIQUENESS_MARGIN,
        }

    return {
        "result": "PASS" if diagnostic is None else "INCONCLUSIVE",
        "selection_basis": "alert_rest_only",
        "start_hint_seconds": round(float(hint_s), 3),
        "selected_video_offset_seconds": round(float(selected["offset"]), 3),
        "hint_adjustment_seconds": round(adjustment, 3),
        "maximum_adjustment_seconds": MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
        "candidate_step_seconds": round(frame_seconds, 6),
        "candidate_count": len(scored),
        "best_score": round(best_score, 6),
        "runner_up_score": round(runner_up_score, 6),
        "uniqueness_margin": round(margin, 6),
        "equivalent_candidate_count": len(equivalent),
        "equivalent_offset_range_seconds": [round(equivalent_min, 3), round(equivalent_max, 3)],
        "coverage_ratio": round(float(selected["coverage"]), 6),
        "boundary_hit": boundary,
        "diagnostic": diagnostic,
    }


def find_replay_offset(
    observations: list[FrameObservation],
    encounters: list[EncounterObservation],
    hint_s: float | None,
) -> tuple[float, float]:
    """Compatibility wrapper for callers that still expect the old tuple."""
    alignment = find_replay_alignment(observations, encounters, hint_s)
    if alignment.get("result") != "PASS":
        diagnostic = alignment.get("diagnostic") or {}
        raise RuntimeError(str(diagnostic.get("message") or "replay camera alignment is inconclusive"))
    return (
        float(alignment["selected_video_offset_seconds"]),
        float(alignment["best_score"]),
    )


def grade_idle(observations: list[FrameObservation], start_s: float, duration_s: float) -> dict[str, Any]:
    selected = [
        item
        for item in observations
        if start_s + 3.0 <= item.time_s <= start_s + max(3.0, duration_s - 3.0)
    ]
    if not selected:
        return {"result": "FAIL", "checks": {}, "errors": ["no camera frames cover the bench window"]}
    visible_ratio = sum(item.visible_pixels >= 80 for item in selected) / len(selected)
    idle_ratio = sum(not item.alert_visible for item in selected) / len(selected)
    checks = {
        "display_visible": {"result": "PASS" if visible_ratio >= 0.95 else "FAIL", "ratio": visible_ratio},
        "idle_matches_log": {"result": "PASS" if idle_ratio >= 0.98 else "FAIL", "ratio": idle_ratio},
    }
    result = "PASS" if all(item["result"] == "PASS" for item in checks.values()) else "FAIL"
    return {"result": result, "checks": checks, "errors": []}


def grade_display_preview(
    observations: list[FrameObservation], start_s: float, duration_s: float
) -> dict[str, Any]:
    selected = [
        item
        for item in observations
        if start_s + 3.0 <= item.time_s <= start_s + max(3.0, duration_s - 3.0)
    ]
    if not selected:
        return {"result": "FAIL", "checks": {}, "errors": ["no camera frames cover the bench window"]}
    visible_ratio = sum(item.visible_pixels >= 80 for item in selected) / len(selected)
    active_ratio = sum(item.alert_visible for item in selected) / len(selected)
    frequencies = collections.Counter(
        item.frequency_mhz for item in selected if item.frequency_mhz is not None
    )
    directions = collections.Counter(item.direction for item in selected if item.direction != "UNKNOWN")
    states = [(item.alert_visible, item.frequency_mhz, item.direction) for item in selected]
    transitions = sum(left != right for left, right in zip(states, states[1:]))
    expected_frequencies = (24150, 34700, 35500)
    expected_directions = ("FRONT", "SIDE", "REAR")
    minimum_samples = max(3, int(len(selected) * 0.01))
    minimum_transitions = max(5, int(duration_s / 5.0))
    checks = {
        "display_visible": {"result": "PASS" if visible_ratio >= 0.95 else "FAIL", "ratio": visible_ratio},
        "preview_alert_and_rest_states": {
            "result": "PASS" if 0.70 <= active_ratio <= 0.95 else "FAIL",
            "active_ratio": round(active_ratio, 4),
        },
        "preview_frequency_sweep": {
            "result": "PASS"
            if all(frequencies[frequency] >= minimum_samples for frequency in expected_frequencies)
            else "FAIL",
            "observed": {str(key): frequencies[key] for key in expected_frequencies},
        },
        "preview_direction_sweep": {
            "result": "PASS"
            if all(directions[direction] >= minimum_samples for direction in expected_directions)
            else "FAIL",
            "observed": {key: directions[key] for key in expected_directions},
        },
        "preview_kept_advancing": {
            "result": "PASS" if transitions >= minimum_transitions else "FAIL",
            "transitions": transitions,
            "minimum": minimum_transitions,
        },
    }
    result = "PASS" if all(item["result"] == "PASS" for item in checks.values()) else "FAIL"
    return {"result": result, "checks": checks, "errors": []}


def _consensus(values: list[Any]) -> tuple[Any | None, float]:
    if not values:
        return None, 0.0
    counts = collections.Counter(values)
    ranked = counts.most_common()
    top_value, top_count = ranked[0]
    ratio = top_count / len(values)
    if ratio < CONSENSUS_MIN_RATIO or (len(ranked) > 1 and ranked[1][1] == top_count):
        return None, ratio
    return top_value, ratio


def _encounter_consensus(
    observations: list[FrameObservation],
    encounter: EncounterObservation,
    offset_s: float,
) -> dict[str, Any]:
    nearby = _nearest(observations, offset_s + encounter.time_s, 0.75)
    alert, alert_ratio = _consensus([item.alert_visible for item in nearby])
    # Unreadable samples are evidence about confidence, not samples that may be
    # discarded. Keeping them in the denominator prevents a sparse favorable
    # reading from becoming the consensus for an otherwise unreadable window.
    frequencies = [item.frequency_mhz for item in nearby]
    directions = [item.direction if item.direction != "UNKNOWN" else None for item in nearby]
    frequency, frequency_ratio = _consensus(frequencies)
    direction, direction_ratio = _consensus(directions)
    return {
        "encounter": encounter,
        "nearby_count": len(nearby),
        "visible_count": sum(item.visible_pixels >= 80 for item in nearby),
        "alert": alert,
        "alert_consensus_ratio": alert_ratio,
        "frequency": frequency,
        "frequency_consensus_ratio": frequency_ratio,
        "direction": direction,
        "direction_consensus_ratio": direction_ratio,
    }


def _gate(result: bool, **measurements: Any) -> dict[str, Any]:
    return {"result": "PASS" if result else "INCONCLUSIVE", **measurements}


def _diagnostic(code: str, message: str, **measurements: Any) -> dict[str, Any]:
    return {"code": code, "message": message, **measurements}


def grade_replay(
    observations: list[FrameObservation],
    encounters: list[EncounterObservation],
    start_hint_s: float | None,
) -> dict[str, Any]:
    alignment = find_replay_alignment(observations, encounters, start_hint_s)
    diagnostics: list[dict[str, Any]] = []
    if alignment.get("result") != "PASS":
        if isinstance(alignment.get("diagnostic"), dict):
            diagnostics.append(dict(alignment["diagnostic"]))
        return {
            "result": "INCONCLUSIVE",
            "alignment": alignment,
            "confidence": {
                "result": "INCONCLUSIVE",
                "gates": {"alignment": _gate(False)},
            },
            "checks": {},
            "diagnostics": diagnostics,
            "errors": [],
        }

    offset_s = float(alignment["selected_video_offset_seconds"])
    summaries = [_encounter_consensus(observations, encounter, offset_s) for encounter in encounters]
    alert_summaries = [item for item in summaries if item["alert"] is not None]
    frequency_summaries = [item for item in summaries if item["frequency"] is not None]
    direction_summaries = [item for item in summaries if item["direction"] is not None]
    visible_frames = sum(item.visible_pixels >= 80 for item in observations)
    visible_ratio = visible_frames / len(observations) if observations else 0.0
    ambiguous = sum(
        item["alert"] is None
        or item["frequency"] is None
        or item["direction"] is None
        for item in summaries
    )
    ambiguous_ratio = ambiguous / len(summaries) if summaries else 1.0
    gates = {
        "alignment": _gate(True),
        "display_readable": _gate(
            visible_ratio >= MIN_DISPLAY_VISIBLE_RATIO,
            ratio=round(visible_ratio, 4),
            minimum=MIN_DISPLAY_VISIBLE_RATIO,
        ),
        "alert_observations": _gate(
            len(alert_summaries) >= MIN_ALERT_COMPARISONS,
            compared=len(alert_summaries),
            minimum=MIN_ALERT_COMPARISONS,
        ),
        "frequency_observations": _gate(
            len(frequency_summaries) >= MIN_FREQUENCY_COMPARISONS,
            compared=len(frequency_summaries),
            minimum=MIN_FREQUENCY_COMPARISONS,
        ),
        "direction_observations": _gate(
            len(direction_summaries) >= MIN_DIRECTION_COMPARISONS,
            compared=len(direction_summaries),
            minimum=MIN_DIRECTION_COMPARISONS,
        ),
        "encounter_consensus": _gate(
            ambiguous_ratio <= MAX_AMBIGUOUS_ENCOUNTER_RATIO,
            ambiguous=ambiguous,
            total=len(summaries),
            ratio=round(ambiguous_ratio, 4),
            maximum=MAX_AMBIGUOUS_ENCOUNTER_RATIO,
        ),
    }
    if gates["display_readable"]["result"] != "PASS":
        diagnostics.append(
            _diagnostic(
                "display_unreadable",
                "too few replay camera frames contain a readable display",
                **gates["display_readable"],
            )
        )
    for gate_name, code, message in (
        ("alert_observations", "alert_observations_insufficient", "too few alert observations are readable"),
        (
            "frequency_observations",
            "frequency_observations_insufficient",
            "too few frequency observations are readable",
        ),
        (
            "direction_observations",
            "direction_observations_insufficient",
            "too few direction observations are readable",
        ),
        (
            "encounter_consensus",
            "encounter_classification_ambiguous",
            "too many encounter windows have ambiguous visual classifications",
        ),
    ):
        if gates[gate_name]["result"] != "PASS":
            diagnostics.append(_diagnostic(code, message, **gates[gate_name]))
    if diagnostics:
        return {
            "result": "INCONCLUSIVE",
            "alignment": alignment,
            "confidence": {"result": "INCONCLUSIVE", "gates": gates},
            "checks": {},
            "diagnostics": diagnostics,
            "errors": [],
        }

    alert_matches = sum(bool(item["alert"]) for item in alert_summaries)
    alert_compared = len(alert_summaries)
    frequency_matches = sum(
        item["frequency"] == item["encounter"].frequency_mhz for item in frequency_summaries
    )
    frequency_compared = len(frequency_summaries)
    direction_matches = sum(
        item["direction"] == item["encounter"].direction for item in direction_summaries
    )
    direction_compared = len(direction_summaries)
    alert_ratio = alert_matches / alert_compared if alert_compared else 0.0
    frequency_ratio = frequency_matches / frequency_compared if frequency_compared else 0.0
    direction_ratio = direction_matches / direction_compared if direction_compared else 0.0
    checks = {
        "alignment_near_start_hint": {
            "result": "PASS",
            "adjustment_seconds": alignment["hint_adjustment_seconds"],
            "maximum_adjustment_seconds": MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
        },
        "timeline_matches_log": {
            "result": "PASS" if alignment["best_score"] >= MIN_TIMELINE_MATCH_RATIO else "FAIL",
            "ratio": alignment["best_score"],
        },
        "logged_alerts_visible": {
            "result": "PASS"
            if alert_compared >= MIN_ALERT_COMPARISONS and alert_ratio >= MIN_ALERT_MATCH_RATIO
            else "FAIL",
            "matched": alert_matches,
            "compared": alert_compared,
            "ratio": round(alert_ratio, 4),
        },
        "logged_frequencies_visible": {
            "result": "PASS"
            if frequency_compared >= MIN_FREQUENCY_COMPARISONS
            and frequency_ratio >= MIN_FREQUENCY_MATCH_RATIO
            else "FAIL",
            "matched": frequency_matches,
            "compared": frequency_compared,
            "ratio": round(frequency_ratio, 4),
        },
        "logged_directions_visible": {
            "result": "PASS"
            if direction_compared >= MIN_DIRECTION_COMPARISONS
            and direction_ratio >= MIN_DIRECTION_MATCH_RATIO
            else "FAIL",
            "matched": direction_matches,
            "compared": direction_compared,
            "ratio": round(direction_ratio, 4),
        },
    }
    result = "PASS" if all(item["result"] == "PASS" for item in checks.values()) else "FAIL"
    return {
        "result": result,
        "alignment": alignment,
        "confidence": {"result": "PASS", "gates": gates},
        "checks": checks,
        "diagnostics": [],
        "errors": [],
    }


def grade_camera(
    *,
    suite: str,
    camera_dir: Path,
    camera_result: dict[str, Any],
    capture_manifest: dict[str, Any],
    grader_fingerprint: str,
    emulator_result: dict[str, Any],
    encounter_csv_path: Path | None,
    timeline_start_video_s: float | None,
) -> dict[str, Any]:
    capture_id = str(capture_manifest.get("capture_id") or "")
    payload: dict[str, Any] = {
        "schema_version": GRADE_SCHEMA_VERSION,
        "kind": "bench_camera_grade",
        "capture_id": capture_id,
        "grader_fingerprint": grader_fingerprint,
        "grade_id": hashlib.sha256(f"{capture_id}:{grader_fingerprint}".encode("ascii")).hexdigest(),
        "input_hashes": capture_input_hashes(capture_manifest),
        "timestamp_utc": utc_now(),
        "suite": suite,
        "video": str(camera_result.get("video") or ""),
        "result": "INCONCLUSIVE",
        "evidence_contract": camera_evidence_contract(suite),
        "preflight": camera_result.get("preflight") or capture_manifest.get("preflight") or {},
        "timing_anchor": camera_result.get("timing_anchor") or {},
        "confidence": {"result": "INCONCLUSIVE", "gates": {}},
        "checks": {},
        "diagnostics": [],
        "errors": [],
    }
    try:
        if camera_result.get("result") != "CAPTURED":
            raise RuntimeError("camera capture did not complete")
        if suite == "replay" and (
            timeline_start_video_s is None or not math.isfinite(timeline_start_video_s)
        ):
            payload["diagnostics"] = [
                {
                    "code": "timing_anchor_missing",
                    "message": "replay camera grade requires a finite replay timing anchor",
                }
            ]
            return payload
        validate_camera_reference()
        payload["camera_reference"] = {
            "source_git_sha": CAMERA_REFERENCE.get("source_git_sha"),
            "verified_by": CAMERA_REFERENCE.get("verified_by"),
            "verified_utc": CAMERA_REFERENCE.get("verified_utc"),
        }
        validate_camera_profile(camera_result)
        video_name = str(camera_result.get("video") or "")
        video_path = camera_dir / video_name
        if not video_name or not video_path.is_file():
            raise RuntimeError("camera video is missing")
        crop_offset_x, crop_offset_y, registration = calibrate_display_crop_from_evidence(
            camera_dir,
            camera_result,
        )
        payload["crop_registration"] = registration
        payload["transform"] = {
            "kind": "translation",
            "offset_pixels": registration.get("offset_pixels") or [crop_offset_x, crop_offset_y],
        }
        observations = extract_observations(
            video_path,
            crop_offset_x=crop_offset_x,
            crop_offset_y=crop_offset_y,
        )
        payload["sample_rate_hz"] = FRAME_RATE
        payload["sample_count"] = len(observations)
        payload["video"] = video_name
        if suite == "replay":
            if not emulator_result.get("completed"):
                raise RuntimeError("replay emulator did not complete")
            if encounter_csv_path is None or not encounter_csv_path.is_file():
                raise RuntimeError("replay encounter CSV is missing")
            encounters = load_encounters(encounter_csv_path)
            payload.update(grade_replay(observations, encounters, timeline_start_video_s))
            payload["encounter_csv"] = encounter_csv_path.name
            payload["encounter_observations"] = len(encounters)
        elif suite == "display":
            duration_s = float(camera_result.get("expected_duration_seconds") or 0.0)
            payload.update(
                grade_display_preview(observations, timeline_start_video_s or 0.0, duration_s)
            )
        else:
            duration_s = float(camera_result.get("expected_duration_seconds") or 0.0)
            payload.update(grade_idle(observations, timeline_start_video_s or 0.0, duration_s))
    except Exception as exc:  # noqa: BLE001 - the grade artifact is the failure contract
        payload["errors"] = [str(exc)]
        payload["diagnostics"] = [
            {
                "code": "camera_processing_error",
                "message": str(exc),
            }
        ]
        payload["confidence"] = {"result": "INCONCLUSIVE", "gates": {}}
        payload["result"] = "INCONCLUSIVE"
    return payload


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["core", "display", "replay"], required=True)
    parser.add_argument("--camera-dir", required=True)
    parser.add_argument("--encounter-csv", default="")
    parser.add_argument(
        "--timeline-start-video-seconds",
        "--emulator-start-video-seconds",
        dest="timeline_start_video_seconds",
        type=float,
        default=None,
        help="Replay/display timeline start on the camera video's clock",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    camera_dir = Path(args.camera_dir).resolve()
    capture_manifest = load_capture_manifest(camera_dir / CAPTURE_MANIFEST_NAME)
    camera_result = camera_result_view(capture_manifest)
    grader_fingerprint = current_grader_fingerprint()
    encounter_path = (
        Path(args.encounter_csv).resolve()
        if args.encounter_csv
        else resolve_manifest_artifact(camera_dir, capture_manifest, "encounter_csv")
    )
    timing_anchor = capture_manifest.get("timing_anchor")
    timeline_start = (
        timing_anchor.get("video_seconds") if isinstance(timing_anchor, dict) else None
    )
    grade = grade_camera(
        suite=args.suite,
        camera_dir=camera_dir,
        camera_result=camera_result,
        capture_manifest=capture_manifest,
        grader_fingerprint=grader_fingerprint,
        emulator_result={"completed": True},
        encounter_csv_path=encounter_path,
        timeline_start_video_s=(
            args.timeline_start_video_seconds
            if args.timeline_start_video_seconds is not None
            else timeline_start
        ),
    )
    publish_grade(camera_dir, capture_manifest, grader_fingerprint, grade)
    print(json.dumps(grade, indent=2))
    return 0 if grade["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
