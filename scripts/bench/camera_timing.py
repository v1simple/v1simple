#!/usr/bin/env python3
"""Exact camera sidecar and encoded-frame timing comparison."""

from __future__ import annotations

import json
import subprocess
from collections import defaultdict, deque
from fractions import Fraction
from pathlib import Path
from typing import Any, Iterable

from artifact_privacy import sanitize_artifact_value


VIDEO_TIMING_VERIFICATION_SCHEMA = 1
FRAME_TIMING_SIDECAR_SCHEMA = 1
FRAME_STATUSES = frozenset({"written", "capture_drop", "writer_drop", "timestamp_error"})
REQUIRED_FRAME_FIELDS = frozenset(
    {
        "schema_version",
        "phase",
        "frame_seq",
        "source_clock",
        "callback_clock",
        "source_pts_value",
        "source_pts_timescale",
        "source_duration_value",
        "source_duration_timescale",
        "callback_host_ns",
        "host_capture_ns",
        "video_pts_value",
        "video_pts_timescale",
        "video_duration_value",
        "video_duration_timescale",
        "duration_ns",
        "status",
        "drop_reason",
    }
)


def _fraction(value: Any, timescale: Any) -> Fraction | None:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not isinstance(timescale, int)
        or isinstance(timescale, bool)
        or timescale <= 0
    ):
        return None
    return Fraction(value, timescale)


def _integer(value: Any, *, minimum: int | None = None) -> bool:
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and (minimum is None or value >= minimum)
    )


def _validate_optional_timing_pair(
    record: dict[str, Any],
    *,
    value_key: str,
    timescale_key: str,
    record_index: int,
    positive: bool,
) -> None:
    value = record[value_key]
    timescale = record[timescale_key]
    if value is None and timescale is None:
        return
    timing = _fraction(value, timescale)
    if timing is None or (positive and timing <= 0):
        raise ValueError(
            f"camera sidecar record {record_index} has invalid {value_key.removesuffix('_value')}"
        )


