#!/usr/bin/env python3
"""Compare two perf CSV captures and flag metric regressions."""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import score_perf_csv


@dataclass
class ScoreRun:
    path: Path
    session_selector: str
    session_index: int
    total_sessions: int
    session_token: Optional[str]
    rows: list[dict[str, int]]
    checks: dict[str, score_perf_csv.CheckResult]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare perf CSV captures (week-over-week style) and flag regressions",
    )
    parser.add_argument("baseline_csv", help="Older/baseline perf CSV path")
    parser.add_argument("candidate_csv", help="New/candidate perf CSV path")
    parser.add_argument("--profile", default="drive_wifi_off", help="SLO profile to score")
    parser.add_argument(
        "--session",
        default="last-connected",
        help=(
            "Session selector for each CSV: all|last|last-connected|longest|"
            "longest-connected|<1-based-index>"
        ),
    )
    parser.add_argument(
        "--slo-file",
        default=str(score_perf_csv.DEFAULT_SLO_FILE),
        help="Path to canonical SLO threshold JSON",
    )
    parser.add_argument("--json", action="store_true", help="Output JSON")
    return parser.parse_args()


def format_value(metric: str, value: float) -> str:
    return score_perf_csv.format_value(metric, value)


def score_capture(path: Path, selector: str, profile: str, config: score_perf_csv.ThresholdConfig) -> ScoreRun:
    sessions = score_perf_csv.load_sessions(path)
    if not sessions:
        raise RuntimeError(f"No sessions found in CSV: {path}")

    if selector == "all":
        rows: list[dict[str, int]] = []
        for _meta, session_rows in sessions:
            rows.extend(session_rows)
        if not rows:
            raise RuntimeError(f"No data rows found in CSV: {path}")
        session_index = 0
        session_token: Optional[str] = None
    else:
        meta, rows, session_index = score_perf_csv.select_session(sessions, selector)
        if not rows:
            raise RuntimeError(f"Selected session has no rows: {path}")
        session_token = meta.token if meta else None

    checks = {
        check.metric: check
        for check in score_perf_csv.evaluate(rows, profile, config)
    }
    return ScoreRun(
        path=path,
        session_selector=selector,
        session_index=session_index,
        total_sessions=len(sessions),
        session_token=session_token,
        rows=rows,
        checks=checks,
    )


def worsening_delta(
    baseline: score_perf_csv.CheckResult,
    candidate: score_perf_csv.CheckResult,
) -> tuple[float, float]:
    raw_delta = candidate.value - baseline.value
    if baseline.op == "<=":
        return raw_delta, raw_delta
    if baseline.op == ">=":
        return raw_delta, baseline.value - candidate.value
    if baseline.op == "==":
        base_dist = abs(baseline.value - baseline.limit)
        cand_dist = abs(candidate.value - candidate.limit)
        return raw_delta, cand_dist - base_dist
    raise ValueError(f"Unsupported op: {baseline.op}")


