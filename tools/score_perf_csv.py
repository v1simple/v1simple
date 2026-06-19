#!/usr/bin/env python3
"""
Score perf CSV captures against canonical SLO thresholds.

Usage:
  python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv --profile drive_wifi_off
  python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv --profile drive_wifi_off --json

Exit codes:
  0 = hard SLO pass (advisories may warn)
  1 = hard SLO pass, advisory failures present
  2 = hard SLO failure
  3 = tool/input error
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SLO_FILE = ROOT / "tools" / "perf_slo_thresholds.json"
CheckTuple = tuple[str, str, str, float]


@dataclass
class ThresholdConfig:
    profiles: tuple[str, ...]
    hard_common: list[CheckTuple]
    hard_computed: list[CheckTuple]
    hard_profile: dict[str, list[CheckTuple]]
    advisory: list[CheckTuple]


@dataclass
class CheckResult:
    metric: str
    level: str
    op: str
    limit: float
    value: float
    passed: bool
    source: str


def _to_check_tuples(raw_checks: list[dict], section: str) -> list[CheckTuple]:
    checks: list[CheckTuple] = []
    for idx, entry in enumerate(raw_checks):
        try:
            metric = str(entry["metric"])
            source = str(entry["source"])
            op = str(entry["op"])
            limit = float(entry["limit"])
        except Exception as exc:
            raise ValueError(f"Invalid check in {section}[{idx}]: {exc}") from exc
        checks.append((metric, source, op, limit))
    return checks


def load_threshold_config(path: Path) -> ThresholdConfig:
    if not path.exists():
        raise RuntimeError(f"SLO threshold file not found: {path}")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise RuntimeError(f"Failed to parse SLO threshold file {path}: {exc}") from exc

    profiles_raw = payload.get("profiles")
    if not isinstance(profiles_raw, list) or not profiles_raw:
        raise RuntimeError("SLO threshold file missing non-empty 'profiles' list")

    profiles = tuple(str(item) for item in profiles_raw)
    hard_common = _to_check_tuples(payload.get("hard_common", []), "hard_common")
    hard_computed = _to_check_tuples(payload.get("hard_computed", []), "hard_computed")
    advisory = _to_check_tuples(payload.get("advisory", []), "advisory")

    hard_profile_raw = payload.get("hard_profile", {})
    if not isinstance(hard_profile_raw, dict):
        raise RuntimeError("SLO threshold file field 'hard_profile' must be an object")

    hard_profile: dict[str, list[CheckTuple]] = {}
    for profile in profiles:
        profile_checks = hard_profile_raw.get(profile)
        if not isinstance(profile_checks, list):
            raise RuntimeError(f"SLO threshold file missing hard_profile entries for '{profile}'")
        hard_profile[profile] = _to_check_tuples(profile_checks, f"hard_profile.{profile}")

    return ThresholdConfig(
        profiles=profiles,
        hard_common=hard_common,
        hard_computed=hard_computed,
        hard_profile=hard_profile,
        advisory=advisory,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Score perf CSV against Perf SLOs")
    parser.add_argument("csv_path", help="Path to perf_boot_*.csv")
    parser.add_argument("--profile", default="drive_wifi_off")
    parser.add_argument(
        "--slo-file",
        default=str(DEFAULT_SLO_FILE),
        help=f"Path to canonical SLO threshold file (default: {DEFAULT_SLO_FILE})",
    )
    parser.add_argument("--json", action="store_true", help="Output JSON")
    parser.add_argument(
        "--session",
        default="last-connected",
        help=(
            "Which session(s) to score. Options: "
            "'all' (legacy flat mode), "
            "'last' (last session), "
            "'last-connected' (last session with rx>0, default), "
            "'longest' (longest duration session), "
            "'longest-connected' (longest session with rx>0), "
            "or a 1-based session number."
        ),
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List sessions in the CSV and exit without scoring",
    )
    return parser.parse_args()


def parse_int(value: str) -> int:
    value = (value or "").strip()
    if value == "":
        return 0
    try:
        return int(value)
    except ValueError:
        # Accept accidental float-like values without crashing.
        return int(float(value))


def load_rows(path: Path) -> List[Dict[str, int]]:
    """Load all data rows from a (possibly multi-session) CSV as one flat list.

    Kept for backwards compatibility with ``--session all``.
    """
    sessions = load_sessions(path)
    rows: List[Dict[str, int]] = []
    for _meta, session_rows in sessions:
        rows.extend(session_rows)
    if not rows:
        raise RuntimeError("No data rows found in CSV")
    return rows


@dataclass
class SessionMeta:
    """Parsed metadata from a ``#session_start`` comment line."""

    seq: int = 0
    bootId: int = 0
    uptime_ms: int = 0
    token: str = ""
    schema: int = 0


