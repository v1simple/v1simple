#!/usr/bin/env python3
"""Validate raw firmware perf CSV evidence without producing derived artifacts."""

from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


REPLAY_ALL_VOLUME_COUNTER = "v1AllVolumeParsed"
REPLAY_ALL_VOLUME_MIN_SCHEMA = 46
UINT32_MAX = 0xFFFFFFFF

REPLAY_EXACT_DELTAS = {
    "prioritySelectRowFlag": 708,
    "alertTablePublishes": 708,
    "alertTablePublishes3Bogey": 30,
}
REPLAY_ZERO_DELTAS = (
    "prioritySelectFirstUsable",
    "prioritySelectFirstEntry",
    "prioritySelectAmbiguousIndex",
    "prioritySelectUnusableIndex",
    "prioritySelectInvalidChosen",
    "alertTableRowReplacements",
    "alertTableAssemblyTimeouts",
    "parserRowsBandNone",
    "parserRowsKuRaw",
    "displayLiveInvalidPrioritySkips",
    "displayLiveFallbackToUsable",
    "disc",
    "qDrop",
    "parseFail",
)


class CsvEvidenceError(RuntimeError):
    """Raw perf CSV evidence is absent, malformed, or internally inconsistent."""


@dataclass(frozen=True)
class SessionMeta:
    seq: int = 0
    bootId: int = 0
    uptime_ms: int = 0
    token: str = ""
    schema: int = 0


@dataclass(frozen=True)
class ParsedSession:
    meta: Optional[SessionMeta]
    header: tuple[str, ...]
    rows: tuple[dict[str, int], ...]


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        self.print_usage(sys.stderr)
        self.exit(3, f"{self.prog}: error: {message}\n")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = _ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Raw firmware perf CSV")
    parser.add_argument(
        "--suite",
        required=True,
        choices=("core", "display", "replay"),
        help="Bench leg whose raw evidence is being validated",
    )
    parser.add_argument(
        "--segment",
        default="last",
        help="Core/display segment: last, last-connected, longest, longest-connected, auto, or 1-based index",
    )
    return parser.parse_args(argv)


def parse_int(value: str) -> int:
    text = (value or "").strip()
    if not text:
        raise ValueError("empty integer")
    digits = text[1:] if text.startswith("-") else text
    if not digits or not digits.isascii() or not digits.isdigit():
        raise ValueError(f"not an integer: {text!r}")
    return int(text)


def parse_session_meta(line: str) -> SessionMeta:
    values: dict[str, str] = {}
    for part in line.split(","):
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        values[key.strip()] = value.strip()
    return SessionMeta(
        seq=parse_int(values.get("seq", "0")),
        bootId=parse_int(values.get("bootId", "0")),
        uptime_ms=parse_int(values.get("uptime_ms", "0")),
        token=values.get("token", ""),
        schema=parse_int(values.get("schema", "0")),
    )


def _csv_fields(raw_line: str, line_number: int) -> list[str]:
    try:
        rows = list(csv.reader([raw_line], strict=True))
    except csv.Error as exc:
        raise CsvEvidenceError(f"perf CSV line {line_number} is malformed: {exc}") from exc
    if len(rows) != 1:
        raise CsvEvidenceError(f"perf CSV line {line_number} is malformed")
    return [field.strip() for field in rows[0]]


def _validate_header(fields: list[str], line_number: int) -> tuple[str, ...]:
    if not fields or fields[0] != "millis":
        raise CsvEvidenceError(f"perf CSV line {line_number} has no millis-led header")
    if any(not field for field in fields):
        raise CsvEvidenceError(f"perf CSV line {line_number} has an empty header field")
    duplicates = sorted({field for field in fields if fields.count(field) > 1})
    if duplicates:
        raise CsvEvidenceError(
            "perf CSV header repeats columns: " + ", ".join(duplicates)
        )
    return tuple(fields)


def _parse_row(raw_line: str, header: tuple[str, ...], line_number: int) -> dict[str, int]:
    fields = _csv_fields(raw_line, line_number)
    if len(fields) != len(header):
        millis = fields[0] if fields else ""
        raise CsvEvidenceError(
            f"perf CSV row at millis {millis or '?'} has {len(fields)} columns; "
            f"expected {len(header)}"
        )
    if not fields[0]:
        raise CsvEvidenceError(f"perf CSV line {line_number} has no millis value")

    parsed: dict[str, int] = {}
    for key, raw_value in zip(header, fields):
        if key == "utc":
            continue
        try:
            parsed[key] = parse_int(raw_value)
        except ValueError as exc:
            raise CsvEvidenceError(
                f"perf CSV line {line_number} column {key} is not an integer"
            ) from exc
    return parsed


