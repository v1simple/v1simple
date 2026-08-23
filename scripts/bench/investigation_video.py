#!/usr/bin/env python3
"""Extract bounded visual evidence without interpreting frame content."""

from __future__ import annotations

import json
import hashlib
import math
import os
import re
import shutil
import subprocess
import tempfile
from bisect import bisect_left
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any


MAX_CHANGE_SHEETS = 12
MAX_OVERVIEW_FRAMES = 12
MAX_INTERVALS = 6
MAX_INTERVAL_FRAMES = 36
DEFAULT_INTERVAL_HZ = 8.0
CELL_WIDTH = 240
CHANGE_ATLAS_COLUMNS = 3
FRAME_INDEX_SCHEMA_VERSION = 1
FRAME_INDEX_ALGORITHM = "ffmpeg_scene_score_native_v2"
FRAME_INDEX_TIMEOUT_SECONDS = 900
PTS_DECIMAL_PLACES = 9

FRAME_RE = re.compile(
    r"^frame:(\d+)\s+pts:([-+0-9]+)\s+pts_time:([-+0-9.eE]+)$"
)
SCORE_RE = re.compile(r"^lavfi\.scene_score=([-+0-9.eE]+)$")


class CommandError(Exception):
    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(code)
        self.code, self.detail = code, detail


def _rounded(value: float) -> float:
    return round(value, 6)


def _rounded_pts(value: float) -> float:
    return round(value, PTS_DECIMAL_PLACES)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _tool(configured: str | None, default: str) -> str | None:
    candidate = configured or shutil.which(default)
    if not candidate:
        return None
    candidate = shutil.which(candidate) if os.sep not in candidate else candidate
    return candidate if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK) else None


def _detail(text: str, video: Path, output: Path) -> str:
    text = text.replace(str(video), "<video>").replace(str(output), "<output>")
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return lines[-1][-240:] if lines else ""


def _run(
    command: list[str], code: str, video: Path, output: Path, timeout: int = 180
) -> subprocess.CompletedProcess[str]:
    try:
        process = subprocess.run(
            command, capture_output=True, text=True, check=False, timeout=timeout
        )
    except FileNotFoundError as error:
        raise CommandError("tool_missing") from error
    except subprocess.TimeoutExpired as error:
        raise CommandError("tool_timeout") from error
    except OSError as error:
        raise CommandError("tool_error") from error
    if process.returncode:
        raise CommandError(code, _detail(process.stderr, video, output))
    return process


def _record(
    errors: list[dict[str, Any]],
    stage: str,
    error: CommandError,
    request_index: int | None = None,
) -> None:
    item: dict[str, Any] = {"stage": stage, "code": error.code}
    if request_index is not None:
        item["request_index"] = request_index
    if error.detail:
        item["detail"] = error.detail
    errors.append(item)


def _probe(video: Path, output: Path, ffprobe: str) -> dict[str, Any]:
    process = _run(
        [
            ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
            (
                "format=duration:stream=width,height,duration,avg_frame_rate,"
                "codec_name,time_base"
            ),
            "-of", "json", str(video),
        ],
        "ffprobe_failed", video, output, 30,
    )
    try:
        payload = json.loads(process.stdout)
        stream = payload["streams"][0]
        duration = float(payload.get("format", {}).get("duration") or stream.get("duration"))
        width, height = int(stream["width"]), int(stream["height"])
        time_base_numerator, time_base_denominator = (
            int(value) for value in str(stream["time_base"]).split("/", 1)
        )
        if min(duration, width, height) <= 0 or not math.isfinite(duration):
            raise ValueError
        if time_base_numerator <= 0 or time_base_denominator <= 0:
            raise ValueError
    except (IndexError, KeyError, TypeError, ValueError) as error:
        raise CommandError("ffprobe_output_invalid") from error
    try:
        numerator, denominator = str(stream.get("avg_frame_rate")).split("/", 1)
        frame_rate = float(numerator) / float(denominator)
        frame_rate = _rounded(frame_rate) if math.isfinite(frame_rate) and frame_rate > 0 else None
    except (TypeError, ValueError, ZeroDivisionError):
        frame_rate = None
    return {
        "duration_seconds": _rounded(duration),
        "width": width,
        "height": height,
        "codec": str(stream.get("codec_name") or ""),
        "average_frame_rate": frame_rate,
        "source_time_base": {
            "numerator": time_base_numerator,
            "denominator": time_base_denominator,
        },
    }