def validate_frame_sidecar(records: list[dict[str, Any]]) -> None:
    if not records:
        raise ValueError("camera sidecar contains no frame records")

    previous_sequence: int | None = None
    phase_origins: dict[str, Fraction] = {}
    phase_last_source_pts: dict[str, Fraction] = {}
    for record_index, record in enumerate(records):
        missing = sorted(REQUIRED_FRAME_FIELDS - record.keys())
        if missing:
            raise ValueError(
                f"camera sidecar record {record_index} is missing required fields: {','.join(missing)}"
            )
        if record["schema_version"] != FRAME_TIMING_SIDECAR_SCHEMA:
            raise ValueError(f"camera sidecar record {record_index} has unsupported schema_version")
        if not isinstance(record["phase"], str) or not record["phase"]:
            raise ValueError(f"camera sidecar record {record_index} has invalid phase")
        if record["source_clock"] != "avcapture_session_synchronization_clock":
            raise ValueError(f"camera sidecar record {record_index} has invalid source_clock")
        if record["callback_clock"] != "host_monotonic":
            raise ValueError(f"camera sidecar record {record_index} has invalid callback_clock")

        sequence = record["frame_seq"]
        if not _integer(sequence, minimum=1):
            raise ValueError(f"camera sidecar record {record_index} has invalid frame_seq")
        if previous_sequence is not None and sequence != previous_sequence + 1:
            raise ValueError(f"camera sidecar record {record_index} breaks the frame_seq sequence")
        previous_sequence = sequence

        status = record["status"]
        if status not in FRAME_STATUSES:
            raise ValueError(f"camera sidecar record {record_index} has unsupported status")
        if not _integer(record["source_pts_value"]):
            raise ValueError(f"camera sidecar record {record_index} has invalid source_pts_value")
        if not _integer(record["source_pts_timescale"]):
            raise ValueError(f"camera sidecar record {record_index} has invalid source_pts_timescale")
        if not _integer(record["source_duration_value"]):
            raise ValueError(f"camera sidecar record {record_index} has invalid source_duration_value")
        if not _integer(record["source_duration_timescale"]):
            raise ValueError(f"camera sidecar record {record_index} has invalid source_duration_timescale")
        if not _integer(record["callback_host_ns"], minimum=0):
            raise ValueError(f"camera sidecar record {record_index} has invalid callback_host_ns")

        drop_reason = record["drop_reason"]
        if drop_reason is not None and (not isinstance(drop_reason, str) or not drop_reason):
            raise ValueError(f"camera sidecar record {record_index} has invalid drop_reason")
        if status in {"capture_drop", "writer_drop"} and not isinstance(drop_reason, str):
            raise ValueError(f"camera sidecar record {record_index} has no drop_reason")

        timestamp_error = record.get("timestamp_error")
        if timestamp_error is not None and (
            not isinstance(timestamp_error, str) or not timestamp_error
        ):
            raise ValueError(f"camera sidecar record {record_index} has invalid timestamp_error")
        if status == "timestamp_error" and not isinstance(timestamp_error, str):
            raise ValueError(f"camera sidecar record {record_index} has no timestamp_error")
        timestamp_errors = record.get("timestamp_errors", [])
        if not isinstance(timestamp_errors, list) or any(
            not isinstance(value, str) or not value for value in timestamp_errors
        ):
            raise ValueError(f"camera sidecar record {record_index} has invalid timestamp_errors")
        declared_errors = set(timestamp_errors)
        if isinstance(timestamp_error, str):
            declared_errors.add(timestamp_error)

        source_pts = _fraction(record["source_pts_value"], record["source_pts_timescale"])
        source_duration = _fraction(
            record["source_duration_value"], record["source_duration_timescale"]
        )
        source_pts_usable = source_pts is not None and "invalid_source_pts" not in declared_errors
        source_duration_usable = (
            source_duration is not None
            and source_duration > 0
            and "invalid_source_duration" not in declared_errors
        )
        if not source_pts_usable and "invalid_source_pts" not in declared_errors:
            raise ValueError(f"camera sidecar record {record_index} has unusable source PTS without evidence")
        if source_pts_usable:
            assert source_pts is not None
            previous_source_pts = phase_last_source_pts.get(record["phase"])
            if previous_source_pts is not None and source_pts <= previous_source_pts:
                if "non_monotonic_source_pts" not in declared_errors:
                    raise ValueError(f"camera sidecar record {record_index} has non-monotonic source PTS")
            else:
                if "non_monotonic_source_pts" in declared_errors:
                    raise ValueError(
                        f"camera sidecar record {record_index} falsely reports non-monotonic source PTS"
                    )
                phase_origins.setdefault(record["phase"], source_pts)
                phase_last_source_pts[record["phase"]] = source_pts
        if not source_duration_usable and "invalid_source_duration" not in declared_errors:
            raise ValueError(
                f"camera sidecar record {record_index} has unusable source duration without evidence"
            )

        host_capture_ns = record["host_capture_ns"]
        if host_capture_ns is not None and not _integer(host_capture_ns, minimum=0):
            raise ValueError(f"camera sidecar record {record_index} has invalid host_capture_ns")
        duration_ns = record["duration_ns"]
        if duration_ns is not None and not _integer(duration_ns):
            raise ValueError(f"camera sidecar record {record_index} has invalid duration_ns")
        if source_duration_usable and duration_ns != _nanoseconds(source_duration):
            raise ValueError(f"camera sidecar record {record_index} duration_ns differs from source duration")
        _validate_optional_timing_pair(
            record,
            value_key="video_pts_value",
            timescale_key="video_pts_timescale",
            record_index=record_index,
            positive=False,
        )
        _validate_optional_timing_pair(
            record,
            value_key="video_duration_value",
            timescale_key="video_duration_timescale",
            record_index=record_index,
            positive=True,
        )

        complete_timing_required = status in {"written", "writer_drop"} or (
            status == "capture_drop" and timestamp_error is None
        )
        if complete_timing_required:
            if not source_pts_usable:
                raise ValueError(f"camera sidecar record {record_index} has no valid source PTS")
            if not source_duration_usable:
                raise ValueError(f"camera sidecar record {record_index} has no valid source duration")
            if not _integer(host_capture_ns, minimum=0):
                raise ValueError(f"camera sidecar record {record_index} has no host_capture_ns")
            if not _integer(duration_ns, minimum=1):
                raise ValueError(f"camera sidecar record {record_index} has no duration_ns")
            if _fraction(record["video_pts_value"], record["video_pts_timescale"]) is None:
                raise ValueError(f"camera sidecar record {record_index} has no video PTS")
            duration = _fraction(
                record["video_duration_value"], record["video_duration_timescale"]
            )
            if duration is None or duration <= 0:
                raise ValueError(f"camera sidecar record {record_index} has no video duration")

        host_conversion_failed = bool(
            declared_errors
            & {"synchronization_clock_unavailable", "host_clock_conversion_failed"}
        )
        if source_pts_usable and not host_conversion_failed and not _integer(host_capture_ns, minimum=0):
            raise ValueError(
                f"camera sidecar record {record_index} lost host conversion for a valid source PTS"
            )

        video_pts = _fraction(record["video_pts_value"], record["video_pts_timescale"])
        video_duration = _fraction(
            record["video_duration_value"], record["video_duration_timescale"]
        )
        if video_pts is not None or video_duration is not None:
            origin = phase_origins.get(record["phase"])
            if not source_pts_usable or source_pts is None or origin is None or video_pts != source_pts - origin:
                raise ValueError(
                    f"camera sidecar record {record_index} video PTS is not source-relative"
                )
            if not source_duration_usable or source_duration is None or video_duration != source_duration:
                raise ValueError(
                    f"camera sidecar record {record_index} video duration differs from source duration"
                )