def _parse_session_meta(line: str) -> SessionMeta:
    meta = SessionMeta()
    # Format: #session_start,seq=1,bootId=2,uptime_ms=1722,token=39ED396E,schema=6
    for part in line.split(","):
        if "=" not in part:
            continue
        key, val = part.split("=", 1)
        key = key.strip()
        val = val.strip()
        if key == "seq":
            meta.seq = int(val)
        elif key == "bootId":
            meta.bootId = int(val)
        elif key == "uptime_ms":
            meta.uptime_ms = int(val)
        elif key == "token":
            meta.token = val
        elif key == "schema":
            meta.schema = int(val)
    return meta


def load_sessions(
    path: Path,
) -> List[tuple]:
    """Parse a multi-session perf CSV into a list of (SessionMeta, rows) tuples.

    Each session is delimited by a ``millis,...`` header line followed by an
    optional ``#session_start,...`` comment.  Data rows belong to the most
    recent header section.
    """
    sessions: List[tuple] = []
    current_fields: Optional[List[str]] = None
    current_meta: Optional[SessionMeta] = None
    current_rows: List[Dict[str, int]] = []

    with path.open("r", newline="") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line:
                continue

            if line.startswith("millis,"):
                # New header = new session boundary
                if current_fields is not None and current_rows:
                    sessions.append((current_meta, current_rows))
                current_fields = line.split(",")
                current_rows = []
                current_meta = None
                continue

            if line.startswith("#session_start"):
                current_meta = _parse_session_meta(line)
                continue

            # Data row
            if current_fields is None:
                continue
            values = line.split(",")
            millis_val = values[0].strip() if values else ""
            if not millis_val or millis_val.startswith("#"):
                continue
            parsed: Dict[str, int] = {}
            for i, key in enumerate(current_fields):
                raw = values[i].strip() if i < len(values) else "0"
                parsed[key] = parse_int(raw or "0")
            current_rows.append(parsed)

    if current_fields is not None and current_rows:
        sessions.append((current_meta, current_rows))

    return sessions


def select_session(
    sessions: List[tuple],
    selector: str,
) -> tuple:
    """Return (SessionMeta | None, rows, session_index_1based) for the chosen session."""
    if not sessions:
        raise RuntimeError("No sessions found in CSV")

    # Numeric session index (1-based)
    try:
        idx = int(selector)
        if idx < 1 or idx > len(sessions):
            raise RuntimeError(
                f"Session {idx} out of range (1..{len(sessions)})"
            )
        meta, rows = sessions[idx - 1]
        return meta, rows, idx
    except ValueError:
        pass

    if selector == "last":
        meta, rows = sessions[-1]
        return meta, rows, len(sessions)

    if selector == "last-connected":
        for i in range(len(sessions) - 1, -1, -1):
            _meta, rows = sessions[i]
            if rows and rows[-1].get("rx", 0) > 0:
                return _meta, rows, i + 1
        # Fallback to last session if none have rx>0
        meta, rows = sessions[-1]
        return meta, rows, len(sessions)

    if selector == "longest":
        best_i = 0
        best_dur = 0
        for i, (_meta, rows) in enumerate(sessions):
            if rows:
                dur = rows[-1].get("millis", 0) - rows[0].get("millis", 0)
                if dur > best_dur:
                    best_dur = dur
                    best_i = i
        meta, rows = sessions[best_i]
        return meta, rows, best_i + 1

    if selector == "longest-connected":
        best_i = -1
        best_dur = 0
        for i, (_meta, rows) in enumerate(sessions):
            if rows and rows[-1].get("rx", 0) > 0:
                dur = rows[-1].get("millis", 0) - rows[0].get("millis", 0)
                if dur > best_dur:
                    best_dur = dur
                    best_i = i
        if best_i < 0:
            raise RuntimeError("No V1-connected sessions found in CSV")
        meta, rows = sessions[best_i]
        return meta, rows, best_i + 1

    raise RuntimeError(f"Unknown session selector: {selector}")


def compare(value: float, op: str, limit: float) -> bool:
    if op == "==":
        return math.isclose(value, limit, rel_tol=0.0, abs_tol=1e-9)
    if op == "<=":
        return value <= limit
    if op == ">=":
        return value >= limit
    raise ValueError(f"Unsupported op: {op}")