def parse_perf_csv(path: Path) -> list[ParsedSession]:
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise CsvEvidenceError("perf CSV input is missing") from exc
    except (OSError, UnicodeError) as exc:
        raise CsvEvidenceError("perf CSV could not be read") from exc
    if not text:
        raise CsvEvidenceError("perf CSV is empty")
    if not text.endswith("\n"):
        raise CsvEvidenceError("perf CSV does not end with a complete line")

    sessions: list[ParsedSession] = []
    current_header: tuple[str, ...] | None = None
    current_meta: SessionMeta | None = None
    current_rows: list[dict[str, int]] = []
    leading_rows: list[tuple[int, str]] = []

    def finish_current(*, superseded: bool = False) -> None:
        nonlocal current_header, current_meta, current_rows
        if current_header is not None:
            if not current_rows:
                # A V1 connection can be replaced before the five-second perf
                # sampler emits its first row. A following header proves that
                # this empty session was superseded; an empty terminal session
                # remains a collection failure.
                if superseded:
                    current_meta = None
                    current_rows = []
                    return
                raise CsvEvidenceError("perf CSV session has no metric rows")
            sessions.append(
                ParsedSession(current_meta, current_header, tuple(current_rows))
            )
        current_meta = None
        current_rows = []

    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue

        if line.startswith("millis,"):
            fields = _validate_header(_csv_fields(raw_line, line_number), line_number)
            if current_header is None and leading_rows:
                implicit_rows = tuple(
                    _parse_row(raw, fields, pending_line)
                    for pending_line, raw in leading_rows
                )
                if implicit_rows:
                    sessions.append(ParsedSession(None, fields, implicit_rows))
                leading_rows = []
            else:
                finish_current(superseded=True)
            current_header = fields
            continue

        if line.startswith("#session_start"):
            if current_header is None:
                raise CsvEvidenceError(
                    f"perf CSV session marker at line {line_number} precedes its header"
                )
            if current_meta is not None or current_rows:
                raise CsvEvidenceError(
                    f"perf CSV session marker at line {line_number} is not directly after its header"
                )
            try:
                current_meta = parse_session_meta(line)
            except ValueError as exc:
                raise CsvEvidenceError(
                    f"perf CSV session marker at line {line_number} is invalid"
                ) from exc
            continue

        if line.startswith("#"):
            continue

        if current_header is None:
            leading_rows.append((line_number, raw_line))
            continue
        current_rows.append(_parse_row(raw_line, current_header, line_number))

    if current_header is None:
        raise CsvEvidenceError("perf CSV contains no header")
    finish_current()
    if not sessions:
        raise CsvEvidenceError("perf CSV contains no metric rows")
    return sessions


def load_sessions(path: Path) -> list[tuple[Optional[SessionMeta], list[dict[str, int]]]]:
    """Compatibility view used by direct raw-evidence checks."""
    return [
        (session.meta, [dict(row) for row in session.rows])
        for session in parse_perf_csv(path)
        if session.rows
    ]


def _duration_ms(rows: list[dict[str, int]]) -> int:
    if not rows:
        return 0
    return max(0, int(rows[-1].get("millis", 0)) - int(rows[0].get("millis", 0)))


def _connected(rows: list[dict[str, int]]) -> bool:
    if not rows:
        return False
    return int(rows[-1].get("rx", 0)) > int(rows[0].get("rx", 0)) or int(
        rows[-1].get("rx", 0)
    ) > 0


def choose_session(
    sessions: list[tuple[Optional[SessionMeta], list[dict[str, int]]]],
    selector: str,
) -> tuple[Optional[SessionMeta], list[dict[str, int]], int]:
    nonempty = [(meta, rows) for meta, rows in sessions if rows]
    if not nonempty:
        raise CsvEvidenceError("perf CSV contains no metric rows")

    value = (selector or "last").strip() or "last"
    try:
        index = int(value)
    except ValueError:
        index = 0
    if index:
        if index < 1 or index > len(nonempty):
            raise CsvEvidenceError(
                f"perf CSV segment {index} is outside 1..{len(nonempty)}"
            )
        meta, rows = nonempty[index - 1]
        return meta, rows, index

    if value == "last":
        meta, rows = nonempty[-1]
        return meta, rows, len(nonempty)
    if value in {"last-connected", "auto"}:
        for offset in range(len(nonempty) - 1, -1, -1):
            meta, rows = nonempty[offset]
            if _connected(rows):
                return meta, rows, offset + 1
        if value == "last-connected":
            raise CsvEvidenceError("perf CSV contains no connected segment")
        value = "longest"
    if value in {"longest", "longest-connected"}:
        candidates = [
            (offset, meta, rows)
            for offset, (meta, rows) in enumerate(nonempty)
            if value == "longest" or _connected(rows)
        ]
        if not candidates:
            raise CsvEvidenceError("perf CSV contains no connected segment")
        offset, meta, rows = max(
            candidates,
            key=lambda item: (_duration_ms(item[2]), -item[0]),
        )
        return meta, rows, offset + 1
    raise CsvEvidenceError(f"unknown perf CSV segment selector: {selector}")