def _atomic_json(path: Path, payload: Mapping[str, Any]) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, sort_keys=True, separators=(",", ":"), allow_nan=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _percentile(values: Sequence[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[position]


def _change_windows(
    rows: Sequence[Mapping[str, Any]], native_rate: float | None
) -> list[dict[str, Any]]:
    minimum_gap = max(0.05, 3.0 / native_rate) if native_rate else 0.05
    chosen: list[Mapping[str, Any]] = []
    for row in sorted(
        rows,
        key=lambda item: (-float(item["change_score"]), float(item["pts_seconds"])),
    ):
        score = float(row["change_score"])
        pts = float(row["pts_seconds"])
        if score <= 0 or any(
            abs(pts - float(prior["pts_seconds"])) < minimum_gap for prior in chosen
        ):
            continue
        chosen.append(row)
        if len(chosen) == MAX_CHANGE_SHEETS:
            break

    windows: list[dict[str, Any]] = []
    for rank, center in enumerate(chosen, 1):
        center_index = int(center["frame_index"])
        window_rows = rows[max(0, center_index - 1) : center_index + 2]
        windows.append(
            {
                "change_rank": rank,
                "frame_index": center_index,
                "pts_seconds": _rounded(float(center["pts_seconds"])),
                "change_score": round(float(center["change_score"]), 6),
                "window_start_frame": int(window_rows[0]["frame_index"]),
                "window_end_frame": int(window_rows[-1]["frame_index"]),
                "window_start_pts_seconds": _rounded(
                    float(window_rows[0]["pts_seconds"])
                ),
                "window_end_pts_seconds": _rounded(
                    float(window_rows[-1]["pts_seconds"])
                ),
            }
        )
    return windows


def _frame_index_paths(index_dir: Path, video_sha256: str) -> tuple[Path, Path]:
    return (
        index_dir / f"{video_sha256}.frames.ndjson",
        index_dir / f"{video_sha256}.summary.json",
    )


def _time_base(value: Any) -> tuple[int, int] | None:
    if not isinstance(value, Mapping):
        return None
    numerator, denominator = value.get("numerator"), value.get("denominator")
    if (
        not isinstance(numerator, int)
        or isinstance(numerator, bool)
        or not isinstance(denominator, int)
        or isinstance(denominator, bool)
        or numerator <= 0
        or denominator <= 0
    ):
        return None
    return numerator, denominator


def _pts_seconds(source_pts: int, time_base: tuple[int, int]) -> float:
    numerator, denominator = time_base
    return _rounded_pts(source_pts * numerator / denominator)


def _load_frame_rows(
    path: Path, expected_count: int, time_base: tuple[int, int]
) -> list[dict[str, Any]] | None:
    rows: list[dict[str, Any]] = []
    expected_keys = {"frame_index", "source_pts", "pts_seconds", "change_score"}
    try:
        with path.open("r", encoding="utf-8") as handle:
            for expected_index, line in enumerate(handle):
                row = json.loads(line)
                if (
                    not isinstance(row, dict)
                    or set(row) != expected_keys
                    or row.get("frame_index") != expected_index
                    or not isinstance(row.get("source_pts"), int)
                    or isinstance(row.get("source_pts"), bool)
                    or not isinstance(row.get("pts_seconds"), (int, float))
                    or isinstance(row.get("pts_seconds"), bool)
                    or not math.isfinite(float(row["pts_seconds"]))
                    or not isinstance(row.get("change_score"), (int, float))
                    or isinstance(row.get("change_score"), bool)
                    or not math.isfinite(float(row["change_score"]))
                    or float(row["pts_seconds"])
                    != _pts_seconds(int(row["source_pts"]), time_base)
                ):
                    return None
                rows.append(row)
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    if len(rows) != expected_count or not rows:
        return None
    if any(
        float(later["pts_seconds"]) < float(earlier["pts_seconds"])
        for earlier, later in zip(rows, rows[1:])
    ):
        return None
    return rows


def _frame_summary(
    rows: Sequence[Mapping[str, Any]],
    *,
    video_sha256: str,
    frame_index_filename: str,
    frame_index_sha256: str,
    source_time_base: tuple[int, int],
    probed_rate: float | None,
) -> dict[str, Any]:
    scores = [float(row["change_score"]) for row in rows]
    first_pts = float(rows[0]["pts_seconds"])
    last_pts = float(rows[-1]["pts_seconds"])
    measured_rate = (
        (len(rows) - 1) / (last_pts - first_pts)
        if len(rows) > 1 and last_pts > first_pts
        else None
    )
    native_rate = measured_rate or probed_rate
    return {
        "schema_version": FRAME_INDEX_SCHEMA_VERSION,
        "kind": "investigation_video_frame_index",
        "algorithm": FRAME_INDEX_ALGORITHM,
        "source_video_sha256": video_sha256,
        "source_time_base": {
            "numerator": source_time_base[0],
            "denominator": source_time_base[1],
        },
        "frame_index_filename": frame_index_filename,
        "frame_index_sha256": frame_index_sha256,
        "frame_count": len(rows),
        "first_frame_index": 0,
        "last_frame_index": len(rows) - 1,
        "frame_indices_contiguous": True,
        "first_pts_seconds": _rounded_pts(first_pts),
        "last_pts_seconds": _rounded_pts(last_pts),
        "native_frame_rate_fps": _rounded(native_rate) if native_rate else None,
        "probed_average_frame_rate_fps": _rounded(probed_rate)
        if probed_rate
        else None,
        "score_distribution": {
            "count": len(scores),
            "nonzero_count": sum(score > 0 for score in scores),
            "minimum": round(min(scores), 6),
            "maximum": round(max(scores), 6),
            "mean": round(sum(scores) / len(scores), 6),
            "p50": round(_percentile(scores, 0.50), 6),
            "p90": round(_percentile(scores, 0.90), 6),
            "p95": round(_percentile(scores, 0.95), 6),
            "p99": round(_percentile(scores, 0.99), 6),
        },
        "top_change_windows": _change_windows(rows, native_rate),
    }


def _load_cached_frame_index(
    index_dir: Path, video_sha256: str
) -> tuple[dict[str, Any], list[dict[str, Any]]] | None:
    frames_path, summary_path = _frame_index_paths(index_dir, video_sha256)
    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    if (
        not isinstance(summary, dict)
        or summary.get("schema_version") != FRAME_INDEX_SCHEMA_VERSION
        or summary.get("algorithm") != FRAME_INDEX_ALGORITHM
        or summary.get("source_video_sha256") != video_sha256
        or summary.get("frame_index_filename") != frames_path.name
        or not isinstance(summary.get("frame_count"), int)
        or isinstance(summary.get("frame_count"), bool)
        or summary.get("frame_count", 0) < 1
        or not isinstance(summary.get("frame_index_sha256"), str)
    ):
        return None
    time_base = _time_base(summary.get("source_time_base"))
    if time_base is None:
        return None
    probed_rate_value = summary.get("probed_average_frame_rate_fps")
    if probed_rate_value is not None and (
        not isinstance(probed_rate_value, (int, float))
        or isinstance(probed_rate_value, bool)
        or not math.isfinite(float(probed_rate_value))
        or float(probed_rate_value) <= 0
    ):
        return None
    try:
        if _sha256_file(frames_path) != summary["frame_index_sha256"]:
            return None
    except OSError:
        return None
    rows = _load_frame_rows(frames_path, int(summary["frame_count"]), time_base)
    if rows is None:
        return None
    expected = _frame_summary(
        rows,
        video_sha256=video_sha256,
        frame_index_filename=frames_path.name,
        frame_index_sha256=str(summary["frame_index_sha256"]),
        source_time_base=time_base,
        probed_rate=float(probed_rate_value) if probed_rate_value is not None else None,
    )
    return (summary, rows) if summary == expected else None


def build_frame_index(
    video_path: Path | str,
    index_dir: Path | str,
    *,
    video_sha256: str | None = None,
    probe: Mapping[str, Any] | None = None,
    ffmpeg: str | None = None,
    ffprobe: str | None = None,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Build or load a hash-keyed exhaustive native-frame scene index."""
    video = Path(video_path).resolve()
    requested_index_root = Path(index_dir)
    if requested_index_root.is_symlink():
        raise CommandError("frame_index_directory_invalid")
    index_root = requested_index_root.resolve()
    if not video.is_file():
        raise CommandError("video_missing")
    try:
        index_root.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise CommandError("frame_index_directory_invalid") from error
    if not index_root.is_dir() or index_root.is_symlink():
        raise CommandError("frame_index_directory_invalid")

    try:
        initial_stat = video.stat()
        actual_digest = _sha256_file(video)
        hashed_stat = video.stat()
    except OSError as error:
        raise CommandError("video_unreadable") from error
    if (
        initial_stat.st_size != hashed_stat.st_size
        or initial_stat.st_mtime_ns != hashed_stat.st_mtime_ns
    ):
        raise CommandError("video_changed_during_indexing")
    if video_sha256 is not None and video_sha256 != actual_digest:
        raise CommandError("video_hash_mismatch")
    digest = actual_digest
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise CommandError("video_hash_invalid")
    if cached := _load_cached_frame_index(index_root, digest):
        return cached

    ffmpeg_tool = _tool(ffmpeg, "ffmpeg")
    ffprobe_tool = _tool(ffprobe, "ffprobe")
    if not ffmpeg_tool:
        raise CommandError("ffmpeg_missing")
    if probe is None:
        if not ffprobe_tool:
            raise CommandError("ffprobe_missing")
        probe = _probe(video, index_root, ffprobe_tool)
    native_rate_value = probe.get("average_frame_rate")
    native_rate = (
        float(native_rate_value)
        if isinstance(native_rate_value, (int, float))
        and not isinstance(native_rate_value, bool)
        and math.isfinite(float(native_rate_value))
        and float(native_rate_value) > 0
        else None
    )
    source_time_base = _time_base(probe.get("source_time_base"))
    if source_time_base is None:
        raise CommandError("ffprobe_output_invalid")

    frames_path, summary_path = _frame_index_paths(index_root, digest)
    metadata_descriptor, metadata_name = tempfile.mkstemp(
        prefix=f".{digest}.metadata.", dir=index_root
    )
    os.close(metadata_descriptor)
    metadata_path = Path(metadata_name)
    frame_descriptor, frame_name = tempfile.mkstemp(
        prefix=f".{frames_path.name}.", dir=index_root
    )
    os.close(frame_descriptor)
    frame_temporary = Path(frame_name)
    command = [
        ffmpeg_tool,
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-xerror",
        "-i",
        str(video),
        "-map",
        "0:v:0",
        "-vf",
        "scale=160:-2:flags=area,select='gte(scene,0)',metadata=print:file=-",
        "-an",
        "-f",
        "null",
        "-",
    ]
    try:
        try:
            with metadata_path.open("w", encoding="utf-8") as metadata_output:
                process = subprocess.run(
                    command,
                    stdout=metadata_output,
                    stderr=subprocess.PIPE,
                    text=True,
                    check=False,
                    timeout=FRAME_INDEX_TIMEOUT_SECONDS,
                )
        except FileNotFoundError as error:
            raise CommandError("tool_missing") from error
        except subprocess.TimeoutExpired as error:
            raise CommandError("tool_timeout") from error
        except OSError as error:
            raise CommandError("tool_error") from error
        if process.returncode:
            raise CommandError(
                "ffmpeg_failed", _detail(process.stderr, video, index_root)
            )

        rows: list[dict[str, Any]] = []
        pending: tuple[int, int, float] | None = None
        with (
            metadata_path.open("r", encoding="utf-8") as metadata_input,
            frame_temporary.open("w", encoding="utf-8") as frame_output,
        ):
            for raw_line in metadata_input:
                line = raw_line.strip()
                if match := FRAME_RE.fullmatch(line):
                    if pending is not None:
                        raise CommandError("frame_index_metadata_invalid")
                    try:
                        pending = (
                            int(match.group(1)),
                            int(match.group(2)),
                            float(match.group(3)),
                        )
                    except ValueError as error:
                        raise CommandError("frame_index_metadata_invalid") from error
                    continue
                if pending is None or not (match := SCORE_RE.fullmatch(line)):
                    continue
                try:
                    score = float(match.group(1))
                except ValueError as error:
                    raise CommandError("frame_index_metadata_invalid") from error
                frame_index, source_pts, reported_pts_seconds = pending
                pending = None
                pts_seconds = _pts_seconds(source_pts, source_time_base)
                if (
                    frame_index != len(rows)
                    or not math.isfinite(reported_pts_seconds)
                    or not math.isfinite(score)
                    or (rows and pts_seconds < float(rows[-1]["pts_seconds"]))
                    or not math.isclose(
                        reported_pts_seconds,
                        pts_seconds,
                        rel_tol=0,
                        abs_tol=0.000_001,
                    )
                ):
                    raise CommandError("frame_index_metadata_invalid")
                row = {
                    "frame_index": frame_index,
                    "source_pts": source_pts,
                    "pts_seconds": pts_seconds,
                    "change_score": round(score, 6),
                }
                frame_output.write(
                    json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n"
                )
                rows.append(row)
            frame_output.flush()
            os.fsync(frame_output.fileno())
        if pending is not None or not rows:
            raise CommandError("frame_index_metadata_invalid")
        try:
            final_stat = video.stat()
        except OSError as error:
            raise CommandError("video_unreadable") from error
        if (
            final_stat.st_size != hashed_stat.st_size
            or final_stat.st_mtime_ns != hashed_stat.st_mtime_ns
        ):
            raise CommandError("video_changed_during_indexing")
        os.replace(frame_temporary, frames_path)
        frame_index_sha256 = _sha256_file(frames_path)
        summary = _frame_summary(
            rows,
            video_sha256=digest,
            frame_index_filename=frames_path.name,
            frame_index_sha256=frame_index_sha256,
            source_time_base=source_time_base,
            probed_rate=native_rate,
        )
        _atomic_json(summary_path, summary)
        return summary, rows
    finally:
        for temporary in (metadata_path, frame_temporary):
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def _sheet(
    video: Path,
    output: Path,
    destination: Path,
    ffmpeg: str,
    purpose: str,
    start: float,
    end: float,
    frame_count: int,
    *,
    fixed_layout: tuple[int, int] | None = None,
) -> dict[str, Any]:
    span = end - start
    if fixed_layout is None:
        columns = min(6, max(1, math.ceil(math.sqrt(frame_count))))
        rows = math.ceil(frame_count / columns)
    else:
        columns, rows = fixed_layout
        if columns < 1 or rows < 1 or columns * rows < frame_count:
            raise ValueError("fixed sheet layout cannot contain its frames")
    rate = frame_count / span
    command = [
        ffmpeg,
        "-y",
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-xerror",
    ]
    if start > 0:
        command += ["-ss", f"{start:.9f}"]
    command += [
        "-t", f"{span:.9f}", "-i", str(video), "-map", "0:v:0", "-vf",
        (
            f"fps={rate:.9f}:start_time=0:eof_action=pass,"
            f"scale={CELL_WIDTH}:-2:flags=area,"
            f"tile={columns}x{rows}:nb_frames={frame_count}:padding=2:margin=2"
        ),
        "-an", "-frames:v", "1",
    ]
    if destination.suffix.casefold() in {".jpg", ".jpeg"}:
        command += ["-q:v", "3"]
    command.append(str(destination))
    _run(command, "ffmpeg_failed", video, output)
    if not destination.is_file() or destination.stat().st_size == 0:
        raise CommandError("image_missing")
    cells = []
    for index in range(frame_count):
        nominal = start + index / rate
        uncertainty = max(nominal - start, end - nominal)
        cells.append(
            {
                "cell_index": index,
                "cell_label": f"cell_{index + 1:03d}",
                "nominal_requested_pts_seconds": _rounded(nominal),
                "source_pts_measured": False,
                "pts_uncertainty_seconds": _rounded(uncertainty),
                "pts_uncertainty_interval": {
                    "start_pts_seconds": _rounded(start),
                    "end_pts_seconds": _rounded(end),
                },
            }
        )
    return {
        "status": "complete",
        "filename": destination.name,
        "purpose": purpose,
        "frame_count": frame_count,
        "sample_rate_hz": round(rate, 6),
        "interval": {
            "start_pts_seconds": _rounded(start),
            "end_pts_seconds": _rounded(end),
        },
        "layout": {"columns": columns, "rows": rows, "cell_order": "row_major"},
        "cells": cells,
    }


def _selected_pts(stdout: str) -> list[tuple[int, float]]:
    selected: list[tuple[int, float]] = []
    for raw_line in stdout.splitlines():
        match = FRAME_RE.fullmatch(raw_line.strip())
        if match:
            selected.append((int(match.group(2)), float(match.group(3))))
    return selected


def _exact_sheet_command(
    video: Path,
    destination: Path,
    ffmpeg: str,
    rows: Sequence[Mapping[str, Any]],
    columns: int,
    sheet_rows: int,
    *,
    seek_start: float | None,
) -> list[str]:
    selection = "+".join(f"eq(pts\\,{int(row['source_pts'])})" for row in rows)
    command = [
        ffmpeg,
        "-y",
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-xerror",
    ]
    if seek_start is not None and seek_start > 0:
        command += ["-ss", f"{seek_start:.9f}", "-copyts"]
    command += [
        "-i",
        str(video),
        "-map",
        "0:v:0",
        "-vf",
        (
            f"select='{selection}',"
            "metadata=add:key=bench_selected:value=1,metadata=print:file=-,"
            f"scale={CELL_WIDTH}:-2:flags=area,"
            f"tile={columns}x{sheet_rows}:nb_frames={len(rows)}:padding=2:margin=2"
        ),
        "-an",
        "-frames:v",
        "1",
    ]
    if destination.suffix.casefold() in {".jpg", ".jpeg"}:
        command += ["-q:v", "3"]
    command.append(str(destination))
    return command


def _exact_sheet(
    video: Path,
    output: Path,
    destination: Path,
    ffmpeg: str,
    purpose: str,
    selected_rows: Sequence[Mapping[str, Any]],
    *,
    represented_start: float,
    represented_end: float,
    sample_rate_hz: float | None,
    source_time_base: tuple[int, int],
    fixed_layout: tuple[int, int] | None = None,
) -> dict[str, Any]:
    rows = sorted(
        {int(row["frame_index"]): dict(row) for row in selected_rows}.values(),
        key=lambda row: int(row["frame_index"]),
    )
    if not rows:
        raise CommandError("source_frames_missing")
    if fixed_layout is None:
        columns = min(6, max(1, math.ceil(math.sqrt(len(rows)))))
        sheet_rows = math.ceil(len(rows) / columns)
    else:
        columns, sheet_rows = fixed_layout
        if columns < 1 or sheet_rows < 1 or columns * sheet_rows < len(rows):
            raise ValueError("fixed sheet layout cannot contain its frames")

    first_pts = float(rows[0]["pts_seconds"])
    cadence = 1.0 / sample_rate_hz if sample_rate_hz and sample_rate_hz > 0 else 0.05
    seek_start = max(0.0, first_pts - max(0.05, cadence * 3))
    expected = [(int(row["source_pts"]), float(row["pts_seconds"])) for row in rows]
    process = _run(
        _exact_sheet_command(
            video,
            destination,
            ffmpeg,
            rows,
            columns,
            sheet_rows,
            seek_start=seek_start,
        ),
        "ffmpeg_failed",
        video,
        output,
    )
    measured = _selected_pts(process.stdout)
    if len(measured) != len(expected) or [item[0] for item in measured] != [
        item[0] for item in expected
    ]:
        process = _run(
            _exact_sheet_command(
                video,
                destination,
                ffmpeg,
                rows,
                columns,
                sheet_rows,
                seek_start=None,
            ),
            "ffmpeg_failed",
            video,
            output,
        )
        measured = _selected_pts(process.stdout)
    if (
        len(measured) != len(expected)
        or [item[0] for item in measured] != [item[0] for item in expected]
        or not destination.is_file()
        or destination.stat().st_size == 0
    ):
        raise CommandError("source_frame_selection_mismatch")

    cells: list[dict[str, Any]] = []
    for cell_index, (row, (measured_source_pts, measured_pts)) in enumerate(
        zip(rows, measured)
    ):
        exact_pts = _pts_seconds(measured_source_pts, source_time_base)
        if not math.isclose(measured_pts, exact_pts, rel_tol=0, abs_tol=0.000001):
            raise CommandError("source_frame_selection_mismatch")
        cells.append(
            {
                "cell_index": cell_index,
                "cell_label": f"cell_{cell_index + 1:03d}",
                "nominal_requested_pts_seconds": exact_pts,
                "source_pts_measured": True,
                "source_pts_seconds": exact_pts,
                "source_pts_value": measured_source_pts,
                "source_pts_time_base": {
                    "numerator": source_time_base[0],
                    "denominator": source_time_base[1],
                },
                "source_frame_index": int(row["frame_index"]),
                "pts_uncertainty_seconds": 0.0,
                "pts_uncertainty_interval": {
                    "start_pts_seconds": exact_pts,
                    "end_pts_seconds": exact_pts,
                },
            }
        )
    return {
        "status": "complete",
        "filename": destination.name,
        "purpose": purpose,
        "frame_count": len(cells),
        "sample_rate_hz": _rounded(sample_rate_hz) if sample_rate_hz else None,
        "interval": {
            "start_pts_seconds": _rounded(min(represented_start, first_pts)),
            "end_pts_seconds": _rounded(
                max(represented_end, float(rows[-1]["pts_seconds"]))
            ),
        },
        "layout": {
            "columns": columns,
            "rows": sheet_rows,
            "cell_order": "row_major",
        },
        "cells": cells,
    }


def _change_sheets(
    video: Path,
    output: Path,
    ffmpeg: str,
    scan: dict[str, Any],
    frame_rows: Sequence[Mapping[str, Any]],
    errors: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    evidence: list[dict[str, Any]] = []
    by_index = {int(row["frame_index"]): row for row in frame_rows}
    native_rate_value = scan.get("native_frame_rate_fps")
    native_rate = (
        float(native_rate_value)
        if isinstance(native_rate_value, (int, float))
        and not isinstance(native_rate_value, bool)
        and float(native_rate_value) > 0
        else None
    )
    source_time_base = _time_base(scan.get("source_time_base"))
    if source_time_base is None:
        _record(errors, "change_sheet", CommandError("source_time_base_invalid"))
        return []
    for candidate in scan["change_candidates"]:
        rank = int(candidate["change_rank"])
        first = int(candidate["window_start_frame"])
        last = int(candidate["window_end_frame"])
        rows = [by_index[index] for index in range(first, last + 1) if index in by_index]
        try:
            sheet = _exact_sheet(
                video,
                output,
                output / f"change_{rank:02d}.png",
                ffmpeg,
                "temporal_change_candidate",
                rows,
                represented_start=float(candidate["window_start_pts_seconds"]),
                represented_end=float(candidate["window_end_pts_seconds"]),
                sample_rate_hz=native_rate,
                source_time_base=source_time_base,
                fixed_layout=(2, 2),
            )
            evidence.append({**candidate, **sheet})
        except CommandError as error:
            evidence.append(
                {
                    **candidate,
                    "status": "failed",
                    "purpose": "temporal_change_candidate",
                    "reason": error.code,
                }
            )
            _record(errors, "change_sheet", error)
            errors[-1]["change_rank"] = rank
    return evidence


def _pack_change_sheets(
    video: Path,
    output: Path,
    ffmpeg: str,
    sheets: Sequence[Mapping[str, Any]],
) -> dict[str, Any]:
    complete = [
        sheet
        for sheet in sheets
        if sheet.get("status") == "complete"
        and isinstance(sheet.get("filename"), str)
        and isinstance(sheet.get("interval"), Mapping)
        and isinstance(sheet.get("layout"), Mapping)
        and isinstance(sheet.get("cells"), list)
        and isinstance(sheet.get("pts_seconds"), (int, float))
        and not isinstance(sheet.get("pts_seconds"), bool)
    ]
    if not complete:
        raise CommandError("change_sheets_missing")
    # A partial atlas would make a successful pack look complete while silently
    # dropping a selected interval. Keep the individual-sheet fallback instead.
    if len(complete) != len(sheets):
        raise CommandError("change_sheet_partial")

    destination = output / "change_candidates.png"
    command = [
        ffmpeg,
        "-y",
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-xerror",
    ]
    for sheet in complete:
        image = output / str(sheet["filename"])
        if not image.is_file():
            raise CommandError("change_sheet_missing")
        command += ["-i", str(image)]

    columns = min(CHANGE_ATLAS_COLUMNS, len(complete))
    rows = math.ceil(len(complete) / columns)
    positions: list[str] = []
    for index in range(len(complete)):
        row, column = divmod(index, columns)
        row_start = row * columns
        x = "0" if column == 0 else "+".join(
            f"w{row_start + prior_column}" for prior_column in range(column)
        )
        y = "0" if row == 0 else "+".join(
            f"h{prior_row * columns}" for prior_row in range(row)
        )
        positions.append(f"{x}_{y}")
    layout = "|".join(positions)
    inputs = "".join(f"[{index}:v]" for index in range(len(complete)))
    command += [
        "-filter_complex",
        f"{inputs}xstack=inputs={len(complete)}:layout={layout}:fill=black[out]",
        "-map",
        "[out]",
        "-frames:v",
        "1",
        str(destination),
    ]
    _run(command, "ffmpeg_failed", video, output)
    if not destination.is_file() or destination.stat().st_size == 0:
        raise CommandError("image_missing")

    cells: list[dict[str, Any]] = []
    starts: list[float] = []
    ends: list[float] = []
    inner_columns = 2
    inner_rows = 2
    atlas_columns = columns * inner_columns
    atlas_rows = rows * inner_rows
    for index, sheet in enumerate(complete):
        interval = sheet["interval"]
        start = float(interval["start_pts_seconds"])
        end = float(interval["end_pts_seconds"])
        starts.append(start)
        ends.append(end)
        inner_layout = sheet["layout"]
        if (
            inner_layout.get("columns") != inner_columns
            or inner_layout.get("rows") != inner_rows
            or inner_layout.get("cell_order") != "row_major"
        ):
            raise CommandError("change_sheet_layout_invalid")
        outer_row, outer_column = divmod(index, columns)
        rank = int(sheet["change_rank"])
        for source_cell in sheet["cells"]:
            if not isinstance(source_cell, Mapping):
                raise CommandError("change_sheet_cells_invalid")
            inner_index = source_cell.get("cell_index")
            if (
                not isinstance(inner_index, int)
                or isinstance(inner_index, bool)
                or inner_index < 0
                or inner_index >= inner_columns * inner_rows
            ):
                raise CommandError("change_sheet_cells_invalid")
            inner_row, inner_column = divmod(inner_index, inner_columns)
            atlas_row = outer_row * inner_rows + inner_row
            atlas_column = outer_column * inner_columns + inner_column
            cell = dict(source_cell)
            cell["cell_index"] = atlas_row * atlas_columns + atlas_column
            cell["cell_label"] = (
                f"change_rank_{rank:02d}_{source_cell.get('cell_label', '')}"
            )
            cells.append(cell)
    cells.sort(key=lambda cell: cell["cell_index"])
    return {
        "status": "complete",
        "filename": destination.name,
        "purpose": "temporal_change_candidates",
        "frame_count": len(cells),
        "interval": {
            "start_pts_seconds": _rounded(min(starts)),
            "end_pts_seconds": _rounded(max(ends)),
        },
        "layout": {
            "columns": atlas_columns,
            "rows": atlas_rows,
            "cell_order": "row_major",
        },
        "cells": cells,
    }


def _request(value: Any) -> tuple[float, float, float]:
    if isinstance(value, Mapping):
        values = (
            value.get("start_seconds"), value.get("end_seconds"),
            value.get("sample_rate_hz", DEFAULT_INTERVAL_HZ),
        )
    elif isinstance(value, Sequence) and not isinstance(value, (str, bytes)) and len(value) in {2, 3}:
        values = (*value, DEFAULT_INTERVAL_HZ) if len(value) == 2 else value
    else:
        raise CommandError("interval_shape_invalid")
    if any(isinstance(item, bool) or not isinstance(item, (int, float)) for item in values):
        raise CommandError("interval_value_invalid")
    start, end, rate = (float(item) for item in values)
    if not all(math.isfinite(item) for item in (start, end, rate)):
        raise CommandError("interval_value_invalid")
    if start >= end or rate <= 0:
        raise CommandError("interval_range_invalid")
    return start, end, rate


def _merge(intervals: list[tuple[float, float]]) -> list[tuple[float, float]]:
    merged: list[list[float]] = []
    for start, end in sorted(intervals):
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(end, merged[-1][1])
        else:
            merged.append([start, end])
    return [(start, end) for start, end in merged]


def _coverage(intervals: list[tuple[float, float]], duration: float) -> dict[str, Any]:
    sampled = _merge(intervals)
    gaps: list[tuple[float, float]] = []
    cursor = 0.0
    for start, end in sampled:
        if start > cursor:
            gaps.append((cursor, start))
        cursor = max(cursor, end)
    if cursor < duration:
        gaps.append((cursor, duration))

    def items(values: list[tuple[float, float]]) -> list[dict[str, float]]:
        return [
            {"start_seconds": _rounded(start), "end_seconds": _rounded(end)}
            for start, end in values if end > start
        ]

    return {"sampled_intervals": items(sampled), "unsampled_intervals": items(gaps)}


def _scan_coverage(scan: Mapping[str, Any], duration: float) -> dict[str, Any]:
    if scan.get("status") != "complete":
        return {
            "sampling": "not_indexed",
            "temporal_coverage": "none",
            "continuous_coverage": False,
            "semantic_review": False,
            "indexed_frame_count": 0,
            "frame_indices_contiguous": False,
            "unsampled_edge_intervals": [
                {
                    "start_seconds": 0.0,
                    "end_seconds": _rounded(duration),
                    "sampled_boundary": None,
                }
            ],
        }
    return {
        "sampling": "every_decoded_source_frame",
        "temporal_coverage": "exhaustive_frame_index",
        "continuous_coverage": False,
        "semantic_review": False,
        "indexed_frame_count": int(scan.get("frame_count") or 0),
        "frame_indices_contiguous": scan.get("frame_indices_contiguous") is True,
        "first_frame_index": scan.get("first_frame_index"),
        "last_frame_index": scan.get("last_frame_index"),
        "first_pts_seconds": scan.get("first_pts_seconds"),
        "last_pts_seconds": scan.get("last_pts_seconds"),
        "unsampled_edge_intervals": [],
        "limitations": [
            "Automated full-frame change scoring is not semantic video review."
        ],
    }


def _nearest_frame(
    rows: Sequence[Mapping[str, Any]], pts_values: Sequence[float], target: float
) -> Mapping[str, Any]:
    position = bisect_left(pts_values, target)
    candidates = rows[max(0, position - 1) : min(len(rows), position + 1)]
    return min(
        candidates,
        key=lambda row: (
            abs(float(row["pts_seconds"]) - target),
            int(row["frame_index"]),
        ),
    )


def _interval_frame_rows(
    rows: Sequence[Mapping[str, Any]],
    start: float,
    end: float,
    rate: float,
    native_rate: float,
) -> tuple[list[Mapping[str, Any]], bool]:
    pts_values = [float(row["pts_seconds"]) for row in rows]
    first = bisect_left(pts_values, start)
    last = bisect_left(pts_values, end)
    in_interval = list(rows[first:last])
    if not in_interval:
        return [], False
    if rate >= native_rate - 1e-9:
        selected = in_interval
    else:
        count = max(1, math.ceil((end - start) * rate))
        interval_pts = [float(row["pts_seconds"]) for row in in_interval]
        selected = [
            _nearest_frame(
                in_interval,
                interval_pts,
                min(math.nextafter(end, start), start + index / rate),
            )
            for index in range(count)
        ]
    selected = sorted(
        {int(row["frame_index"]): row for row in selected}.values(),
        key=lambda row: int(row["frame_index"]),
    )
    capped = len(selected) > MAX_INTERVAL_FRAMES
    if capped:
        if MAX_INTERVAL_FRAMES == 1:
            selected = [selected[0]]
        else:
            selected = [
                selected[round(index * (len(selected) - 1) / (MAX_INTERVAL_FRAMES - 1))]
                for index in range(MAX_INTERVAL_FRAMES)
            ]
    return selected, capped


def inspect_video(
    video_path: Path | str,
    output_dir: Path | str,
    requested_intervals: Sequence[Any] = (),
    *,
    index_dir: Path | str | None = None,
    video_sha256: str | None = None,
    ffmpeg: str | None = None,
    ffprobe: str | None = None,
    scan_overview: bool = True,
) -> dict[str, Any]:
    """Return JSON-safe metadata; all generated images stay in ``output_dir``."""
    video, output = Path(video_path).resolve(), Path(output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    errors: list[dict[str, Any]] = []
    result: dict[str, Any] = {
        "schema_version": 1,
        "kind": "investigation_video_evidence",
        "status": "failed",
        "video": {"name": video.name},
        "temporal_scan": {"status": "not_run", "spatial_scope": "full_frame"},
        "change_images": [],
        "overview": {"status": "not_run", "purpose": "whole_video_overview"},
        "requested_intervals": [],
        "coverage": {},
        "errors": errors,
    }
    if not video.is_file():
        _record(errors, "probe", CommandError("video_missing"))
        return result
    ffprobe_tool = _tool(ffprobe, "ffprobe")
    if not ffprobe_tool:
        _record(errors, "probe", CommandError("ffprobe_missing"))
        return result
    try:
        probe = _probe(video, output, ffprobe_tool)
    except CommandError as error:
        _record(errors, "probe", error)
        return result
    result["video"].update(probe)
    duration = float(probe["duration_seconds"])
    ffmpeg_tool = _tool(ffmpeg, "ffmpeg")
    frame_rows: list[dict[str, Any]] = []
    frame_summary: dict[str, Any] | None = None
    if ffmpeg_tool:
        try:
            frame_summary, frame_rows = build_frame_index(
                video,
                Path(index_dir) if index_dir is not None else output / "investigation_index",
                video_sha256=video_sha256,
                probe=probe,
                ffmpeg=ffmpeg_tool,
                ffprobe=ffprobe_tool,
            )
            result["frame_index_summary"] = copy_summary = dict(frame_summary)
            copy_summary.pop("frame_index_filename", None)
        except CommandError as error:
            _record(errors, "frame_index", error)

    if not ffmpeg_tool:
        _record(errors, "extraction", CommandError("ffmpeg_missing"))
        result["temporal_scan"] = {
            "status": "failed", "spatial_scope": "full_frame", "reason": "ffmpeg_missing"
        }
        result["overview"] = {
            "status": "failed",
            "purpose": "whole_video_overview",
            "reason": "ffmpeg_missing",
        }
    elif scan_overview:
        if frame_summary is not None:
            scan = {
                "status": "complete",
                "spatial_scope": "full_frame",
                "purpose": "candidate_selection_only",
                "method": "ffmpeg_scene_score_every_source_frame",
                "frame_count": frame_summary["frame_count"],
                "sample_count": frame_summary["frame_count"],
                "first_frame_index": frame_summary["first_frame_index"],
                "last_frame_index": frame_summary["last_frame_index"],
                "frame_indices_contiguous": frame_summary[
                    "frame_indices_contiguous"
                ],
                "first_pts_seconds": frame_summary["first_pts_seconds"],
                "last_pts_seconds": frame_summary["last_pts_seconds"],
                "source_time_base": frame_summary["source_time_base"],
                "native_frame_rate_fps": frame_summary["native_frame_rate_fps"],
                "score_distribution": frame_summary["score_distribution"],
                "change_candidates": frame_summary["top_change_windows"],
            }
            result["temporal_scan"] = scan
            change_sheets = _change_sheets(
                video, output, ffmpeg_tool, scan, frame_rows, errors
            )
            result["change_images"] = change_sheets
            if len(change_sheets) > 1:
                try:
                    result["change_images"] = [
                        _pack_change_sheets(
                            video, output, ffmpeg_tool, change_sheets
                        )
                    ]
                except CommandError as error:
                    _record(errors, "change_atlas", error)
        else:
            result["temporal_scan"] = {
                "status": "failed",
                "spatial_scope": "full_frame",
                "reason": "frame_index_unavailable",
            }
        try:
            count = min(MAX_OVERVIEW_FRAMES, max(1, math.ceil(duration * 2)))
            result["overview"] = _sheet(
                video, output, output / "overview.jpg", ffmpeg_tool,
                "whole_video_overview", 0.0, duration, count,
            )
        except CommandError as error:
            _record(errors, "overview", error)
            result["overview"] = {
                "status": "failed",
                "purpose": "whole_video_overview",
                "reason": error.code,
            }
    else:
        result["temporal_scan"] = {
            "status": "not_run",
            "spatial_scope": "full_frame",
            "reason": "follow_up_intervals_only",
        }
        result["overview"] = {
            "status": "not_run",
            "purpose": "whole_video_overview",
            "reason": "follow_up_intervals_only",
        }

    sampled: list[tuple[float, float]] = []
    native_rate_value = (
        frame_summary.get("native_frame_rate_fps") if frame_summary is not None else None
    )
    native_rate = (
        float(native_rate_value)
        if isinstance(native_rate_value, (int, float))
        and not isinstance(native_rate_value, bool)
        and math.isfinite(float(native_rate_value))
        and float(native_rate_value) > 0
        else None
    )
    source_time_base = (
        _time_base(frame_summary.get("source_time_base"))
        if frame_summary is not None
        else None
    )
    for index, raw in enumerate(requested_intervals):
        try:
            requested_start, requested_end, requested_rate = _request(raw)
        except CommandError as error:
            _record(errors, "requested_interval", error, index)
            result["requested_intervals"].append(
                {"request_index": index, "status": "not_sampled", "reason": error.code}
            )
            continue
        entry: dict[str, Any] = {
            "request_index": index,
            "purpose": "requested_interval",
            "requested": {
                "start_seconds": _rounded(requested_start),
                "end_seconds": _rounded(requested_end),
                "sample_rate_hz": _rounded(requested_rate),
            },
            "status": "not_sampled",
        }
        result["requested_intervals"].append(entry)
        if index >= MAX_INTERVALS:
            error = CommandError("requested_interval_limit")
            entry["reason"] = error.code
            _record(errors, "requested_interval", error, index)
            continue
        start, end = max(0.0, requested_start), min(duration, requested_end)
        if start >= end:
            error = CommandError("interval_outside_video")
            entry["reason"] = error.code
            _record(errors, "requested_interval", error, index)
            continue
        limits: list[str] = []
        if (start, end) != (requested_start, requested_end):
            limits.append("clipped_to_video")
        if native_rate is None or not frame_rows or source_time_base is None:
            error = CommandError("frame_index_unavailable")
            entry["reason"] = error.code
            _record(errors, "requested_interval", error, index)
            continue
        rate = min(requested_rate, native_rate)
        if rate != requested_rate:
            limits.append("sample_rate_capped")
        selected_rows, frame_count_capped = _interval_frame_rows(
            frame_rows, start, end, rate, native_rate
        )
        if not selected_rows:
            error = CommandError("no_source_frame_in_interval")
            entry["reason"] = error.code
            _record(errors, "requested_interval", error, index)
            continue
        if frame_count_capped:
            limits.append("frame_count_capped")
        if ffmpeg_tool:
            try:
                entry.update(
                    _exact_sheet(
                        video,
                        output,
                        output / f"interval_{index:03d}.jpg",
                        ffmpeg_tool,
                        "requested_interval",
                        selected_rows,
                        represented_start=start,
                        represented_end=end,
                        sample_rate_hz=rate,
                        source_time_base=source_time_base,
                    )
                )
                entry["status"] = (
                    "partial"
                    if {"clipped_to_video", "frame_count_capped"}.intersection(limits)
                    else "complete"
                )
                entry["reason"] = ""
                sampled.append((start, end))
            except CommandError as error:
                entry.update({"status": "failed", "reason": error.code})
                _record(errors, "requested_interval", error, index)
        else:
            entry["reason"] = "ffmpeg_missing"
        if limits:
            entry["limits"] = limits

    result["coverage"] = {
        "full_frame_scan": _scan_coverage(result["temporal_scan"], duration),
        "higher_rate": _coverage(sampled, duration),
    }
    result["status"] = "complete" if not errors else "partial"
    return result