def max_of(rows: List[Dict[str, int]], field: str) -> float:
    return float(max(r[field] for r in rows))


def min_of(rows: List[Dict[str, int]], field: str) -> float:
    return float(min(r[field] for r in rows))


def final_of(rows: List[Dict[str, int]], field: str) -> float:
    return float(rows[-1][field])


def duration_s(rows: List[Dict[str, int]]) -> float:
    millis = rows[-1]["millis"] - rows[0]["millis"]
    return max(0.001, millis / 1000.0)


def compute_value(rows: List[Dict[str, int]], metric: str) -> float:
    dur = duration_s(rows)
    if metric == "cmdPaceNotYetPerMin":
        return final_of(rows, "cmdPaceNotYet") * 60.0 / dur
    if metric == "displaySkipPct":
        updates = final_of(rows, "displayUpdates")
        skips = final_of(rows, "displaySkips")
        total = updates + skips
        if total <= 0:
            return 0.0
        return skips * 100.0 / total
    if metric == "displaySkipsPerMin":
        return final_of(rows, "displaySkips") * 60.0 / dur
    raise KeyError(f"Unknown computed metric: {metric}")


def run_check(
    rows: List[Dict[str, int]],
    metric: str,
    source: str,
    op: str,
    limit: float,
    level: str,
) -> CheckResult:
    if source == "max":
        value = max_of(rows, metric)
    elif source == "min":
        value = min_of(rows, metric)
    elif source == "final":
        value = final_of(rows, metric)
    elif source == "computed":
        value = compute_value(rows, metric)
    else:
        raise ValueError(f"Unsupported source: {source}")

    passed = compare(value, op, limit)
    return CheckResult(
        metric=metric,
        level=level,
        op=op,
        limit=limit,
        value=value,
        passed=passed,
        source=source,
    )


def evaluate(rows: List[Dict[str, int]], profile: str, config: ThresholdConfig) -> List[CheckResult]:
    checks: List[CheckResult] = []
    for metric, source, op, limit in config.hard_common:
        checks.append(run_check(rows, metric, source, op, limit, "hard"))
    for metric, source, op, limit in config.hard_computed:
        checks.append(run_check(rows, metric, source, op, limit, "hard"))
    for metric, source, op, limit in config.hard_profile[profile]:
        checks.append(run_check(rows, metric, source, op, limit, "hard"))
    for metric, source, op, limit in config.advisory:
        checks.append(run_check(rows, metric, source, op, limit, "advisory"))
    return checks


def format_value(metric: str, value: float) -> str:
    if metric.endswith("PerMin") or metric.endswith("Pct") or metric.endswith("Hz"):
        return f"{value:.2f}"
    if abs(value - round(value)) < 1e-9:
        return str(int(round(value)))
    return f"{value:.2f}"


def print_human(path: Path, profile: str, rows: List[Dict[str, int]], checks: List[CheckResult],
                session_idx: int = 0, total_sessions: int = 0,
                session_meta: Optional[SessionMeta] = None, selector: str = "") -> None:
    dur = duration_s(rows)
    hard = [c for c in checks if c.level == "hard"]
    adv = [c for c in checks if c.level == "advisory"]
    hard_fail = [c for c in hard if not c.passed]
    adv_fail = [c for c in adv if not c.passed]

    print("=" * 72)
    print("PERF CSV SLO SCORECARD")
    print("=" * 72)
    print(f"File: {path}")
    print(f"Profile: {profile}")
    if total_sessions > 0:
        rx_final = int(final_of(rows, "rx")) if rows else 0
        tag = "V1-CONNECTED" if rx_final > 0 else "IDLE"
        token_str = f" token={session_meta.token}" if session_meta and session_meta.token else ""
        print(f"Session: {session_idx}/{total_sessions} [{tag}]{token_str} (--session {selector})")
    print(f"Rows: {len(rows)}")
    print(f"Duration: {dur:.2f}s")
    display_updates = int(final_of(rows, "displayUpdates"))
    display_skips = int(final_of(rows, "displaySkips"))
    display_skip_pct = compute_value(rows, "displaySkipPct")
    print(
        "Display: updates="
        f"{display_updates}, skips={display_skips}, skipPct={display_skip_pct:.2f}%"
    )
    print()
    print("Hard SLOs:")
    for c in hard:
        status = "PASS" if c.passed else "FAIL"
        print(
            f"  [{status}] {c.metric:22s} value={format_value(c.metric, c.value):>10s} "
            f"{c.op} {format_value(c.metric, c.limit):>10s} ({c.source})"
        )
    print()
    print("Advisory SLOs:")
    for c in adv:
        status = "PASS" if c.passed else "WARN"
        print(
            f"  [{status}] {c.metric:22s} value={format_value(c.metric, c.value):>10s} "
            f"{c.op} {format_value(c.metric, c.limit):>10s} ({c.source})"
        )
    print()
    print("-" * 72)
    print(f"Hard failures: {len(hard_fail)}")
    print(f"Advisory warnings: {len(adv_fail)}")
    if hard_fail:
        print("Result: FAIL")
    elif adv_fail:
        print("Result: PASS_WITH_WARNINGS")
    else:
        print("Result: PASS")


