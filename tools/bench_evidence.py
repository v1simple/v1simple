#!/usr/bin/env python3
"""Build and query bounded, citation-ready bench evidence indexes."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import tempfile
from bisect import bisect_left
from collections import Counter, defaultdict
from collections.abc import Iterable, Iterator, Mapping, Sequence
from functools import lru_cache
from pathlib import Path
from typing import Any


sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
BENCH_SCRIPTS = ROOT / "scripts" / "bench"
INDEX_DIRECTORY = "investigation_index"
ATTACHMENT_DIRECTORY = "investigation_sheets"
INDEX_SCHEMA_VERSION = 2
INDEX_ALGORITHM = "bench_evidence_v2"
RECORD_INDEX_FILENAME = "records.ndjson"
MANIFEST_FILENAME = "manifest.json"
TIME_BUCKET_NS = 1_000_000_000
DEFAULT_RESULT_LIMIT = 20
MAX_RESULT_LIMIT = 100
MAX_CONTEXT_RECORDS = 3
MAX_SOURCE_LINES = 200
MAX_STDOUT_BYTES = 262_144
MAX_STRING_LENGTH = 4096
VIDEO_SUFFIXES = {".mov", ".mp4", ".m4v", ".mkv"}
TEXT_SUFFIXES = {".ndjson", ".jsonl", ".csv", ".tsv", ".log", ".err", ".txt"}
IGNORED_NAMES = {"investigation.json", "investigation_debug.json"}
AUXILIARY_VIDEO_NAMES = {".camera_preflight.mov"}
HOST_NS_FIELDS = (
    "host_earliest_ns",
    "host_estimate_ns",
    "host_latest_ns",
    "host_monotonic_ns",
    "hostMonotonicNs",
    "requestedHostMonotonicNs",
    "intendedHostMonotonicNs",
    "callback_host_ns",
    "host_capture_ns",
    "observed_host_ns",
)
DUT_US_FIELDS = (
    "stage_dut_micros",
    "rx_dut_micros",
    "dutMicros",
    "dut_micros",
    "dut_monotonic_us",
    "render_request_dut_micros",
    "display_commit_dut_micros",
    "state_published_dut_micros",
    "alert_published_dut_micros",
    "state_rx_dut_micros",
    "alert_rx_dut_micros",
)
DUT_MS_FIELDS = ("stage_dut_millis", "rx_dut_millis", "millis", "dutMillis")
CLOCK_SEGMENT_FIELDS = ("clock_segment", "clockSegment")
LOG_FIELD_RE = re.compile(r"\b([A-Za-z][A-Za-z0-9_.-]{1,63})\s*[=:]")


class EvidenceError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _integer(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, str):
        try:
            return int(value, 10)
        except ValueError:
            return None
    return None


def _number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value)
    if isinstance(value, str):
        try:
            parsed = float(value)
        except ValueError:
            return None
        return parsed if math.isfinite(parsed) else None
    return None


def _first(record: Mapping[str, Any], names: Sequence[str]) -> Any:
    for name in names:
        if name in record and record[name] not in (None, ""):
            return record[name]
    return None


def _relative_file(run_dir: Path, raw_path: str) -> Path:
    candidate_path = Path(raw_path)
    if candidate_path.is_absolute() or ".." in candidate_path.parts:
        raise EvidenceError("artifact path must be run-relative")
    candidate = (run_dir / candidate_path).resolve()
    try:
        candidate.relative_to(run_dir.resolve())
    except ValueError as error:
        raise EvidenceError("artifact path escapes the run") from error
    if not candidate.is_file() or candidate.is_symlink():
        raise EvidenceError(f"artifact is unavailable: {raw_path}")
    return candidate


def index_root(run_dir: Path, *, create: bool) -> Path:
    resolved_run = run_dir.resolve()
    if not resolved_run.is_dir():
        raise EvidenceError("run directory is unavailable")
    root = resolved_run / INDEX_DIRECTORY
    if root.is_symlink():
        raise EvidenceError("investigation index must not be a symlink")
    if create:
        root.mkdir(exist_ok=True)
    if not root.is_dir() or root.resolve() != root:
        raise EvidenceError("investigation index is unavailable")
    return root


def _atomic_json(path: Path, payload: Mapping[str, Any]) -> None:
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
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


def _artifact_paths(run_dir: Path) -> tuple[list[Path], list[Path]]:
    text: list[Path] = []
    videos: list[Path] = []
    for path in sorted(run_dir.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        relative = path.relative_to(run_dir)
        if (
            INDEX_DIRECTORY in relative.parts
            or ATTACHMENT_DIRECTORY in relative.parts
            or path.name in IGNORED_NAMES
            or path.name.startswith(".investigation.")
        ):
            continue
        suffix = path.suffix.casefold()
        if suffix in VIDEO_SUFFIXES and path.name not in AUXILIARY_VIDEO_NAMES:
            videos.append(path)
        elif suffix in TEXT_SUFFIXES:
            text.append(path)
    return text, videos


def _format(path: Path) -> str:
    suffix = path.suffix.casefold()
    if suffix in {".ndjson", ".jsonl"}:
        return "ndjson"
    if suffix in {".csv", ".tsv"}:
        return "csv"
    return "log"


def _csv_records(path: Path) -> Iterator[tuple[int, int, int, dict[str, str]]]:
    delimiter = "\t" if path.suffix.casefold() == ".tsv" else ","
    with path.open("r", encoding="utf-8", errors="replace", newline="") as handle:
        kept = [
            (physical_line, line)
            for physical_line, line in enumerate(handle, 1)
            if line.strip() and not line.startswith("#")
        ]
    if not kept:
        return
    reader = csv.DictReader((line for _physical, line in kept), delimiter=delimiter)
    previous_line_num = 1
    for row_number, row in enumerate(reader, 1):
        start_filtered = previous_line_num
        end_filtered = reader.line_num - 1
        previous_line_num = reader.line_num
        physical_start = kept[min(start_filtered, len(kept) - 1)][0]
        physical_end = kept[min(end_filtered, len(kept) - 1)][0]
        yield row_number, physical_start, physical_end, dict(row)


def _records(path: Path) -> Iterator[tuple[int, int, int, Any]]:
    format_name = _format(path)
    if format_name == "csv":
        yield from _csv_records(path)
        return
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for physical_line, line in enumerate(handle, 1):
            if format_name == "log":
                yield physical_line, physical_line, physical_line, line.rstrip("\n")
                continue
            try:
                record: Any = json.loads(line)
            except json.JSONDecodeError:
                record = {"_invalid_json": line.rstrip("\n")}
            yield physical_line, physical_line, physical_line, record


def _value_type(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, Mapping):
        return "object"
    return type(value).__name__


def _fields(value: Any, prefix: str = "", depth: int = 0) -> Iterator[tuple[str, str]]:
    if isinstance(value, Mapping):
        for key, child in value.items():
            name = f"{prefix}.{key}" if prefix else str(key)
            yield name, _value_type(child)
            if depth < 3:
                yield from _fields(child, name, depth + 1)
    elif isinstance(value, list) and depth < 3:
        for child in value[:32]:
            if isinstance(child, Mapping):
                yield from _fields(child, f"{prefix}[]", depth + 1)


def _log_record(line: str) -> dict[str, Any]:
    return {
        "line": line,
        "fields": {name: True for name in sorted(set(LOG_FIELD_RE.findall(line)))},
    }


def _kind(path: Path, record: Any) -> str:
    name = path.name.casefold()
    if not isinstance(record, Mapping):
        return "log_line"
    if _format(path) == "log":
        return "log_line"
    if "causal_trace" in name and record.get("stage"):
        return str(record["stage"])
    if "display_commits" in name:
        return "display_commit_row"
    if "replay_stimulus" in name:
        return "stimulus_record"
    if name.startswith("perf_"):
        return "perf_sample"
    if name == "metrics.ndjson":
        return "metric_record"
    for field in ("kind", "event", "type", "state"):
        if record.get(field) not in (None, ""):
            return str(record[field])
    return "structured_record"


def _clock_segment(record: Mapping[str, Any]) -> Any:
    causal = record.get("causal_identifiers")
    if isinstance(causal, Mapping) and causal.get("clock_segment") not in (None, ""):
        return causal["clock_segment"]
    return _first(record, CLOCK_SEGMENT_FIELDS)


def _segment_instance(record: Mapping[str, Any]) -> int | None:
    causal = record.get("causal_identifiers")
    if isinstance(causal, Mapping):
        value = _integer(causal.get("segment_instance"))
        if value is not None:
            return value
    return _integer(record.get("segment_instance"))


def _raw_clocks(record: Mapping[str, Any]) -> list[dict[str, Any]]:
    clocks: list[dict[str, Any]] = []
    raw_clock = record.get("raw_clock")
    raw_timestamp = record.get("raw_timestamp")
    if isinstance(raw_clock, str):
        if isinstance(raw_timestamp, Mapping):
            value = _number(raw_timestamp.get("value"))
            timescale = _number(raw_timestamp.get("timescale"))
            if value is not None and timescale and timescale > 0:
                clocks.append({"clock": raw_clock, "value": value / timescale})
        elif (value := _number(raw_timestamp)) is not None:
            clocks.append({"clock": raw_clock, "value": value})
    for field in HOST_NS_FIELDS:
        if (value := _number(record.get(field))) is not None:
            clocks.append({"clock": "host_monotonic_ns", "value": value, "field": field})
    segment = _clock_segment(record)
    for field in DUT_US_FIELDS:
        if (value := _number(record.get(field))) is not None and value > 0:
            clocks.append(
                {
                    "clock": "dut_monotonic_us",
                    "value": value,
                    "field": field,
                    "clock_segment": segment,
                    "segment_instance": _segment_instance(record),
                }
            )
    for field in DUT_MS_FIELDS:
        if (value := _number(record.get(field))) is not None and value > 0:
            clocks.append(
                {
                    "clock": "dut_monotonic_ms",
                    "value": value,
                    "field": field,
                    "clock_segment": segment,
                    "segment_instance": _segment_instance(record),
                }
            )
    if (value := _number(record.get("replayOffsetSeconds"))) is not None:
        clocks.append({"clock": "replay_offset_s", "value": value})
    for prefix in ("video_pts", "source_pts"):
        value = _number(record.get(f"{prefix}_value"))
        timescale = _number(record.get(f"{prefix}_timescale"))
        if value is not None and timescale and timescale > 0:
            clocks.append({"clock": f"{prefix}_s", "value": value / timescale})
    unique: dict[tuple[str, float, str], dict[str, Any]] = {}
    for item in clocks:
        key = (
            str(item["clock"]),
            float(item["value"]),
            str(item.get("field") or ""),
        )
        unique[key] = item
    return list(unique.values())


def _source_identity(run_dir: Path, timeline: Path, source: str) -> str | None:
    source_path = Path(source)
    if source_path.is_absolute() or ".." in source_path.parts or source.startswith("derived:"):
        return None
    candidates = [timeline.parent / source_path, run_dir / source_path]
    for candidate in candidates:
        try:
            relative = candidate.resolve().relative_to(run_dir.resolve())
        except (OSError, ValueError):
            continue
        if candidate.is_file() and not candidate.is_symlink():
            return relative.as_posix()
    return None


def _timeline_source_map(
    run_dir: Path, paths: Sequence[Path]
) -> dict[tuple[str, int], list[tuple[int, int, int]]]:
    mapped: dict[tuple[str, int], list[tuple[int, int, int]]] = defaultdict(list)
    for path in paths:
        if path.name != "aligned_timeline.ndjson":
            continue
        for _coordinate, _physical_start, _physical_end, record in _records(path):
            if not isinstance(record, Mapping):
                continue
            source = record.get("source_artifact")
            source_record = _integer(record.get("source_record"))
            earliest = _integer(record.get("host_earliest_ns"))
            estimate = _integer(record.get("host_estimate_ns"))
            latest = _integer(record.get("host_latest_ns"))
            identity = (
                _source_identity(run_dir, path, source)
                if isinstance(source, str)
                else None
            )
            if (
                identity is not None
                and source_record is not None
                and earliest is not None
                and estimate is not None
                and latest is not None
            ):
                mapped[(identity, source_record)].append((earliest, estimate, latest))
    return mapped


def _load_clock_alignments(run_dir: Path) -> list[dict[str, Any]]:
    alignments: list[dict[str, Any]] = []
    for path in sorted(run_dir.rglob("clock_alignment.json")):
        relative = path.relative_to(run_dir)
        if INDEX_DIRECTORY in relative.parts or path.is_symlink():
            continue
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            continue
        if not isinstance(payload, dict):
            continue
        scope = relative.parent.as_posix()
        alignments.append(
            {
                "path": relative.as_posix(),
                "scope": "" if scope == "." else scope,
                "sha256": sha256_file(path),
                "document": payload,
            }
        )
    return alignments


def _alignment_for_path(
    relative_path: str, alignments: Sequence[Mapping[str, Any]]
) -> Mapping[str, Any] | None:
    parts = Path(relative_path).parts
    candidates = []
    for item in alignments:
        scope = str(item.get("scope") or "")
        scope_parts = Path(scope).parts if scope else ()
        if parts[: len(scope_parts)] == scope_parts:
            candidates.append((len(scope_parts), item))
    if not candidates:
        return alignments[0] if len(alignments) == 1 else None
    longest = max(length for length, _item in candidates)
    selected = [item for length, item in candidates if length == longest]
    return selected[0] if len(selected) == 1 else None


@lru_cache(maxsize=1)
def _dut_mapper() -> Any:
    sys.path.insert(0, str(BENCH_SCRIPTS))
    try:
        from clock_alignment import map_dut_timestamp
    finally:
        try:
            sys.path.remove(str(BENCH_SCRIPTS))
        except ValueError:
            pass
    return map_dut_timestamp


def _map_dut(
    alignment: Mapping[str, Any],
    segment: Any,
    dut_us: Any,
    *,
    segment_instance: int | None = None,
) -> dict[str, Any]:
    return _dut_mapper()(
        alignment, segment, dut_us, segment_instance=segment_instance
    )


def _host_interval(
    relative_path: str,
    physical_line: int,
    record: Mapping[str, Any],
    source_map: Mapping[tuple[str, int], Sequence[tuple[int, int, int]]],
    alignment: Mapping[str, Any] | None,
    raw_clocks: Sequence[Mapping[str, Any]] = (),
) -> tuple[int | None, int | None, int | None, list[str]]:
    earliest = _integer(record.get("host_earliest_ns"))
    estimate = _integer(record.get("host_estimate_ns"))
    latest = _integer(record.get("host_latest_ns"))
    limitations: list[str] = []
    if earliest is not None and estimate is not None and latest is not None:
        return earliest, estimate, latest, limitations
    anchored = [
        item
        for item in raw_clocks
        if _integer(item.get("host_earliest_ns")) is not None
        and _integer(item.get("host_estimate_ns")) is not None
        and _integer(item.get("host_latest_ns")) is not None
    ]
    if anchored:
        anchored_limitations = []
        if any(item.get("precision") == "millisecond" for item in anchored):
            anchored_limitations.append("source timestamp has millisecond precision")
        if len(anchored) > 1:
            anchored_limitations.append(
                "host interval covers multiple recorded clock anchors"
            )
        return (
            min(int(item["host_earliest_ns"]) for item in anchored),
            round(sum(int(item["host_estimate_ns"]) for item in anchored) / len(anchored)),
            max(int(item["host_latest_ns"]) for item in anchored),
            anchored_limitations,
        )
    linked = source_map.get((relative_path, physical_line), ())
    if linked:
        return (
            min(item[0] for item in linked),
            round(sum(item[1] for item in linked) / len(linked)),
            max(item[2] for item in linked),
            ["host interval combines aligned events emitted from this raw record"],
        )
    for field in (
        "host_monotonic_ns",
        "hostMonotonicNs",
        "requestedHostMonotonicNs",
        "host_capture_ns",
        "observed_host_ns",
    ):
        if (host_ns := _integer(record.get(field))) is not None:
            duration = max(0, _integer(record.get("duration_ns")) or 0)
            return host_ns, host_ns, host_ns + duration, limitations
    if alignment is not None:
        segment = _clock_segment(record)
        dut_us = _first(record, DUT_US_FIELDS)
        millisecond_precision = False
        if dut_us in (None, ""):
            dut_ms = _first(record, DUT_MS_FIELDS)
            parsed_ms = _number(dut_ms)
            if parsed_ms is not None:
                dut_us = round(parsed_ms * 1000)
                millisecond_precision = True
        mapped = _map_dut(
            alignment,
            segment,
            dut_us,
            segment_instance=_segment_instance(record),
        )
        if mapped.get("status") == "mapped":
            if millisecond_precision:
                limitations.append("source timestamp has millisecond precision")
            return (
                _integer(mapped.get("host_earliest_ns")),
                _integer(mapped.get("host_estimate_ns")),
                _integer(mapped.get("host_latest_ns")),
                limitations,
            )
    return None, None, None, ["host time unavailable"]


def _anchor_raw_clocks(
    relative_path: str,
    physical_line: int,
    record: Mapping[str, Any],
    clocks: Sequence[dict[str, Any]],
    source_map: Mapping[tuple[str, int], Sequence[tuple[int, int, int]]],
    alignment: Mapping[str, Any] | None,
) -> None:
    if all(
        _integer(record.get(field)) is not None
        for field in ("host_earliest_ns", "host_estimate_ns", "host_latest_ns")
    ):
        return
    linked = source_map.get((relative_path, physical_line), ())
    intended_host = _integer(record.get("intendedHostMonotonicNs"))
    requested_host = _integer(record.get("requestedHostMonotonicNs"))
    for clock in clocks:
        clock_name = str(clock.get("clock"))
        value = _number(clock.get("value"))
        anchor: tuple[int, int, int] | None = None
        method: str | None = None
        if value is None:
            continue
        if clock_name == "host_monotonic_ns":
            host_ns = round(value)
            anchor, method = (host_ns, host_ns, host_ns), "identity"
        elif clock_name == "replay_offset_s" and (
            intended_host is not None or requested_host is not None
        ):
            host_ns = intended_host if intended_host is not None else requested_host
            assert host_ns is not None
            anchor, method = (host_ns, host_ns, host_ns), (
                "recorded_intended_host" if intended_host is not None else "recorded_requested_host"
            )
        elif clock_name in {"dut_monotonic_us", "dut_monotonic_ms"} and alignment is not None:
            millisecond_precision = clock_name.endswith("_ms")
            dut_us = value * (1000 if millisecond_precision else 1)
            mapped_start = _map_dut(
                alignment,
                clock.get("clock_segment"),
                round(dut_us),
                segment_instance=_integer(clock.get("segment_instance")),
            )
            mapped_end = _map_dut(
                alignment,
                clock.get("clock_segment"),
                round(dut_us) + (999 if millisecond_precision else 0),
                segment_instance=_integer(clock.get("segment_instance")),
            )
            if (
                mapped_start.get("status") == "mapped"
                and mapped_end.get("status") == "mapped"
                and mapped_start.get("mapping_id") == mapped_end.get("mapping_id")
            ):
                anchor = (
                    int(mapped_start["host_earliest_ns"]),
                    round(
                        (
                            int(mapped_start["host_estimate_ns"])
                            + int(mapped_end["host_estimate_ns"])
                        )
                        / 2
                    ),
                    int(mapped_end["host_latest_ns"]),
                )
                method = "clock_alignment_affine"
                clock["mapping_id"] = mapped_start.get("mapping_id")
                if millisecond_precision:
                    clock["precision"] = "millisecond"
        elif linked:
            anchor = (
                min(item[0] for item in linked),
                round(sum(item[1] for item in linked) / len(linked)),
                max(item[2] for item in linked),
            )
            method = "aligned_timeline_source_join"
        if anchor is not None:
            clock.update(
                {
                    "host_earliest_ns": anchor[0],
                    "host_estimate_ns": anchor[1],
                    "host_latest_ns": anchor[2],
                    "mapping_method": method,
                }
            )


def _compress_ranges(values: Iterable[int]) -> list[list[int]]:
    ranges: list[list[int]] = []
    for value in sorted(set(values)):
        if ranges and value == ranges[-1][1] + 1:
            ranges[-1][1] = value
        else:
            ranges.append([value, value])
    return ranges


def _summary_fields(counter: Mapping[str, Mapping[str, Counter[str]]]) -> dict[str, Any]:
    return {
        kind: {
            "count": sum(fields.get("__record__", Counter()).values()),
            "fields": {
                field: {"types": dict(sorted(types.items())), "count": sum(types.values())}
                for field, types in sorted(fields.items())
                if field != "__record__"
            },
        }
        for kind, fields in sorted(counter.items())
    }


def build_run_index(run_dir: Path) -> dict[str, Any]:
    """Build all runner-owned finding-aid indexes below investigation_index/."""
    run_dir = run_dir.resolve()
    root = index_root(run_dir, create=True)
    text_paths, video_paths = _artifact_paths(run_dir)
    source_map = _timeline_source_map(run_dir, text_paths)
    alignments = _load_clock_alignments(run_dir)

    sys.path.insert(0, str(BENCH_SCRIPTS))
    try:
        from investigation_video import build_frame_index
    finally:
        try:
            sys.path.remove(str(BENCH_SCRIPTS))
        except ValueError:
            pass

    video_summaries: list[dict[str, Any]] = []
    for video in video_paths:
        relative = video.relative_to(run_dir).as_posix()
        digest = sha256_file(video)
        try:
            summary, _rows = build_frame_index(video, root, video_sha256=digest)
            video_summaries.append(
                {
                    "path": relative,
                    "sha256": digest,
                    "status": "complete",
                    "summary": summary,
                }
            )
        except Exception as error:
            video_summaries.append(
                {
                    "path": relative,
                    "sha256": digest,
                    "status": "failed",
                    "error": type(error).__name__,
                }
            )

    descriptor, temporary_name = tempfile.mkstemp(prefix=".records.", dir=root)
    temporary = Path(temporary_name)
    os.close(descriptor)
    global_dictionary: dict[str, dict[str, Counter[str]]] = defaultdict(
        lambda: defaultdict(Counter)
    )
    file_summaries: list[dict[str, Any]] = []
    clock_ranges: dict[str, list[float]] = defaultdict(list)
    try:
        with temporary.open("w", encoding="utf-8") as index_output:
            for path in text_paths:
                relative = path.relative_to(run_dir).as_posix()
                alignment_entry = _alignment_for_path(relative, alignments)
                alignment = (
                    alignment_entry.get("document")
                    if isinstance(alignment_entry, Mapping)
                    else None
                )
                digest = sha256_file(path)
                format_name = _format(path)
                dictionary: dict[str, dict[str, Counter[str]]] = defaultdict(
                    lambda: defaultdict(Counter)
                )
                buckets: dict[int, list[int]] = defaultdict(list)
                file_clock_ranges: dict[str, list[float]] = defaultdict(list)
                record_count = 0
                mapped_count = 0
                host_min: int | None = None
                host_max: int | None = None
                for coordinate, physical_start, physical_end, raw_record in _records(path):
                    semantic_record = (
                        raw_record
                        if isinstance(raw_record, Mapping)
                        else _log_record(str(raw_record))
                    )
                    kind = str(_sanitize(_kind(path, semantic_record), run_dir))
                    dictionary_record = _sanitize(
                        semantic_record, run_dir, mapping_keys=True
                    )
                    dictionary[kind]["__record__"]["count"] += 1
                    global_dictionary[kind]["__record__"]["count"] += 1
                    for field, value_type in _fields(dictionary_record):
                        dictionary[kind][field][value_type] += 1
                        global_dictionary[kind][field][value_type] += 1
                    raw_clocks = _raw_clocks(semantic_record)
                    _anchor_raw_clocks(
                        relative,
                        physical_start,
                        semantic_record,
                        raw_clocks,
                        source_map,
                        alignment if isinstance(alignment, Mapping) else None,
                    )
                    earliest, estimate, latest, limitations = _host_interval(
                        relative,
                        physical_start,
                        semantic_record,
                        source_map,
                        alignment if isinstance(alignment, Mapping) else None,
                        raw_clocks,
                    )
                    for clock in raw_clocks:
                        clock_name = str(clock["clock"])
                        clock_value = float(clock["value"])
                        clock_ranges[clock_name].append(clock_value)
                        file_clock_ranges[clock_name].append(clock_value)
                    index_record = {
                        "path": relative,
                        "sha256": digest,
                        "format": format_name,
                        "coordinate": coordinate,
                        "physical_line_start": physical_start,
                        "physical_line_end": physical_end,
                        "kind": kind,
                        "host_earliest_ns": earliest,
                        "host_estimate_ns": estimate,
                        "host_latest_ns": latest,
                        "raw_clocks": raw_clocks,
                        "limitations": limitations,
                    }
                    index_output.write(
                        json.dumps(index_record, sort_keys=True, separators=(",", ":"))
                        + "\n"
                    )
                    record_count += 1
                    if earliest is not None and latest is not None:
                        mapped_count += 1
                        host_min = earliest if host_min is None else min(host_min, earliest)
                        host_max = latest if host_max is None else max(host_max, latest)
                        first_bucket = earliest // TIME_BUCKET_NS
                        last_bucket = latest // TIME_BUCKET_NS
                        if last_bucket - first_bucket <= 1000:
                            for bucket in range(first_bucket, last_bucket + 1):
                                buckets[bucket].append(coordinate)
                file_summaries.append(
                    {
                        "path": relative,
                        "sha256": digest,
                        "format": format_name,
                        "coordinate_kind": "row" if format_name == "csv" else "line",
                        "record_count": record_count,
                        "host_time_mapped_count": mapped_count,
                        "host_time_range_ns": {
                            "earliest": host_min,
                            "latest": host_max,
                        },
                        "host_time_map": [
                            {
                                "bucket_start_ns": bucket * TIME_BUCKET_NS,
                                "bucket_end_ns": (bucket + 1) * TIME_BUCKET_NS,
                                "coordinate_ranges": _compress_ranges(coordinates),
                            }
                            for bucket, coordinates in sorted(buckets.items())
                        ],
                        "recorded_clocks": {
                            clock: {
                                "count": len(values),
                                "minimum": min(values),
                                "maximum": max(values),
                            }
                            for clock, values in sorted(file_clock_ranges.items())
                            if values
                        },
                        "kinds": _summary_fields(dictionary),
                    }
                )
            index_output.flush()
            os.fsync(index_output.fileno())
        record_index = root / RECORD_INDEX_FILENAME
        os.replace(temporary, record_index)
        manifest: dict[str, Any] = {
            "schema_version": INDEX_SCHEMA_VERSION,
            "kind": "bench_evidence_index",
            "algorithm": INDEX_ALGORITHM,
            "record_index_filename": RECORD_INDEX_FILENAME,
            "record_index_sha256": sha256_file(record_index),
            "files": file_summaries,
            "videos": video_summaries,
            "field_dictionary": _summary_fields(global_dictionary),
            "recorded_clocks": {
                clock: {
                    "count": len(values),
                    "minimum": min(values),
                    "maximum": max(values),
                }
                for clock, values in sorted(clock_ranges.items())
                if values
            },
            "clock_alignments": [
                {
                    "path": item["path"],
                    "scope": item["scope"],
                    "sha256": item["sha256"],
                    "status": "available",
                    "segments": item["document"].get("segments", []),
                }
                for item in alignments
            ],
        }
        _atomic_json(root / MANIFEST_FILENAME, manifest)
        return manifest
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def load_manifest(run_dir: Path) -> tuple[Path, dict[str, Any]]:
    root = index_root(run_dir.resolve(), create=False)
    try:
        manifest = json.loads((root / MANIFEST_FILENAME).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError("evidence index is unavailable; run build first") from error
    record_index = root / RECORD_INDEX_FILENAME
    if (
        not isinstance(manifest, dict)
        or manifest.get("schema_version") != INDEX_SCHEMA_VERSION
        or manifest.get("algorithm") != INDEX_ALGORITHM
        or manifest.get("record_index_filename") != RECORD_INDEX_FILENAME
        or not record_index.is_file()
        or sha256_file(record_index) != manifest.get("record_index_sha256")
    ):
        raise EvidenceError("evidence index validation failed; rebuild it")
    return root, manifest


def _validate_indexed_inputs(
    run_dir: Path,
    manifest: Mapping[str, Any],
    *,
    requested_paths: Sequence[str] = (),
    include_videos: bool = False,
) -> None:
    indexed = {
        str(item["path"]): str(item["sha256"])
        for item in manifest.get("files", [])
        if isinstance(item, Mapping)
        and isinstance(item.get("path"), str)
        and isinstance(item.get("sha256"), str)
    }
    if include_videos:
        indexed.update(
            {
                str(item["path"]): str(item["sha256"])
                for item in manifest.get("videos", [])
                if isinstance(item, Mapping)
                and isinstance(item.get("path"), str)
                and isinstance(item.get("sha256"), str)
            }
        )
    selected = set(requested_paths) if requested_paths else set(indexed)
    unknown = selected.difference(indexed)
    if unknown:
        raise EvidenceError(f"artifact is not indexed: {sorted(unknown)[0]}")
    for relative in sorted(selected):
        path = _relative_file(run_dir, relative)
        if sha256_file(path) != indexed[relative]:
            raise EvidenceError(f"indexed artifact changed: {relative}")


def _validate_clock_alignments(
    run_dir: Path,
    manifest: Mapping[str, Any],
    paths: Sequence[str] = (),
) -> None:
    items = [
        item
        for item in manifest.get("clock_alignments", [])
        if isinstance(item, Mapping) and isinstance(item.get("path"), str)
    ]
    if paths:
        selected = {
            str(item["path"]): item
            for path in paths
            if isinstance((item := _alignment_for_path(path, items)), Mapping)
        }
    else:
        selected = {str(item["path"]): item for item in items}
    for relative, item in sorted(selected.items()):
        alignment = _relative_file(run_dir, relative)
        if sha256_file(alignment) != item.get("sha256"):
            raise EvidenceError("indexed clock alignment changed; rebuild the index")


def compact_manifest(manifest: Mapping[str, Any]) -> dict[str, Any]:
    videos: list[dict[str, Any]] = []
    for item in manifest.get("videos", []):
        if not isinstance(item, Mapping):
            continue
        summary = item.get("summary")
        videos.append(
            {
                "path": item.get("path"),
                "sha256": item.get("sha256"),
                "status": item.get("status"),
                "frame_index": (
                    {
                        key: summary.get(key)
                        for key in (
                            "frame_count",
                            "first_frame_index",
                            "last_frame_index",
                            "frame_indices_contiguous",
                            "first_pts_seconds",
                            "last_pts_seconds",
                            "source_time_base",
                            "native_frame_rate_fps",
                            "score_distribution",
                            "top_change_windows",
                        )
                    }
                    if isinstance(summary, Mapping)
                    else None
                ),
                "error": item.get("error"),
            }
        )
    compact_files = [
        {
            **{
                key: item.get(key)
                for key in (
                    "path",
                    "sha256",
                    "format",
                    "coordinate_kind",
                    "record_count",
                    "host_time_mapped_count",
                    "host_time_range_ns",
                    "recorded_clocks",
                )
            },
            "kinds": {
                kind: {"count": details.get("count")}
                for kind, details in item.get("kinds", {}).items()
                if isinstance(details, Mapping)
            },
        }
        for item in manifest.get("files", [])
        if isinstance(item, Mapping)
    ]
    compact_alignments = [
        {
            "path": alignment.get("path"),
            "scope": alignment.get("scope"),
            "sha256": alignment.get("sha256"),
            "status": alignment.get("status"),
            "segments": [
                {
                    key: segment.get(key)
                    for key in (
                        "mapping_id",
                        "clock_segment",
                        "segment_instance",
                        "fit_status",
                        "fit_type",
                        "fit_quality",
                        "poor_fit",
                        "validity_dut_us",
                        "uncertainty_width_ns",
                    )
                }
                for segment in alignment.get("segments", [])
                if isinstance(segment, Mapping)
            ],
        }
        for alignment in manifest.get("clock_alignments", [])
        if isinstance(alignment, Mapping)
    ]
    return {
        "schema_version": manifest.get("schema_version"),
        "files": compact_files,
        "videos": videos,
        "field_dictionary": manifest.get("field_dictionary", {}),
        "recorded_clocks": manifest.get("recorded_clocks", {}),
        "clock_alignments": compact_alignments,
    }


def _parse_predicate(raw: str) -> tuple[str, Any]:
    field, separator, value = raw.partition("=")
    if not separator or not field:
        raise EvidenceError("invalid predicate")
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        parsed = value
    return field, parsed


def _parse_comparison(raw: str) -> tuple[str, str, str]:
    for operator in ("!=", "<=", ">=", "==", "<", ">"):
        left, separator, right = raw.partition(operator)
        if not separator:
            continue
        if not left or not right or not all(
            re.fullmatch(r"[A-Za-z0-9_.\[\]-]+", field)
            for field in (left, right)
        ):
            break
        return left, operator, right
    raise EvidenceError("invalid field comparison")


def _nested_values(value: Any, path: str) -> list[Any]:
    components = path.replace("[]", ".[]").split(".")
    current = [value]
    for component in components:
        if not component:
            continue
        next_values: list[Any] = []
        for item in current:
            if component == "[]" and isinstance(item, list):
                next_values.extend(item)
            elif isinstance(item, Mapping) and component in item:
                next_values.append(item[component])
        current = next_values
    return current


def _equivalent(actual: Any, expected: Any) -> bool:
    if actual == expected and isinstance(actual, bool) == isinstance(expected, bool):
        return True
    actual_number = _number(actual)
    expected_number = _number(expected)
    return (
        actual_number is not None
        and expected_number is not None
        and actual_number == expected_number
    )


def _predicate_matches(record: Any, predicates: Sequence[tuple[str, Any]]) -> bool:
    semantic = record if isinstance(record, Mapping) else _log_record(str(record))

    return all(
        any(_equivalent(actual, expected) for actual in _nested_values(semantic, field))
        for field, expected in predicates
    )


def _comparison_matches(
    record: Any, comparisons: Sequence[tuple[str, str, str]]
) -> bool:
    semantic = record if isinstance(record, Mapping) else _log_record(str(record))

    def compare(actual: Any, operator: str, expected: Any) -> bool:
        if operator == "==":
            return _equivalent(actual, expected)
        if operator == "!=":
            return not _equivalent(actual, expected)
        actual_number = _number(actual)
        expected_number = _number(expected)
        if actual_number is None or expected_number is None:
            return False
        if operator == "<":
            return actual_number < expected_number
        if operator == "<=":
            return actual_number <= expected_number
        if operator == ">":
            return actual_number > expected_number
        return actual_number >= expected_number

    return all(
        any(
            compare(actual, operator, expected)
            for actual in _nested_values(semantic, left)
            for expected in _nested_values(semantic, right)
        )
        for left, operator, right in comparisons
    )


def _record_at(path: Path, format_name: str, coordinate: int) -> Any:
    for current, _physical_start, _physical_end, record in _records(path):
        if current == coordinate:
            return record
        if current > coordinate:
            break
    raise EvidenceError("indexed record coordinate no longer resolves")


@lru_cache(maxsize=1)
def _privacy_sanitizer() -> Any:
    sys.path.insert(0, str(BENCH_SCRIPTS))
    try:
        from artifact_privacy import sanitize_artifact_value
    finally:
        try:
            sys.path.remove(str(BENCH_SCRIPTS))
        except ValueError:
            pass
    return sanitize_artifact_value


def _sanitize(value: Any, run_dir: Path, *, mapping_keys: bool = False) -> Any:
    return _privacy_sanitizer()(
        value, run_dir=run_dir, sanitize_mapping_keys=mapping_keys
    )


def _safe_query_field(field: str, run_dir: Path) -> str:
    sanitized = _sanitize({field: True}, run_dir, mapping_keys=True)
    if isinstance(sanitized, Mapping) and len(sanitized) == 1:
        return str(next(iter(sanitized)))
    return "<redacted-field>"


def _bounded_value(value: Any) -> Any:
    if isinstance(value, str):
        return value if len(value) <= MAX_STRING_LENGTH else value[:MAX_STRING_LENGTH] + "<truncated>"
    if isinstance(value, list):
        bounded = [_bounded_value(item) for item in value[:256]]
        if len(value) > 256:
            bounded.append(
                {"_truncated_items": len(value) - 256, "_original_count": len(value)}
            )
        return bounded
    if isinstance(value, Mapping):
        return {str(key): _bounded_value(item) for key, item in value.items()}
    return value


def _selector(index_record: Mapping[str, Any], description: str) -> dict[str, Any]:
    selector = {
        "kind": (
            "csv"
            if index_record["format"] == "csv"
            else "ndjson" if index_record["format"] == "ndjson" else "log"
        ),
        "path": index_record["path"],
        "sha256": index_record["sha256"],
        "description": description,
    }
    coordinate = int(index_record["coordinate"])
    if selector["kind"] == "csv":
        selector.update({"row_start": coordinate, "row_end": coordinate})
    else:
        selector.update({"line_start": coordinate, "line_end": coordinate})
    return selector


_SELECTOR_KEY_FIELDS = (
    "record_id",
    "trace_seq",
    "commit_seq",
    "event_seq",
    "stimulusSequence",
    "stimulus_sequence",
    "seq",
    "sourceIndex",
    "source_index",
    "source_record",
    "state",
    "kind",
    "stage",
    "metric",
    "sample",
)


def _stable_selector_key(raw_record: Any, sanitized_record: Any) -> str | None:
    """Return one privacy-safe top-level identity that resolves in the raw record."""
    if not isinstance(raw_record, Mapping) or not isinstance(sanitized_record, Mapping):
        return None
    for field in _SELECTOR_KEY_FIELDS:
        raw_value = raw_record.get(field)
        sanitized_value = sanitized_record.get(field)
        if (
            raw_value in (None, "")
            or sanitized_value in (None, "")
            or isinstance(raw_value, (Mapping, list))
            or isinstance(sanitized_value, (Mapping, list))
        ):
            continue
        raw_text = str(raw_value)
        if (
            raw_text != str(sanitized_value)
            or len(raw_text) > 128
            or "\n" in raw_text
            or "\r" in raw_text
        ):
            continue
        return f"{field}={raw_text}"
    return None


def _clock_pairs(
    root: Path, clock: str, paths: Sequence[str]
) -> list[dict[str, Any]]:
    selected_paths = set(paths)
    pairs: list[dict[str, Any]] = []
    with (root / RECORD_INDEX_FILENAME).open("r", encoding="utf-8") as handle:
        for line in handle:
            row = json.loads(line)
            if selected_paths and row.get("path") not in selected_paths:
                continue
            earliest = _integer(row.get("host_earliest_ns"))
            estimate = _integer(row.get("host_estimate_ns"))
            latest = _integer(row.get("host_latest_ns"))
            if earliest is None or estimate is None or latest is None:
                continue
            for raw in row.get("raw_clocks", []):
                if isinstance(raw, Mapping) and raw.get("clock") == clock:
                    value = _number(raw.get("value"))
                    if value is not None:
                        raw_earliest = _integer(raw.get("host_earliest_ns"))
                        raw_estimate = _integer(raw.get("host_estimate_ns"))
                        raw_latest = _integer(raw.get("host_latest_ns"))
                        pairs.append(
                            {
                                "value": value,
                                "earliest": raw_earliest
                                if raw_earliest is not None
                                else earliest,
                                "estimate": raw_estimate
                                if raw_estimate is not None
                                else estimate,
                                "latest": raw_latest
                                if raw_latest is not None
                                else latest,
                                "selector": _selector(
                                    row, "Raw recorded-clock mapping anchor"
                                ),
                            }
                        )
    return sorted(pairs, key=lambda item: (item["value"], item["estimate"]))


def _observed_clock_point(
    pairs: Sequence[Mapping[str, Any]], value: float
) -> dict[str, Any]:
    grouped: dict[float, list[Mapping[str, Any]]] = defaultdict(list)
    for item in pairs:
        grouped[float(item["value"])].append(item)
    nodes: list[dict[str, Any]] = []
    for raw_value, items in sorted(grouped.items()):
        if max(int(item["earliest"]) for item in items) > min(
            int(item["latest"]) for item in items
        ):
            raise EvidenceError(
                "recorded clock mapping is ambiguous; narrow the query with --path"
            )
        nodes.append(
            {
                "value": raw_value,
                "earliest": min(int(item["earliest"]) for item in items),
                "estimate": round(
                    sum(int(item["estimate"]) for item in items) / len(items)
                ),
                "latest": max(int(item["latest"]) for item in items),
                "evidence": [item["selector"] for item in items[:4]],
            }
        )
    raw_values = [float(item["value"]) for item in nodes]
    position = bisect_left(raw_values, value)
    if position < len(nodes) and math.isclose(
        raw_values[position], value, rel_tol=0, abs_tol=1e-12
    ):
        return nodes[position]
    if position == 0 or position == len(nodes):
        raise EvidenceError("requested clock window is outside recorded mapping evidence")
    left, right = nodes[position - 1], nodes[position]
    fraction = (value - float(left["value"])) / (
        float(right["value"]) - float(left["value"])
    )
    estimate = round(
        int(left["estimate"])
        + fraction * (int(right["estimate"]) - int(left["estimate"]))
    )
    earliest_error = min(
        int(left["earliest"]) - int(left["estimate"]),
        int(right["earliest"]) - int(right["estimate"]),
    )
    latest_error = max(
        int(left["latest"]) - int(left["estimate"]),
        int(right["latest"]) - int(right["estimate"]),
    )
    return {
        "value": value,
        "earliest": estimate + earliest_error,
        "estimate": estimate,
        "latest": estimate + latest_error,
        "evidence": [*left["evidence"], *right["evidence"]],
    }


def convert_window(
    run_dir: Path,
    root: Path,
    manifest: Mapping[str, Any],
    *,
    clock: str,
    start: float,
    end: float,
    clock_segment: str | None,
    segment_instance: int | None = None,
    paths: Sequence[str] = (),
) -> dict[str, Any]:
    if not math.isfinite(start) or not math.isfinite(end) or start > end:
        raise EvidenceError("time window is invalid")
    normalized = clock.casefold()
    if normalized in {"host_ns", "host_monotonic_ns"}:
        earliest, latest = round(start), round(end)
        return {
            "status": "mapped",
            "input_clock": clock,
            "input_start": start,
            "input_end": end,
            "method": "identity",
            "host_earliest_ns": earliest,
            "host_estimate_start_ns": earliest,
            "host_estimate_end_ns": latest,
            "host_latest_ns": latest,
            "uncertainty_ns": 0,
            "limitations": [],
            "evidence": [],
        }
    if normalized in {"host_s", "host_monotonic_s"}:
        return convert_window(
            run_dir,
            root,
            manifest,
            clock="host_monotonic_ns",
            start=start * 1_000_000_000,
            end=end * 1_000_000_000,
            clock_segment=clock_segment,
            segment_instance=segment_instance,
            paths=paths,
        ) | {"input_clock": clock, "input_start": start, "input_end": end}
    if normalized in {"dut_us", "dut_monotonic_us", "dut_ms", "dut_monotonic_ms"}:
        alignment_items = [
            item
            for item in manifest.get("clock_alignments", [])
            if isinstance(item, Mapping)
        ]
        selected_alignments: dict[str, Mapping[str, Any]] = {}
        if paths:
            for path in paths:
                selected = _alignment_for_path(path, alignment_items)
                if not isinstance(selected, Mapping) or not isinstance(
                    selected.get("path"), str
                ):
                    raise EvidenceError(
                        f"clock alignment evidence is unavailable for: {path}"
                    )
                selected_alignments[str(selected["path"])] = selected
        elif len(alignment_items) == 1:
            selected_alignments[str(alignment_items[0].get("path"))] = alignment_items[0]
        if len(selected_alignments) != 1:
            raise EvidenceError(
                "DUT clock mapping is suite-ambiguous; narrow the query with --path"
            )
        alignment_info = next(iter(selected_alignments.values()))
        alignment_path = alignment_info.get("path")
        if not isinstance(alignment_path, str):
            raise EvidenceError("clock alignment evidence is unavailable")
        alignment_file = _relative_file(run_dir, alignment_path)
        if sha256_file(alignment_file) != alignment_info.get("sha256"):
            raise EvidenceError("indexed clock alignment changed; rebuild the index")
        alignment = json.loads(alignment_file.read_text(encoding="utf-8"))
        if clock_segment is None:
            segments = alignment.get("segments", [])
            available = {
                str(item.get("clock_segment"))
                for item in segments
                if isinstance(item, Mapping) and item.get("fit_status") == "fitted"
            }
            if len(available) != 1:
                raise EvidenceError("--clock-segment is required for this DUT mapping")
            clock_segment = next(iter(available))
        millisecond_input = normalized.endswith("_ms") or normalized == "dut_ms"
        multiplier = 1000 if millisecond_input else 1
        start_dut_us = round(start * multiplier)
        end_dut_us = round(end * multiplier) + (999 if millisecond_input else 0)
        mapped_start = _map_dut(
            alignment,
            clock_segment,
            start_dut_us,
            segment_instance=segment_instance,
        )
        mapped_end = _map_dut(
            alignment,
            clock_segment,
            end_dut_us,
            segment_instance=segment_instance,
        )
        if mapped_start.get("status") != "mapped" or mapped_end.get("status") != "mapped":
            raise EvidenceError(
                "DUT window does not map: "
                f"{mapped_start.get('status')}/{mapped_end.get('status')}"
            )
        if mapped_start.get("mapping_id") != mapped_end.get("mapping_id"):
            raise EvidenceError("DUT window crosses clock mapping instances")
        earliest = int(mapped_start["host_earliest_ns"])
        latest = int(mapped_end["host_latest_ns"])
        limitations = list(
            dict.fromkeys(
                [
                    *(("input has millisecond precision",) if millisecond_input else ()),
                    *(("clock fit is poor",) if mapped_start.get("poor_fit") or mapped_end.get("poor_fit") else ()),
                ]
            )
        )
        mapping_index = next(
            (
                index
                for index, item in enumerate(alignment.get("segments", []))
                if isinstance(item, Mapping)
                and item.get("mapping_id") == mapped_start.get("mapping_id")
            ),
            None,
        )
        start_uncertainty = int(mapped_start["host_latest_ns"]) - int(
            mapped_start["host_earliest_ns"]
        )
        end_uncertainty = int(mapped_end["host_latest_ns"]) - int(
            mapped_end["host_earliest_ns"]
        )
        return {
            "status": "mapped",
            "input_clock": clock,
            "input_start": start,
            "input_end": end,
            "method": "clock_alignment_affine",
            "mapping_id": mapped_start.get("mapping_id"),
            "segment_instance": mapped_start.get("segment_instance"),
            "host_earliest_ns": earliest,
            "host_estimate_start_ns": mapped_start.get("host_estimate_ns"),
            "host_estimate_end_ns": mapped_end.get("host_estimate_ns"),
            "host_latest_ns": latest,
            "uncertainty_ns": max(start_uncertainty, end_uncertainty),
            "endpoint_uncertainty_ns": {
                "start": start_uncertainty,
                "end": end_uncertainty,
            },
            "limitations": limitations,
            "evidence": [
                {
                    "kind": "json_pointer",
                    "path": alignment_path,
                    "sha256": alignment_info.get("sha256"),
                    "description": "Recorded DUT-to-host clock alignment",
                    "json_pointer": (
                        f"/segments/{mapping_index}" if mapping_index is not None else ""
                    ),
                }
            ],
        }
    pairs = _clock_pairs(root, clock, paths)
    if not pairs:
        raise EvidenceError(f"recorded clock cannot be mapped: {clock}")
    start_interval = _observed_clock_point(pairs, start)
    end_interval = _observed_clock_point(pairs, end)
    return {
        "status": "mapped",
        "input_clock": clock,
        "input_start": start,
        "input_end": end,
        "method": "recorded_piecewise_linear",
        "host_earliest_ns": start_interval["earliest"],
        "host_estimate_start_ns": start_interval["estimate"],
        "host_estimate_end_ns": end_interval["estimate"],
        "host_latest_ns": end_interval["latest"],
        "uncertainty_ns": max(
            int(start_interval["latest"]) - int(start_interval["earliest"]),
            int(end_interval["latest"]) - int(end_interval["earliest"]),
        ),
        "endpoint_uncertainty_ns": {
            "start": int(start_interval["latest"])
            - int(start_interval["earliest"]),
            "end": int(end_interval["latest"]) - int(end_interval["earliest"]),
        },
        "limitations": ["conversion is bounded by adjacent recorded clock samples"],
        "evidence": list(
            {
                json.dumps(item, sort_keys=True): item
                for item in [
                    *start_interval["evidence"],
                    *end_interval["evidence"],
                ]
            }.values()
        )[:8],
    }


def query_records(args: argparse.Namespace) -> dict[str, Any]:
    run_dir = Path(args.run_dir).resolve()
    root, manifest = load_manifest(run_dir)
    predicates = [_parse_predicate(raw) for raw in args.where]
    comparisons = [_parse_comparison(raw) for raw in args.compare]
    paths = set(args.path)
    kinds = set(args.kind)
    if args.context and not paths:
        raise EvidenceError("--context requires at least one --path")
    _validate_indexed_inputs(
        run_dir, manifest, requested_paths=tuple(sorted(paths))
    )
    if paths:
        _validate_indexed_inputs(run_dir, manifest)
    _validate_clock_alignments(run_dir, manifest, tuple(sorted(paths)))
    conversion: dict[str, Any] | None = None
    if (args.start is None) != (args.end is None):
        raise EvidenceError("--start and --end must be supplied together")
    if args.start is not None:
        conversion = convert_window(
            run_dir,
            root,
            manifest,
            clock=args.clock,
            start=args.start,
            end=args.end,
            clock_segment=args.clock_segment,
            segment_instance=getattr(args, "segment_instance", None),
            paths=tuple(paths),
        )
    indexed_files = {
        item["path"]: item
        for item in manifest.get("files", [])
        if isinstance(item, Mapping) and isinstance(item.get("path"), str)
    }
    preliminary: list[dict[str, Any]] = []
    context_by_path: dict[str, list[dict[str, Any]]] = defaultdict(list)
    context_order: dict[tuple[str, int], int] = {}
    with (root / RECORD_INDEX_FILENAME).open("r", encoding="utf-8") as handle:
        for order, line in enumerate(handle):
            index_record = json.loads(line)
            if paths and index_record.get("path") not in paths:
                continue
            if args.context:
                relative = str(index_record["path"])
                context_by_path[relative].append(index_record)
                context_order[(relative, int(index_record["coordinate"]))] = order
            if kinds and index_record.get("kind") not in kinds:
                continue
            if conversion is not None:
                earliest = _integer(index_record.get("host_earliest_ns"))
                latest = _integer(index_record.get("host_latest_ns"))
                if (
                    earliest is None
                    or latest is None
                    or earliest > conversion["host_latest_ns"]
                    or latest < conversion["host_earliest_ns"]
                ):
                    continue
            preliminary.append(index_record)

    by_path: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for index_record in preliminary:
        by_path[str(index_record["path"])].append(index_record)
    for relative, indexed in by_path.items():
        summary = indexed_files.get(relative)
        path = _relative_file(run_dir, relative)
        if summary is None or sha256_file(path) != indexed[0].get("sha256"):
            raise EvidenceError(f"indexed artifact changed: {relative}")

    if predicates or comparisons:
        matching_keys: set[tuple[str, int]] = set()
        for relative, indexed in by_path.items():
            wanted = {int(item["coordinate"]) for item in indexed}
            path = _relative_file(run_dir, relative)
            for coordinate, _physical_start, _physical_end, raw_record in _records(path):
                if (
                    coordinate in wanted
                    and _predicate_matches(raw_record, predicates)
                    and _comparison_matches(raw_record, comparisons)
                ):
                    matching_keys.add((relative, coordinate))
        matched_index = [
            item
            for item in preliminary
            if (str(item["path"]), int(item["coordinate"])) in matching_keys
        ]
    else:
        matched_index = preliminary

    total = len(matched_index)
    start_offset = min(args.offset, total)
    selected_matches = matched_index[start_offset : start_offset + args.limit]
    selected_match_keys = {
        (str(item["path"]), int(item["coordinate"])) for item in selected_matches
    }
    if args.context:
        expanded: dict[tuple[str, int], dict[str, Any]] = {}
        positions_by_path = {
            relative: {
                int(item["coordinate"]): index for index, item in enumerate(indexed)
            }
            for relative, indexed in context_by_path.items()
        }
        for item in selected_matches:
            relative = str(item["path"])
            coordinate = int(item["coordinate"])
            position = positions_by_path[relative][coordinate]
            indexed = context_by_path[relative]
            start = max(0, position - args.context)
            end = min(len(indexed), position + args.context + 1)
            for neighbor in indexed[start:end]:
                expanded[(relative, int(neighbor["coordinate"]))] = neighbor
        returned_index = sorted(
            expanded.values(),
            key=lambda item: context_order[
                (str(item["path"]), int(item["coordinate"]))
            ],
        )
    else:
        returned_index = selected_matches
    returned_by_path: dict[str, set[int]] = defaultdict(set)
    for item in returned_index:
        returned_by_path[str(item["path"])].add(int(item["coordinate"]))
    raw_by_key: dict[tuple[str, int], Any] = {}
    for relative, wanted in returned_by_path.items():
        path = _relative_file(run_dir, relative)
        for coordinate, _physical_start, _physical_end, raw_record in _records(path):
            if coordinate in wanted:
                raw_by_key[(relative, coordinate)] = raw_record
                if len(raw_by_key) == sum(len(values) for values in returned_by_path.values()):
                    break
    returned: list[dict[str, Any]] = []
    for index_record in returned_index:
        key = (str(index_record["path"]), int(index_record["coordinate"]))
        if key not in raw_by_key:
            raise EvidenceError("indexed record coordinate no longer resolves")
        raw_record = raw_by_key[key]
        sanitized_record = _sanitize(raw_record, run_dir, mapping_keys=True)
        selector = _selector(index_record, "Record returned by bounded evidence query")
        if stable_key := _stable_selector_key(raw_record, sanitized_record):
            selector["keys"] = [stable_key]
        returned.append(
            {
                "selector": selector,
                "physical_line_start": index_record["physical_line_start"],
                "physical_line_end": index_record["physical_line_end"],
                "kind": index_record["kind"],
                "selection_role": "match" if key in selected_match_keys else "context",
                "host_time": {
                    "earliest_ns": index_record["host_earliest_ns"],
                    "estimate_ns": index_record["host_estimate_ns"],
                    "latest_ns": index_record["host_latest_ns"],
                    "limitations": index_record.get("limitations", []),
                },
                "record": _bounded_value(sanitized_record),
            }
        )
    return {
        "schema_version": 1,
        "kind": "bench_evidence_query",
        "query": {
            "paths": sorted(paths),
            "kinds": sorted(kinds),
            "predicate_count": len(predicates),
            "predicate_fields": [
                _safe_query_field(field, run_dir) for field, _value in predicates
            ],
            "field_comparisons": [
                (
                    f"{_safe_query_field(left, run_dir)}{operator}"
                    f"{_safe_query_field(right, run_dir)}"
                )
                for left, operator, right in comparisons
            ],
            "adjacent_context_records": args.context,
            "clock_conversion": conversion,
        },
        "total_matches": total,
        "offset": start_offset,
        "returned_match_count": len(selected_matches),
        "returned_count": len(returned),
        "next_offset": (
            start_offset + len(selected_matches)
            if start_offset + len(selected_matches) < total
            else None
        ),
        "truncated": start_offset + len(selected_matches) < total,
        "results": returned,
    }


def query_frames(args: argparse.Namespace) -> dict[str, Any]:
    run_dir = Path(args.run_dir).resolve()
    root, manifest = load_manifest(run_dir)
    video = next(
        (
            item
            for item in manifest.get("videos", [])
            if isinstance(item, Mapping) and item.get("path") == args.path
        ),
        None,
    )
    if not isinstance(video, Mapping) or video.get("status") != "complete":
        raise EvidenceError("video frame index is unavailable")
    source = _relative_file(run_dir, args.path)
    if sha256_file(source) != video.get("sha256"):
        raise EvidenceError("indexed video changed")
    summary = video.get("summary")
    if not isinstance(summary, Mapping):
        raise EvidenceError("video frame summary is unavailable")
    frame_path = root / str(summary.get("frame_index_filename"))
    if not frame_path.is_file() or sha256_file(frame_path) != summary.get("frame_index_sha256"):
        raise EvidenceError("video frame index validation failed")
    matches: list[dict[str, Any]] = []
    with frame_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            row = json.loads(line)
            pts = float(row["pts_seconds"])
            score = float(row["change_score"])
            if args.start is not None and pts < args.start:
                continue
            if args.end is not None and pts > args.end:
                continue
            if args.min_score is not None and score < args.min_score:
                continue
            matches.append(row)
    if args.order == "score":
        matches.sort(key=lambda row: (-float(row["change_score"]), int(row["frame_index"])))
    total = len(matches)
    returned = matches[args.offset : args.offset + args.limit]
    return {
        "schema_version": 1,
        "kind": "bench_frame_index_query",
        "finding_aid_only": True,
        "source_video_path": args.path,
        "source_video_sha256": video.get("sha256"),
        "total_matches": total,
        "offset": min(args.offset, total),
        "returned_count": len(returned),
        "next_offset": args.offset + len(returned) if args.offset + len(returned) < total else None,
        "truncated": args.offset + len(returned) < total,
        "results": returned,
    }


def source_lines(args: argparse.Namespace) -> dict[str, Any]:
    path = Path(args.path)
    if (
        path.is_absolute()
        or ".." in path.parts
        or not args.path
        or args.path.startswith("-")
        or not args.revision
        or args.revision.startswith("-")
        or not re.fullmatch(r"[A-Za-z0-9._/-]{1,128}", args.revision)
    ):
        raise EvidenceError("source path must be repository-relative")
    if args.line_end - args.line_start + 1 > MAX_SOURCE_LINES:
        raise EvidenceError(f"source range exceeds {MAX_SOURCE_LINES} lines")
    revision_process = subprocess.run(
        ["git", "rev-parse", "--verify", f"{args.revision}^{{commit}}"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if revision_process.returncode:
        raise EvidenceError("revision or source path does not resolve")
    try:
        revision = revision_process.stdout.decode("ascii").strip()
    except UnicodeDecodeError as error:
        raise EvidenceError("revision or source path does not resolve") from error
    if not re.fullmatch(r"[0-9a-f]{40,64}", revision):
        raise EvidenceError("revision or source path does not resolve")
    process = subprocess.run(
        ["git", "show", f"{revision}:{args.path}"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode:
        raise EvidenceError("revision or source path does not resolve")
    try:
        content = process.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise EvidenceError("source file is not UTF-8 text") from error
    lines = content.splitlines()
    if args.line_start < 1 or args.line_end < args.line_start or args.line_end > len(lines):
        raise EvidenceError("source line range does not resolve")
    selected = lines[args.line_start - 1 : args.line_end]
    normalized = "\n".join(selected) + "\n"
    selection_sha256 = sha256_bytes(normalized.encode("utf-8"))
    code_selector_basis = {
        "revision": revision,
        "path": args.path,
        "line_start": args.line_start,
        "line_end": args.line_end,
        "selection_sha256": selection_sha256,
    }
    return {
        "schema_version": 1,
        "kind": "bench_source_query",
        "revision": revision,
        "path": args.path,
        "line_start": args.line_start,
        "line_end": args.line_end,
        "selection_sha256": selection_sha256,
        "code_selector_basis": code_selector_basis,
        "lines": [
            {"line_number": number, "text": lines[number - 1]}
            for number in range(args.line_start, args.line_end + 1)
        ],
    }


def list_evidence(args: argparse.Namespace) -> dict[str, Any]:
    run_dir = Path(args.run_dir).resolve()
    _root, manifest = load_manifest(run_dir)
    _validate_indexed_inputs(
        run_dir,
        manifest,
        requested_paths=tuple(args.path),
        include_videos=True,
    )
    _validate_clock_alignments(run_dir, manifest, tuple(args.path))
    summary = compact_manifest(manifest)
    if args.path:
        selected = set(args.path)
        summary["files"] = [item for item in summary["files"] if item.get("path") in selected]
        summary["videos"] = [item for item in summary["videos"] if item.get("path") in selected]
    if args.kind:
        selected_kinds = set(args.kind)
        summary["field_dictionary"] = {
            key: value
            for key, value in summary["field_dictionary"].items()
            if key in selected_kinds
        }
    return {"schema_version": 1, "kind": "bench_evidence_catalog", **summary}


def _emit(payload: Mapping[str, Any]) -> None:
    encoded = (json.dumps(payload, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")
    if len(encoded) > MAX_STDOUT_BYTES:
        raise EvidenceError(
            f"output exceeds {MAX_STDOUT_BYTES} bytes; narrow the query or lower --limit"
        )
    sys.stdout.buffer.write(encoded)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    build = subparsers.add_parser("build", help="Build runner-owned indexes")
    build.add_argument("run_dir")

    listing = subparsers.add_parser("list", help="List indexed kinds, fields, and time ranges")
    listing.add_argument("run_dir")
    listing.add_argument("--path", action="append", default=[])
    listing.add_argument("--kind", action="append", default=[])

    records = subparsers.add_parser("records", help="Return bounded raw artifact records")
    records.add_argument("run_dir")
    records.add_argument("--path", action="append", default=[])
    records.add_argument("--kind", action="append", default=[])
    records.add_argument("--where", action="append", default=[])
    records.add_argument(
        "--compare",
        action="append",
        default=[],
        help="Require a scalar field comparison such as left!=right or start>end",
    )
    records.add_argument("--clock", default="host_monotonic_ns")
    records.add_argument("--clock-segment")
    records.add_argument("--segment-instance", type=int)
    records.add_argument("--start", type=float)
    records.add_argument("--end", type=float)
    records.add_argument("--offset", type=int, default=0)
    records.add_argument("--limit", type=int, default=DEFAULT_RESULT_LIMIT)
    records.add_argument(
        "--context",
        type=int,
        default=0,
        help="Include up to N adjacent raw records on each side of every match",
    )

    frames = subparsers.add_parser("frames", help="Query exhaustive native-frame indexes")
    frames.add_argument("run_dir")
    frames.add_argument("--path", required=True)
    frames.add_argument("--start", type=float)
    frames.add_argument("--end", type=float)
    frames.add_argument("--min-score", type=float)
    frames.add_argument("--order", choices=("time", "score"), default="time")
    frames.add_argument("--offset", type=int, default=0)
    frames.add_argument("--limit", type=int, default=DEFAULT_RESULT_LIMIT)

    source = subparsers.add_parser("source", help="Read exact source at a Git revision")
    source.add_argument("--revision", required=True)
    source.add_argument("--path", required=True)
    source.add_argument("--line-start", type=int, required=True)
    source.add_argument("--line-end", type=int, required=True)

    args = parser.parse_args(argv)
    if hasattr(args, "limit") and not 1 <= args.limit <= MAX_RESULT_LIMIT:
        parser.error(f"--limit must be between 1 and {MAX_RESULT_LIMIT}")
    if hasattr(args, "offset") and args.offset < 0:
        parser.error("--offset must be non-negative")
    if hasattr(args, "context") and not 0 <= args.context <= MAX_CONTEXT_RECORDS:
        parser.error(f"--context must be between 0 and {MAX_CONTEXT_RECORDS}")
    if (
        hasattr(args, "context")
        and hasattr(args, "limit")
        and args.limit * (2 * args.context + 1) > MAX_RESULT_LIMIT
    ):
        parser.error(
            f"--limit and --context may select at most {MAX_RESULT_LIMIT} records"
        )
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.command == "build":
            manifest = build_run_index(Path(args.run_dir))
            failed_videos = [
                item
                for item in manifest.get("videos", [])
                if isinstance(item, Mapping) and item.get("status") != "complete"
            ]
            payload = {
                "schema_version": 1,
                "kind": "bench_evidence_build",
                "status": "partial" if failed_videos else "complete",
                "summary": compact_manifest(manifest),
            }
        elif args.command == "list":
            payload = list_evidence(args)
        elif args.command == "records":
            payload = query_records(args)
        elif args.command == "frames":
            payload = query_frames(args)
        else:
            payload = source_lines(args)
        _emit(payload)
        return 0
    except Exception as error:
        message = re.sub(r"(?:/[^\s:]+)+", "<path>", str(error))[:240]
        print(
            f"bench evidence: {message or type(error).__name__}",
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