def run() -> int:
    args = parse_args()
    baseline_path = Path(args.baseline_csv)
    candidate_path = Path(args.candidate_csv)
    for csv_path in (baseline_path, candidate_path):
        if not csv_path.exists():
            print(f"ERROR: file not found: {csv_path}", file=sys.stderr)
            return 3

    slo_file = Path(args.slo_file)
    try:
        config = score_perf_csv.load_threshold_config(slo_file)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    if args.profile not in config.profiles:
        print(
            f"ERROR: Unknown profile '{args.profile}'. Valid profiles: {', '.join(config.profiles)}",
            file=sys.stderr,
        )
        return 3

    try:
        baseline = score_capture(baseline_path, args.session, args.profile, config)
        candidate = score_capture(candidate_path, args.session, args.profile, config)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    baseline_metrics = set(baseline.checks)
    candidate_metrics = set(candidate.checks)
    if baseline_metrics != candidate_metrics:
        missing = sorted(baseline_metrics - candidate_metrics)
        extra = sorted(candidate_metrics - baseline_metrics)
        if missing:
            print("ERROR: candidate missing metrics: " + ", ".join(missing), file=sys.stderr)
        if extra:
            print("ERROR: candidate has unexpected metrics: " + ", ".join(extra), file=sys.stderr)
        return 3

    regressions: list[dict[str, object]] = []
    improvements: list[dict[str, object]] = []
    hard_regressions = 0
    advisory_regressions = 0
    tolerance = 1e-9

    for metric in sorted(baseline_metrics):
        b = baseline.checks[metric]
        c = candidate.checks[metric]
        raw_delta, worse_delta = worsening_delta(b, c)
        transition = f"{'PASS' if b.passed else 'FAIL'}->{ 'PASS' if c.passed else 'FAIL'}"

        row = {
            "metric": metric,
            "level": c.level,
            "source": c.source,
            "op": c.op,
            "limit": c.limit,
            "baseline": b.value,
            "candidate": c.value,
            "delta": raw_delta,
            "worsening": worse_delta,
            "state": transition,
        }
        if worse_delta > tolerance:
            regressions.append(row)
            if c.level == "hard":
                hard_regressions += 1
            else:
                advisory_regressions += 1
        elif worse_delta < -tolerance:
            improvements.append(row)

    if args.json:
        payload = {
            "profile": args.profile,
            "slo_file": str(slo_file),
            "session_selector": args.session,
            "baseline": {
                "file": str(baseline.path),
                "session_index": baseline.session_index,
                "total_sessions": baseline.total_sessions,
                "session_token": baseline.session_token,
                "rows": len(baseline.rows),
                "duration_s": score_perf_csv.duration_s(baseline.rows),
            },
            "candidate": {
                "file": str(candidate.path),
                "session_index": candidate.session_index,
                "total_sessions": candidate.total_sessions,
                "session_token": candidate.session_token,
                "rows": len(candidate.rows),
                "duration_s": score_perf_csv.duration_s(candidate.rows),
            },
            "regressions": regressions,
            "improvements": improvements,
            "hard_regressions": hard_regressions,
            "advisory_regressions": advisory_regressions,
        }
        print(json.dumps(payload, indent=2))
    else:
        print("=" * 72)
        print("PERF CSV TREND COMPARISON")
        print("=" * 72)
        print(f"Profile: {args.profile}")
        print(f"SLO file: {slo_file}")
        print(
            "Baseline: "
            f"{baseline.path} "
            f"(session={baseline.session_index}/{baseline.total_sessions}, rows={len(baseline.rows)}, duration={score_perf_csv.duration_s(baseline.rows):.2f}s)"
        )
        print(
            "Candidate: "
            f"{candidate.path} "
            f"(session={candidate.session_index}/{candidate.total_sessions}, rows={len(candidate.rows)}, duration={score_perf_csv.duration_s(candidate.rows):.2f}s)"
        )
        print()
        if regressions:
            print("Regressions:")
            for row in regressions:
                metric = str(row["metric"])
                level = str(row["level"]).upper()
                state = str(row["state"])
                op = str(row["op"])
                limit = float(row["limit"])
                baseline_value = float(row["baseline"])
                candidate_value = float(row["candidate"])
                delta = float(row["delta"])
                print(
                    f"  [{level}] {metric:22s} {state:10s} "
                    f"baseline={format_value(metric, baseline_value):>8s} "
                    f"candidate={format_value(metric, candidate_value):>8s} "
                    f"delta={delta:+.2f} gate={op} {format_value(metric, limit)}"
                )
        else:
            print("Regressions: none")

        print()
        print(
            f"Summary: hard_regressions={hard_regressions} advisory_regressions={advisory_regressions} improvements={len(improvements)}"
        )
        if hard_regressions > 0:
            print("Result: HARD_REGRESSION")
        elif advisory_regressions > 0:
            print("Result: ADVISORY_REGRESSION")
        else:
            print("Result: NO_REGRESSION")

    if hard_regressions > 0:
        return 2
    if advisory_regressions > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(run())
