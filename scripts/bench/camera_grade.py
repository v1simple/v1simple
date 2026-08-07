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
import json
import math
import shutil
import subprocess
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


FRAME_WIDTH = 480
FRAME_HEIGHT = 200
FRAME_RATE = 3
FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 3
REFERENCE_PATH = Path(__file__).with_name("camera_reference.json")

# The calibrated camera view.  Coordinates are fractions of the full camera
# frame, then normalized by ffmpeg to FRAME_WIDTH x FRAME_HEIGHT.
DISPLAY_CROP = "crop=iw*0.52:ih*0.38:iw*0.18:ih*0.25"


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


def load_frequency_references() -> dict[int, tuple[int, ...]]:
    payload = CAMERA_REFERENCE
    raw = payload.get("frequency_signatures")
    if not isinstance(raw, dict) or not raw:
        raise RuntimeError("camera reference has no frequency signatures")
    references = {int(key): tuple(int(value) for value in values) for key, values in raw.items()}
    if any(len(values) != 75 for values in references.values()):
        raise RuntimeError("camera reference frequency signatures are malformed")
    return references


FREQUENCY_REFERENCES = load_frequency_references()


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


def extract_observations(video_path: Path, ffmpeg: str | None = None) -> list[FrameObservation]:
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
        f"{DISPLAY_CROP},scale={FRAME_WIDTH}:{FRAME_HEIGHT}:flags=area,fps={FRAME_RATE}",
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


def find_replay_offset(
    observations: list[FrameObservation],
    encounters: list[EncounterObservation],
    hint_s: float | None,
) -> tuple[float, float]:
    candidates: Iterable[float]
    if hint_s is not None and math.isfinite(hint_s):
        candidates = [hint_s + delta / FRAME_RATE for delta in range(-6, 7)]
    else:
        candidates = [step / FRAME_RATE for step in range(0, 61)]
    checkpoints = [float(second) for second in range(1, 252)]
    best_offset = 0.0
    best_ratio = -1.0
    best_score = -1.0
    for offset in candidates:
        matches = 0
        compared = 0
        for replay_time in checkpoints:
            nearby = _nearest(observations, offset + replay_time, 0.2)
            if not nearby:
                continue
            expected = _expected_active_at(replay_time, encounters)
            actual = max(item.frequency_pixels for item in nearby) >= 80
            matches += int(actual == expected)
            compared += 1
        ratio = matches / compared if compared else 0.0
        frequency_matches = 0
        direction_matches = 0
        encounter_compared = 0
        for encounter in encounters:
            nearby = _nearest(observations, offset + encounter.time_s, 0.4)
            if not nearby:
                continue
            encounter_compared += 1
            frequency_matches += int(
                any(item.frequency_mhz == encounter.frequency_mhz for item in nearby)
            )
            direction_matches += int(any(item.direction == encounter.direction for item in nearby))
        encounter_ratio = (
            (frequency_matches + direction_matches) / (2 * encounter_compared)
            if encounter_compared
            else 0.0
        )
        score = ratio + encounter_ratio
        if score > best_score:
            best_offset = offset
            best_ratio = ratio
            best_score = score
    return round(best_offset, 3), round(best_ratio, 4)


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