def print_session_list(path: Path, sessions: List[tuple]) -> None:
    """Print a summary table of all sessions in the CSV."""
    print("=" * 72)
    print(f"SESSIONS IN: {path}")
    print("=" * 72)
    print(f"{'#':>3}  {'Tag':14s}  {'Token':10s}  {'Rows':>5}  {'Duration':>10}  {'rx':>8}  {'parseOK':>8}")
    print("-" * 72)
    for i, (meta, rows) in enumerate(sessions):
        if not rows:
            print(f"{i+1:3d}  {'EMPTY':14s}")
            continue
        dur = max(0.001, rows[-1].get("millis", 0) / 1000.0)
        rx = rows[-1].get("rx", 0)
        parse_ok = rows[-1].get("parseOK", 0)
        tag = "V1-CONNECTED" if rx > 0 else "IDLE"
        token = meta.token[:8] if meta and meta.token else "?"
        dur_str = f"{dur:.1f}s"
        print(f"{i+1:3d}  {tag:14s}  {token:10s}  {len(rows):5d}  {dur_str:>10}  {rx:8d}  {parse_ok:8d}")
    print("-" * 72)
    connected = sum(1 for _m, r in sessions if r and r[-1].get("rx", 0) > 0)
    idle = len(sessions) - connected
    print(f"Total: {len(sessions)} sessions ({connected} connected, {idle} idle)")


def main() -> int:
    args = parse_args()
    path = Path(args.csv_path)
    if not path.exists():
        print(f"ERROR: file not found: {path}", file=sys.stderr)
        return 3

    slo_file = Path(args.slo_file)
    try:
        config = load_threshold_config(slo_file)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    if args.profile not in config.profiles:
        valid = ", ".join(config.profiles)
        print(
            f"ERROR: Unknown profile '{args.profile}'. Valid profiles from {slo_file}: {valid}",
            file=sys.stderr,
        )
        return 3

    try:
        sessions = load_sessions(path)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    if not sessions:
        print("ERROR: No sessions found in CSV", file=sys.stderr)
        return 3

    if args.list:
        print_session_list(path, sessions)
        return 0

    try:
        selector = args.session
        if selector == "all":
            # Legacy flat mode: merge all sessions
            rows: List[Dict[str, int]] = []
            for _meta, session_rows in sessions:
                rows.extend(session_rows)
            if not rows:
                print("ERROR: No data rows found in CSV", file=sys.stderr)
                return 3
            session_meta = None
            session_idx = 0
            total_sessions = len(sessions)
        else:
            session_meta, rows, session_idx = select_session(sessions, selector)
            total_sessions = len(sessions)
            if not rows:
                print(
                    f"ERROR: Selected session {session_idx} has no data rows",
                    file=sys.stderr,
                )
                return 3

        checks = evaluate(rows, args.profile, config)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    hard_fail = [c for c in checks if c.level == "hard" and not c.passed]
    adv_fail = [c for c in checks if c.level == "advisory" and not c.passed]

    if args.json:
        payload = {
            "file": str(path),
            "profile": args.profile,
            "slo_file": str(slo_file),
            "session": selector if selector != "all" else "all",
            "session_index": session_idx,
            "total_sessions": total_sessions,
            "session_token": session_meta.token if session_meta else None,
            "rows": len(rows),
            "duration_s": duration_s(rows),
            "hard_failures": len(hard_fail),
            "advisory_warnings": len(adv_fail),
            "checks": [
                {
                    "metric": c.metric,
                    "level": c.level,
                    "source": c.source,
                    "value": c.value,
                    "op": c.op,
                    "limit": c.limit,
                    "passed": c.passed,
                }
                for c in checks
            ],
        }
        print(json.dumps(payload, indent=2))
    else:
        print_human(path, args.profile, rows, checks,
                    session_idx=session_idx, total_sessions=total_sessions,
                    session_meta=session_meta, selector=selector)

    if hard_fail:
        return 2
    if adv_fail:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
