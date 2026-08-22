#!/usr/bin/env python3
"""Extract bounded visual evidence without interpreting frame content."""

from __future__ import annotations

import json
import math
import os
import re
import shutil
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any


SCAN_HZ = 12.0
MAX_SCAN_SAMPLES = 3600
MAX_CHANGE_SHEETS = 12
MAX_OVERVIEW_FRAMES = 12
MAX_INTERVALS = 6
MAX_INTERVAL_FRAMES = 36
DEFAULT_INTERVAL_HZ = 8.0
MAX_INTERVAL_HZ = 12.0
CELL_WIDTH = 240
CHANGE_ATLAS_COLUMNS = 3

PTS_RE = re.compile(r"\bpts_time:([-+0-9.eE]+)")
SCORE_RE = re.compile(r"^lavfi\.scene_score=([-+0-9.eE]+)$")


class CommandError(Exception):
    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(code)
        self.code, self.detail = code, detail


def _rounded(value: float) -> float:
    return round(value, 6)


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
            "format=duration:stream=width,height,duration,avg_frame_rate,codec_name",
            "-of", "json", str(video),
        ],
        "ffprobe_failed", video, output, 30,
    )
    try:
        payload = json.loads(process.stdout)
        stream = payload["streams"][0]
        duration = float(payload.get("format", {}).get("duration") or stream.get("duration"))
        width, height = int(stream["width"]), int(stream["height"])
        if min(duration, width, height) <= 0 or not math.isfinite(duration):
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
    }