def grade_replay(
    observations: list[FrameObservation],
    encounters: list[EncounterObservation],
    start_hint_s: float | None,
) -> dict[str, Any]:
    offset_s, state_match_ratio = find_replay_offset(observations, encounters, start_hint_s)
    direction_compared = 0
    direction_matches = 0
    frequency_compared = 0
    frequency_matches = 0
    alert_compared = 0
    alert_matches = 0
    for encounter in encounters:
        nearby = _nearest(observations, offset_s + encounter.time_s, 0.75)
        if not nearby:
            continue
        alert_compared += 1
        if any(item.alert_visible for item in nearby):
            alert_matches += 1
        recognized = [item for item in nearby if item.frequency_mhz is not None]
        if recognized:
            frequency_compared += 1
            if any(item.frequency_mhz == encounter.frequency_mhz for item in recognized):
                frequency_matches += 1
        directional = [item for item in nearby if item.direction != "UNKNOWN"]
        if directional:
            direction_compared += 1
            if any(item.direction == encounter.direction for item in directional):
                direction_matches += 1
    alert_ratio = alert_matches / alert_compared if alert_compared else 0.0
    frequency_ratio = frequency_matches / frequency_compared if frequency_compared else 0.0
    direction_ratio = direction_matches / direction_compared if direction_compared else 0.0
    checks = {
        "timeline_matches_log": {
            "result": "PASS" if state_match_ratio >= 0.96 else "FAIL",
            "ratio": state_match_ratio,
        },
        "logged_alerts_visible": {
            "result": "PASS" if alert_compared >= 20 and alert_ratio >= 0.95 else "FAIL",
            "matched": alert_matches,
            "compared": alert_compared,
            "ratio": round(alert_ratio, 4),
        },
        "logged_frequencies_visible": {
            "result": "PASS" if frequency_compared >= 20 and frequency_ratio >= 0.90 else "FAIL",
            "matched": frequency_matches,
            "compared": frequency_compared,
            "ratio": round(frequency_ratio, 4),
        },
        "logged_directions_visible": {
            "result": "PASS" if direction_compared >= 12 and direction_ratio >= 0.85 else "FAIL",
            "matched": direction_matches,
            "compared": direction_compared,
            "ratio": round(direction_ratio, 4),
        },
    }
    result = "PASS" if all(item["result"] == "PASS" for item in checks.values()) else "FAIL"
    return {
        "result": result,
        "alignment": {
            "video_offset_seconds": offset_s,
            "start_hint_seconds": start_hint_s,
        },
        "checks": checks,
        "errors": [],
    }


def grade_camera(
    *,
    suite: str,
    camera_dir: Path,
    camera_result: dict[str, Any],
    emulator_result: dict[str, Any],
    encounter_csv_path: Path | None,
    emulator_start_video_s: float | None,
) -> dict[str, Any]:
    output_path = camera_dir / "camera_grade.json"
    payload: dict[str, Any] = {
        "schema_version": 1,
        "kind": "bench_camera_grade",
        "timestamp_utc": utc_now(),
        "suite": suite,
        "result": "FAIL",
        "checks": {},
        "errors": [],
    }
    try:
        if camera_result.get("result") != "CAPTURED":
            raise RuntimeError("camera capture did not complete")
        validate_camera_profile(camera_result)
        video_name = str(camera_result.get("video") or "")
        video_path = camera_dir / video_name
        if not video_name or not video_path.is_file():
            raise RuntimeError("camera video is missing")
        observations = extract_observations(video_path)
        payload["sample_rate_hz"] = FRAME_RATE
        payload["sample_count"] = len(observations)
        payload["video"] = video_name
        if suite == "replay":
            if not emulator_result.get("completed"):
                raise RuntimeError("replay emulator did not complete")
            if encounter_csv_path is None or not encounter_csv_path.is_file():
                raise RuntimeError("replay encounter CSV is missing")
            encounters = load_encounters(encounter_csv_path)
            payload.update(grade_replay(observations, encounters, emulator_start_video_s))
            payload["encounter_csv"] = encounter_csv_path.name
            payload["encounter_observations"] = len(encounters)
        elif suite == "display":
            duration_s = float(camera_result.get("expected_duration_seconds") or 0.0)
            payload.update(
                grade_display_preview(observations, emulator_start_video_s or 0.0, duration_s)
            )
        else:
            duration_s = float(camera_result.get("expected_duration_seconds") or 0.0)
            payload.update(grade_idle(observations, emulator_start_video_s or 0.0, duration_s))
    except Exception as exc:  # noqa: BLE001 - the grade artifact is the failure contract
        payload["errors"] = [str(exc)]
        payload["result"] = "FAIL"
    camera_dir.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return payload


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["core", "display", "replay"], required=True)
    parser.add_argument("--camera-dir", required=True)
    parser.add_argument("--encounter-csv", default="")
    parser.add_argument("--emulator-start-video-seconds", type=float, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    camera_dir = Path(args.camera_dir).resolve()
    result_path = camera_dir / "camera_result.json"
    camera_result = json.loads(result_path.read_text(encoding="utf-8"))
    grade = grade_camera(
        suite=args.suite,
        camera_dir=camera_dir,
        camera_result=camera_result,
        emulator_result={"completed": True},
        encounter_csv_path=Path(args.encounter_csv).resolve() if args.encounter_csv else None,
        emulator_start_video_s=args.emulator_start_video_seconds,
    )
    print(json.dumps(grade, indent=2))
    return 0 if grade["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