def _validate_row_timing(rows: list[dict[str, int]], meta: SessionMeta | None) -> None:
    if not rows or any("millis" not in row for row in rows):
        raise CsvEvidenceError("perf CSV segment is missing a millis anchor")
    millis = [int(row["millis"]) for row in rows]
    if any(later < earlier for earlier, later in zip(millis, millis[1:])):
        raise CsvEvidenceError("perf CSV row timing regresses within a segment")
    if meta is not None and any(value < meta.uptime_ms for value in millis):
        raise CsvEvidenceError("perf CSV row timing precedes its session marker")


def validate_replay_csv(path: Path) -> tuple[bool, str]:
    parsed = parse_perf_csv(path)
    nonempty = [session for session in parsed if session.rows]
    if len(nonempty) < 2:
        raise CsvEvidenceError(
            "replay CSV requires QSTART and replacement-connection sessions"
        )

    qstart, replacement = nonempty[-2:]
    if qstart.meta is None or replacement.meta is None:
        raise CsvEvidenceError("replay CSV replacement window is missing session markers")
    qstart_meta = qstart.meta
    replacement_meta = replacement.meta
    if qstart.header != replacement.header:
        raise CsvEvidenceError(
            "replay CSV QSTART and replacement session headers do not match"
        )
    if (
        qstart_meta.bootId <= 0
        or replacement_meta.bootId != qstart_meta.bootId
        or replacement_meta.schema != qstart_meta.schema
        or qstart_meta.seq <= 0
        or replacement_meta.seq != qstart_meta.seq + 1
        or replacement_meta.uptime_ms <= qstart_meta.uptime_ms
        or not qstart_meta.token
        or not replacement_meta.token
        or replacement_meta.token == qstart_meta.token
    ):
        raise CsvEvidenceError(
            "replay CSV QSTART and replacement session metadata is discontinuous"
        )
    if qstart_meta.schema < REPLAY_ALL_VOLUME_MIN_SCHEMA:
        raise CsvEvidenceError(
            "replay CSV all-volume evidence requires schema "
            f">={REPLAY_ALL_VOLUME_MIN_SCHEMA}"
        )

    qstart_rows = [dict(row) for row in qstart.rows]
    replacement_rows = [dict(row) for row in replacement.rows]
    _validate_row_timing(qstart_rows, qstart_meta)
    _validate_row_timing(replacement_rows, replacement_meta)
    if max(row["millis"] for row in qstart_rows) >= replacement_meta.uptime_ms:
        raise CsvEvidenceError(
            "replay CSV replacement marker does not follow the QSTART session"
        )

    required = set(REPLAY_EXACT_DELTAS) | set(REPLAY_ZERO_DELTAS) | {
        REPLAY_ALL_VOLUME_COUNTER
    }
    missing = sorted(
        column
        for column in required
        if column not in qstart.header or column not in replacement.header
    )
    if missing:
        raise CsvEvidenceError(
            "replay CSV is missing required columns: " + ", ".join(missing)
        )

    rows = [*qstart_rows, *replacement_rows]
    volume_values = [int(row[REPLAY_ALL_VOLUME_COUNTER]) for row in rows]
    if any(value < 0 or value > UINT32_MAX for value in volume_values):
        raise CsvEvidenceError(
            "replay CSV all-volume consumption counter is not an unsigned integer"
        )

    regressed = sorted(
        column
        for column in required
        if any(
            int(later[column]) < int(earlier[column])
            for earlier, later in zip(rows, rows[1:])
        )
    )
    if regressed:
        raise CsvEvidenceError(
            "replay CSV cumulative counters regress: " + ", ".join(regressed)
        )

    observed = {
        column: int(rows[-1][column]) - int(rows[0][column])
        for column in required
    }
    failures = [
        f"{column} delta={observed[column]} expected={expected}"
        for column, expected in REPLAY_EXACT_DELTAS.items()
        if observed[column] != expected
    ]
    failures.extend(
        f"{column} delta={observed[column]} expected=0"
        for column in REPLAY_ZERO_DELTAS
        if observed[column] != 0
    )
    if observed[REPLAY_ALL_VOLUME_COUNTER] != 1:
        failures.append(
            f"{REPLAY_ALL_VOLUME_COUNTER} delta="
            f"{observed[REPLAY_ALL_VOLUME_COUNTER]} expected=1"
        )
    if failures:
        return False, "; ".join(failures)
    return True, "raw replay perf CSV matches the authored counter contract"