def _scan(video: Path, output: Path, ffmpeg: str, duration: float) -> dict[str, Any]:
    rate = min(SCAN_HZ, MAX_SCAN_SAMPLES / duration)
    process = _run(
        [
            ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(video),
            "-vf", (
                f"fps={rate:.9f},scale=160:-2:flags=area,"
                "select='gte(scene,0)',metadata=print:file=-"
            ),
            "-an", "-f", "null", "-",
        ],
        "ffmpeg_failed", video, output,
    )
    samples: list[tuple[float, float]] = []
    pts: float | None = None
    for line in process.stdout.splitlines():
        if match := PTS_RE.search(line):
            try:
                pts = float(match.group(1))
            except ValueError:
                pts = None
        elif pts is not None and (match := SCORE_RE.fullmatch(line.strip())):
            score = float(match.group(1))
            if math.isfinite(pts) and math.isfinite(score):
                samples.append((pts, score))
    if not samples:
        raise CommandError("scan_samples_missing")

    gap = max(0.05, 1.0 / rate)
    chosen: list[tuple[float, float]] = []
    for pts, score in sorted(samples, key=lambda item: (-item[1], item[0])):
        if score > 0 and all(abs(pts - prior) >= gap for prior, _ in chosen):
            chosen.append((pts, score))
            if len(chosen) == MAX_CHANGE_SHEETS:
                break
    candidates = [
        {"pts_seconds": _rounded(pts), "change_score": round(score, 6), "change_rank": rank}
        for rank, (pts, score) in enumerate(chosen, 1)
    ]
    return {
        "status": "complete",
        "spatial_scope": "full_frame",
        "purpose": "candidate_selection_only",
        "method": "ffmpeg_scene_score",
        "sample_rate_hz": round(rate, 6),
        "maximum_samples": MAX_SCAN_SAMPLES,
        "sample_count": len(samples),
        "sampled_pts_seconds": [_rounded(pts) for pts, _ in samples],
        "change_candidates": candidates,
    }


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
    command = [ffmpeg, "-y", "-nostdin", "-hide_banner", "-loglevel", "error"]
    if start > 0:
        command += ["-ss", f"{start:.9f}"]
    command += [
        "-t", f"{span:.9f}", "-i", str(video), "-vf",
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


def _change_sheets(
    video: Path,
    output: Path,
    ffmpeg: str,
    duration: float,
    scan: dict[str, Any],
    errors: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    evidence: list[dict[str, Any]] = []
    step = 1.0 / float(scan["sample_rate_hz"])
    for candidate in scan["change_candidates"]:
        rank, pts = int(candidate["change_rank"]), float(candidate["pts_seconds"])
        start, end = max(0.0, pts - step), min(duration, pts + 2 * step)
        frame_count = max(1, min(3, round((end - start) / step)))
        try:
            sheet = _sheet(
                video, output, output / f"change_{rank:02d}.png", ffmpeg,
                "temporal_change_candidate", start, end, frame_count,
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
    command = [ffmpeg, "-y", "-nostdin", "-hide_banner", "-loglevel", "error"]
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
            "sampling": "not_sampled",
            "temporal_coverage": "none",
            "continuous_coverage": False,
            "sampled_pts_seconds": [],
            "nominal_cadence_seconds": None,
            "between_point_gaps": {
                "count": 0,
                "minimum_seconds": None,
                "maximum_seconds": None,
                "all_interiors_unsampled": False,
            },
            "unsampled_edge_intervals": [
                {
                    "start_seconds": 0.0,
                    "end_seconds": _rounded(duration),
                    "sampled_boundary": None,
                }
            ],
        }

    points = sorted(
        {
            min(duration, max(0.0, float(value)))
            for value in scan.get("sampled_pts_seconds", [])
            if isinstance(value, (int, float))
            and not isinstance(value, bool)
            and math.isfinite(float(value))
        }
    )
    gaps = [later - earlier for earlier, later in zip(points, points[1:])]
    edges: list[dict[str, Any]] = []
    if not points:
        edges.append(
            {
                "start_seconds": 0.0,
                "end_seconds": _rounded(duration),
                "sampled_boundary": None,
            }
        )
    else:
        if points[0] > 0:
            edges.append(
                {
                    "start_seconds": 0.0,
                    "end_seconds": _rounded(points[0]),
                    "sampled_boundary": "end",
                }
            )
        if points[-1] < duration:
            edges.append(
                {
                    "start_seconds": _rounded(points[-1]),
                    "end_seconds": _rounded(duration),
                    "sampled_boundary": "start",
                }
            )
    rate = float(scan.get("sample_rate_hz") or 0.0)
    return {
        "sampling": "periodic_pts",
        "temporal_coverage": "sample_points_only",
        "continuous_coverage": False,
        "sampled_pts_seconds": [_rounded(point) for point in points],
        "nominal_cadence_seconds": _rounded(1.0 / rate) if rate > 0 else None,
        "between_point_gaps": {
            "count": len(gaps),
            "minimum_seconds": _rounded(min(gaps)) if gaps else None,
            "maximum_seconds": _rounded(max(gaps)) if gaps else None,
            "all_interiors_unsampled": bool(gaps),
        },
        "unsampled_edge_intervals": edges,
    }


def inspect_video(
    video_path: Path | str,
    output_dir: Path | str,
    requested_intervals: Sequence[Any] = (),
    *,
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
        try:
            scan = _scan(video, output, ffmpeg_tool, duration)
            result["temporal_scan"] = scan
            change_sheets = _change_sheets(
                video, output, ffmpeg_tool, duration, scan, errors
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
        except CommandError as error:
            _record(errors, "temporal_scan", error)
            result["temporal_scan"] = {
                "status": "failed", "spatial_scope": "full_frame", "reason": error.code
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
        rate = min(requested_rate, MAX_INTERVAL_HZ)
        if rate != requested_rate:
            limits.append("sample_rate_capped")
        frame_count = max(1, math.ceil((end - start) * rate))
        if frame_count > MAX_INTERVAL_FRAMES:
            frame_count = MAX_INTERVAL_FRAMES
            limits.append("frame_count_capped")
        if ffmpeg_tool:
            try:
                entry.update(_sheet(
                    video, output, output / f"interval_{index:03d}.jpg", ffmpeg_tool,
                    "requested_interval", start, end, frame_count,
                ))
                entry["status"] = "partial" if "clipped_to_video" in limits else "complete"
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