def load_frame_sidecar(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    with path.open(encoding="utf-8") as handle:
        for line_number, raw in enumerate(handle, start=1):
            if not raw.strip():
                continue
            try:
                record = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise ValueError(f"camera sidecar line {line_number} is invalid JSON") from exc
            if not isinstance(record, dict):
                raise ValueError(f"camera sidecar line {line_number} is not an object")
            records.append(record)
    validate_frame_sidecar(records)
    return records


def encoded_frames_from_ffprobe(payload: dict[str, Any]) -> list[dict[str, Any]]:
    streams = payload.get("streams")
    frames = payload.get("frames")
    if not isinstance(streams, list) or len(streams) != 1 or not isinstance(frames, list):
        raise ValueError("ffprobe frame response is malformed")
    stream = streams[0]
    if not isinstance(stream, dict):
        raise ValueError("ffprobe stream response is malformed")
    try:
        numerator_text, denominator_text = str(stream["time_base"]).split("/", 1)
        time_base = Fraction(int(numerator_text), int(denominator_text))
    except (KeyError, ValueError, ZeroDivisionError) as exc:
        raise ValueError("ffprobe stream time base is malformed") from exc
    if time_base <= 0:
        raise ValueError("ffprobe stream time base is not positive")

    result: list[dict[str, Any]] = []
    for index, frame in enumerate(frames):
        if not isinstance(frame, dict):
            raise ValueError(f"ffprobe frame {index} is malformed")
        try:
            pts = int(frame["pts"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError(f"ffprobe frame {index} has no integer PTS") from exc
        duration_raw = frame.get("duration", frame.get("pkt_duration"))
        duration = None
        if duration_raw not in {None, "N/A"}:
            try:
                duration = int(duration_raw)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"ffprobe frame {index} has an invalid duration") from exc
        result.append(
            {
                "encoded_index": index,
                "pts": Fraction(pts) * time_base,
                "duration": None if duration is None else Fraction(duration) * time_base,
                "pts_value": pts,
                "duration_value": duration,
                "time_base": str(stream["time_base"]),
            }
        )
    return result


def _expected_written_frames(records: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    expected: list[dict[str, Any]] = []
    for record_index, record in enumerate(records):
        if record.get("status") != "written":
            continue
        pts = _fraction(record.get("video_pts_value"), record.get("video_pts_timescale"))
        if pts is None:
            raise ValueError(f"written sidecar record {record_index} has invalid video PTS")
        duration = _fraction(
            record.get("video_duration_value"),
            record.get("video_duration_timescale"),
        )
        if duration is None:
            raise ValueError(f"written sidecar record {record_index} has invalid video duration")
        expected.append(
            {
                "record_index": record_index,
                "frame_seq": record.get("frame_seq"),
                "pts": pts,
                "duration": duration,
            }
        )
    return expected


def _nanoseconds(value: Fraction) -> int:
    scaled = value * 1_000_000_000
    quotient, remainder = divmod(abs(scaled.numerator), scaled.denominator)
    if remainder * 2 >= scaled.denominator:
        quotient += 1
    return -quotient if scaled < 0 else quotient


def compare_encoded_video_timing(
    sidecar_records: list[dict[str, Any]],
    encoded_frames: list[dict[str, Any]],
) -> dict[str, Any]:
    validate_frame_sidecar(sidecar_records)
    expected = _expected_written_frames(sidecar_records)
    source_intervals = [
        following["pts"] - current["pts"]
        for current, following in zip(expected, expected[1:])
    ]
    expected_encoded_durations = (
        [*source_intervals, expected[-1]["duration"]] if expected else []
    )
    encoded_by_pts: dict[Fraction, deque[int]] = defaultdict(deque)
    for index, frame in enumerate(encoded_frames):
        encoded_by_pts[frame["pts"]].append(index)

    matched_encoded: set[int] = set()
    missing: list[dict[str, Any]] = []
    duration_mismatches: list[dict[str, Any]] = []
    first_mismatch: dict[str, Any] | None = None

    for expected_index, (frame, expected_duration) in enumerate(
        zip(expected, expected_encoded_durations, strict=True)
    ):
        candidates = encoded_by_pts.get(frame["pts"])
        encoded_index = candidates.popleft() if candidates else None
        if encoded_index is None:
            mismatch = {
                "type": "missing_encoded_frame",
                "written_index": expected_index,
                "frame_seq": frame["frame_seq"],
                "expected_pts_value": sidecar_records[frame["record_index"]].get("video_pts_value"),
                "expected_pts_timescale": sidecar_records[frame["record_index"]].get("video_pts_timescale"),
            }
            missing.append(mismatch)
            if first_mismatch is None:
                first_mismatch = mismatch
            continue
        matched_encoded.add(encoded_index)
        encoded = encoded_frames[encoded_index]
        if encoded["duration"] != expected_duration:
            mismatch = {
                "type": "duration_mismatch",
                "written_index": expected_index,
                "encoded_index": encoded_index,
                "frame_seq": frame["frame_seq"],
                "expected_duration_ns": _nanoseconds(expected_duration),
                "encoded_duration_ns": (
                    None if encoded["duration"] is None else _nanoseconds(encoded["duration"])
                ),
            }
            duration_mismatches.append(mismatch)
            if first_mismatch is None:
                first_mismatch = mismatch

    extra = [
        {
            "type": "extra_encoded_frame",
            "encoded_index": index,
            "encoded_pts_value": frame["pts_value"],
            "encoded_time_base": frame["time_base"],
        }
        for index, frame in enumerate(encoded_frames)
        if index not in matched_encoded
    ]
    if first_mismatch is None and extra:
        first_mismatch = extra[0]

    max_timestamp_difference_ns = 0
    for expected_frame, encoded_frame in zip(expected, encoded_frames):
        difference = abs(encoded_frame["pts"] - expected_frame["pts"])
        max_timestamp_difference_ns = max(max_timestamp_difference_ns, _nanoseconds(difference))
        if difference != 0 and first_mismatch is None:
            first_mismatch = {
                "type": "timestamp_mismatch",
                "written_index": expected_frame["record_index"],
                "encoded_index": encoded_frame["encoded_index"],
                "difference_ns": _nanoseconds(difference),
            }

    counts = {
        status: sum(1 for record in sidecar_records if record.get("status") == status)
        for status in ("written", "capture_drop", "writer_drop")
    }
    timestamp_error_count = sum(
        record.get("status") == "timestamp_error" or isinstance(record.get("timestamp_error"), str)
        for record in sidecar_records
    )
    return {
        "schema_version": VIDEO_TIMING_VERIFICATION_SCHEMA,
        "kind": "camera_video_timing_verification",
        "status": (
            "verified"
            if not missing and not extra and not duration_mismatches and max_timestamp_difference_ns == 0
            else "mismatch"
        ),
        "source_frame_count": len(sidecar_records),
        "written_frame_count": counts["written"],
        "encoded_frame_count": len(encoded_frames),
        "capture_drop_count": counts["capture_drop"],
        "writer_drop_count": counts["writer_drop"],
        "timestamp_error_count": timestamp_error_count,
        "missing_encoded_frame_count": len(missing),
        "extra_encoded_frame_count": len(extra),
        "missing_encoded_frames": missing,
        "extra_encoded_frames": extra,
        "duration_mismatch_count": len(duration_mismatches),
        "duration_mismatches": duration_mismatches,
        "source_interval_difference_count": sum(
            interval != frame["duration"]
            for frame, interval in zip(expected[:-1], source_intervals, strict=True)
        ),
        "maximum_source_interval_ns": max(
            (_nanoseconds(interval) for interval in source_intervals),
            default=None,
        ),
        "first_mismatch": first_mismatch,
        "maximum_timestamp_difference_ns": max_timestamp_difference_ns,
    }


def probe_all_video_frames(ffprobe: str, video_path: Path) -> list[dict[str, Any]]:
    process = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_frames",
            "-show_streams",
            "-show_entries",
            "stream=time_base:frame=pts,duration,pkt_duration",
            "-of",
            "json",
            str(video_path),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(f"ffprobe frame inspection failed with exit {process.returncode}")
    try:
        payload = json.loads(process.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError("ffprobe frame inspection returned invalid JSON") from exc
    if not isinstance(payload, dict):
        raise RuntimeError("ffprobe frame inspection returned a non-object")
    return encoded_frames_from_ffprobe(payload)


def verify_video_file(
    ffprobe: str,
    video_path: Path,
    sidecar_path: Path,
    output_path: Path,
) -> dict[str, Any]:
    try:
        records = load_frame_sidecar(sidecar_path)
    except Exception as exc:  # evidence failure is itself machine-readable
        result = {
            "schema_version": VIDEO_TIMING_VERIFICATION_SCHEMA,
            "kind": "camera_video_timing_verification",
            "status": "sidecar_error",
            "source_frame_count": 0,
            "written_frame_count": 0,
            "encoded_frame_count": 0,
            "capture_drop_count": 0,
            "writer_drop_count": 0,
            "timestamp_error_count": 0,
            "missing_encoded_frame_count": 0,
            "extra_encoded_frame_count": 0,
            "missing_encoded_frames": [],
            "extra_encoded_frames": [],
            "duration_mismatch_count": 0,
            "duration_mismatches": [],
            "first_mismatch": {"type": "sidecar_error", "detail": str(exc)},
            "maximum_timestamp_difference_ns": None,
        }
    else:
        try:
            encoded = probe_all_video_frames(ffprobe, video_path)
            result = compare_encoded_video_timing(records, encoded)
        except Exception as exc:  # do not turn post-capture verification into a run gate
            written = [record for record in records if record.get("status") == "written"]
            result = {
                "schema_version": VIDEO_TIMING_VERIFICATION_SCHEMA,
                "kind": "camera_video_timing_verification",
                "status": "probe_error",
                "source_frame_count": len(records),
                "written_frame_count": len(written),
                "encoded_frame_count": 0,
                "capture_drop_count": sum(record.get("status") == "capture_drop" for record in records),
                "writer_drop_count": sum(record.get("status") == "writer_drop" for record in records),
                "timestamp_error_count": sum(
                    record.get("status") == "timestamp_error"
                    or isinstance(record.get("timestamp_error"), str)
                    for record in records
                ),
                "missing_encoded_frame_count": len(written),
                "extra_encoded_frame_count": 0,
                "missing_encoded_frames": [
                    {"frame_seq": record.get("frame_seq")} for record in written
                ],
                "extra_encoded_frames": [],
                "duration_mismatch_count": 0,
                "duration_mismatches": [],
                "first_mismatch": {"type": "probe_error", "detail": str(exc)},
                "maximum_timestamp_difference_ns": None,
            }
    safe_result = sanitize_artifact_value(result, run_dir=output_path.parent)
    if not isinstance(safe_result, dict):
        raise TypeError("camera timing verification sanitization returned a non-object")
    output_path.write_text(
        json.dumps(safe_result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return safe_result