def validate_perf_csv(path: Path, suite: str, segment: str = "last") -> tuple[bool, str]:
    if suite not in {"core", "display", "replay"}:
        raise CsvEvidenceError(f"unknown bench suite: {suite}")
    if suite == "replay":
        normalized_segment = (segment or "last").strip() or "last"
        if normalized_segment not in {"last", "auto"}:
            raise CsvEvidenceError(
                "replay validation owns the final QSTART/replacement pair; use --segment last"
            )
        return validate_replay_csv(path)

    sessions = load_sessions(path)
    meta, rows, index = choose_session(sessions, segment)
    _validate_row_timing(rows, meta)
    return True, f"raw {suite} perf CSV segment {index} is structurally valid"


def _summary_rows(path: Path, suite: str, segment: str) -> list[dict[str, int]]:
    sessions = load_sessions(path)
    if suite == "replay":
        if len(sessions) < 2:
            raise CsvEvidenceError(
                "replay CSV requires QSTART and replacement-connection sessions"
            )
        return [*sessions[-2][1], *sessions[-1][1]]
    _meta, rows, _index = choose_session(sessions, segment)
    return rows


def _counter_delta(rows: list[dict[str, int]], column: str) -> int | None:
    if not rows or column not in rows[0] or column not in rows[-1]:
        return None
    return int(rows[-1][column]) - int(rows[0][column])


def _peak(rows: list[dict[str, int]], column: str) -> int | None:
    values = [int(row[column]) for row in rows if column in row]
    return max(values) if values else None


def _format_milliseconds(microseconds: int) -> str:
    return f"{microseconds / 1000.0:.1f}ms"


def render_perf_summary(
    path: Path,
    suite: str,
    segment: str = "last",
) -> list[str]:
    """Render a compact, non-gating view of the validated raw perf evidence."""
    rows = _summary_rows(path, suite, segment)
    _validate_row_timing(rows, None)
    duration_seconds = _duration_ms(rows) / 1000.0
    lines = [f"metrics: {len(rows)} samples over {duration_seconds:.1f}s"]

    work_columns = (
        ("rx", "rx"),
        ("parseOK", "parsed"),
        ("alertTablePublishes", "alerts"),
        ("displayUpdates", "display updates"),
    )
    work = [
        f"{label} {delta:+d}"
        for column, label in work_columns
        if (delta := _counter_delta(rows, column)) is not None
    ]
    if work:
        lines.append("work: " + " | ".join(work))

    peak_columns = (
        ("loopMax_us", "loop"),
        ("bleProcessMax_us", "BLE"),
        ("dispMax_us", "display"),
        ("sdMax_us", "SD"),
    )
    peaks = [
        f"{label} {_format_milliseconds(value)}"
        for column, label in peak_columns
        if (value := _peak(rows, column)) is not None
    ]
    if peaks:
        lines.append("peaks: " + " | ".join(peaks))

    health_columns = (
        ("qDrop", "queue drops"),
        ("parseFail", "parse failures"),
        ("perfDrop", "perf drops"),
        ("displaySkips", "display skips"),
    )
    health = [
        f"{label} {delta:+d}"
        for column, label in health_columns
        if (delta := _counter_delta(rows, column)) is not None
    ]
    dma_low = _peak(rows, "dmaFreeMin")
    if dma_low is not None:
        dma_low = min(int(row["dmaFreeMin"]) for row in rows if "dmaFreeMin" in row)
        health.append(f"DMA low {dma_low / 1024.0:.1f}KiB")
    if health:
        lines.append("health: " + " | ".join(health))
    return lines


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        passed, reason = validate_perf_csv(
            Path(args.input),
            args.suite,
            args.segment,
        )
    except CsvEvidenceError as exc:
        print(f"FAIL (collection): {exc}", file=sys.stderr)
        return 3
    if not passed:
        print(f"FAIL (semantic): {reason}", file=sys.stderr)
        return 2
    print(f"PASS: {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
