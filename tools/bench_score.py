#!/usr/bin/env python3
"""Score a bench run made of core/display SD CSV windows.

The bench score intentionally has no baseline concept. A window passes when
collection completed, imported metrics are present, hard catalog failures are
zero, and CSV SLO hard failures are zero. Regression/no-baseline language is not
part of this result.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

RESULT_ORDER = {"PASS": 0, "WARN": 1, "FAIL": 2, "COLLECTION_FAILED": 3}
EXIT_BY_RESULT = {"PASS": 0, "WARN": 1, "FAIL": 2, "COLLECTION_FAILED": 3}
ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--suite", action="append", choices=["core", "display"], default=[])
    parser.add_argument("--out", default="")
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any] | None:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return payload if isinstance(payload, dict) else None


def load_catalog(path: Path = CATALOG_PATH) -> dict[str, dict[str, Any]]:
    payload = load_json(path) or {}
    policies: dict[str, dict[str, Any]] = {}
    for item in payload.get("metrics") or []:
        if not isinstance(item, dict) or item.get("run_kind") != "real_fw_soak":
            continue
        metric = str(item.get("metric") or "")
        if metric and metric not in policies:
            policies[metric] = item
    return policies


def worse(a: str, b: str) -> str:
    return a if RESULT_ORDER[a] >= RESULT_ORDER[b] else b


def metric_failures(scoring: dict[str, Any]) -> list[dict[str, Any]]:
    failures: list[dict[str, Any]] = []
    for metric in scoring.get("metrics") or []:
        if not isinstance(metric, dict):
            continue
        if metric.get("score_status") == "fail":
            failures.append(metric)
    return failures


def budget_pressure(metric: dict[str, Any], catalog: dict[str, dict[str, Any]]) -> dict[str, Any] | None:
    current = metric.get("current_value")
    if not isinstance(current, (int, float)):
        return None
    policy = catalog.get(str(metric.get("metric") or ""))
    if not policy:
        return None
    direction = policy.get("direction")
    score_level = str(policy.get("score_level") or metric.get("score_level") or "")
    if score_level not in {"hard", "advisory"}:
        return None

    if direction == "lower_better":
        limit = policy.get("absolute_max")
        if not isinstance(limit, (int, float)) or limit <= 0:
            return None
        used = float(current) / float(limit)
        rule = "<="
    elif direction == "higher_better":
        limit = policy.get("absolute_min")
        if not isinstance(limit, (int, float)) or limit <= 0:
            return None
        used = float(limit) / float(current) if current > 0 else float("inf")
        rule = ">="
    else:
        return None

    return {
        "metric": metric.get("metric"),
        "value": current,
        "unit": metric.get("unit") or policy.get("unit") or "",
        "limit": limit,
        "rule": rule,
        "level": score_level,
        "budget_used": used,
    }


def top_budget_pressures(scoring: dict[str, Any], catalog: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for metric in scoring.get("metrics") or []:
        if not isinstance(metric, dict):
            continue
        row = budget_pressure(metric, catalog)
        if row is not None:
            rows.append(row)
    rows.sort(key=lambda item: float(item.get("budget_used") or 0), reverse=True)
    return rows[:8]


def csv_failures(scorecard: dict[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    hard: list[dict[str, Any]] = []
    advisory: list[dict[str, Any]] = []
    for check in scorecard.get("checks") or []:
        if not isinstance(check, dict) or check.get("passed") is not False:
            continue
        if check.get("level") == "hard":
            hard.append(check)
        else:
            advisory.append(check)
    return hard, advisory


def window_path(window_dir: Path, raw: Any, fallback_name: str) -> Path:
    text = str(raw or "")
    path = Path(text) if text else window_dir / fallback_name
    if not path.is_absolute():
        path = window_dir / path
    return path


def classify_window(run_dir: Path, suite: str, catalog: dict[str, dict[str, Any]]) -> dict[str, Any]:
    window_dir = run_dir / suite
    result_path = window_dir / "window_result.json"
    window = load_json(result_path)
    if window is None:
        return {
            "suite": suite,
            "result": "COLLECTION_FAILED",
            "artifact_dir": str(window_dir),
            "evidence": [f"missing or invalid {result_path}"],
        }
    if window.get("result") == "COLLECTION_FAILED":
        return {
            "suite": suite,
            "result": "COLLECTION_FAILED",
            "artifact_dir": str(window_dir),
            "evidence": [str(window.get("error") or "collection failed")],
        }

    scoring_path = window_path(window_dir, window.get("scoring_path"), "scoring.json")
    csv_scorecard_path = window_path(window_dir, window.get("csv_scorecard_path"), "csv_scorecard.json")
    scoring = load_json(scoring_path)
    csv_scorecard = load_json(csv_scorecard_path)
    if scoring is None:
        return {
            "suite": suite,
            "result": "COLLECTION_FAILED",
            "artifact_dir": str(window_dir),
            "evidence": [f"missing or invalid scoring artifact: {scoring_path}"],
        }
    if csv_scorecard is None:
        return {
            "suite": suite,
            "result": "COLLECTION_FAILED",
            "artifact_dir": str(window_dir),
            "evidence": [f"missing or invalid CSV scorecard: {csv_scorecard_path}"],
        }

    failures = metric_failures(scoring)
    csv_hard, csv_advisory = csv_failures(csv_scorecard)
    summary = scoring.get("summary") if isinstance(scoring.get("summary"), dict) else {}
    hard_failures = int(summary.get("hard_failures") or 0)
    advisory_failures = int(summary.get("advisory_failures") or 0)
    csv_hard_count = int(csv_scorecard.get("hard_failures") or len(csv_hard))
    csv_advisory_count = int(csv_scorecard.get("advisory_warnings") or len(csv_advisory))

    result = "PASS"
    evidence: list[str] = []
    if hard_failures > 0 or csv_hard_count > 0:
        result = "FAIL"
    elif advisory_failures > 0 or csv_advisory_count > 0:
        result = "WARN"

    for metric in failures[:20]:
        evidence.append(
            f"{metric.get('metric')} current={metric.get('current_value')} "
            f"state={metric.get('absolute_state')} messages={'; '.join(str(m) for m in metric.get('messages') or [])}"
        )
    for check in csv_hard[:20]:
        evidence.append(
            f"csv:{check.get('metric')} value={check.get('value')} {check.get('op')} {check.get('limit')} failed"
        )
    if result == "WARN":
        for check in csv_advisory[:10]:
            evidence.append(
                f"csv:{check.get('metric')} value={check.get('value')} {check.get('op')} {check.get('limit')} warning"
            )

    manifest = load_json(window_path(window_dir, window.get("manifest_path"), "manifest.json")) or {}
    budget = top_budget_pressures(scoring, catalog)
    return {
        "suite": suite,
        "result": result,
        "git_sha": manifest.get("git_sha", ""),
        "git_ref": manifest.get("git_ref", ""),
        "git_worktree_clean": window.get("git_worktree_clean") is True,
        "artifact_dir": str(window_dir),
        "csv_path": window.get("csv_path", ""),
        "rows": manifest.get("rows"),
        "duration_s": manifest.get("duration_s"),
        "hard_failures": hard_failures + csv_hard_count,
        "advisory_failures": advisory_failures + csv_advisory_count,
        "metrics_scored": summary.get("metrics_scored"),
        "budget_pressure": budget,
        "evidence": evidence,
    }


def format_value(value: Any, unit: str = "") -> str:
    if isinstance(value, float):
        text = f"{value:.1f}" if abs(value - round(value)) > 1e-9 else str(int(round(value)))
    else:
        text = str(value)
    return f"{text}{unit}" if unit else text


def render_text(payload: dict[str, Any]) -> str:
    lines: list[str] = []
    lines.append(f"bench result: {payload['result']}")
    lines.append(f"collection: {'PASS' if payload['result'] != 'COLLECTION_FAILED' else 'FAIL'}")
    for window in payload["windows"]:
        detail = f"{window['suite']}: {window['result']}"
        if window.get("rows") is not None:
            detail += f" ({window.get('rows')} rows, {float(window.get('duration_s') or 0):.1f}s)"
        lines.append(detail)
    failures = [w for w in payload["windows"] if w["result"] != "PASS"]
    if failures:
        lines.append("")
        lines.append("failed:")
        for window in failures:
            evidence = window.get("evidence") or []
            if not evidence:
                lines.append(f"  {window['suite']}: {window['result']}")
            for item in evidence:
                lines.append(f"  {window['suite']}.{item}")
    if payload["result"] == "PASS":
        lines.append("")
        lines.append("top budget pressure:")
        for window in payload["windows"]:
            budget = window.get("budget_pressure") or []
            if not budget:
                lines.append(f"  {window['suite']}: no hard/advisory budget metrics found")
                continue
            top = budget[:5]
            for item in top:
                used = float(item.get("budget_used") or 0) * 100.0
                unit = str(item.get("unit") or "")
                lines.append(
                    f"  {window['suite']}.{item.get('metric')}: "
                    f"{format_value(item.get('value'), unit)} "
                    f"{item.get('rule')} {format_value(item.get('limit'), unit)} "
                    f"({used:.0f}% of {item.get('level')} budget)"
                )
    lines.append("")
    lines.append("artifacts:")
    lines.append(f"  {payload['run_dir']}")
    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()
    run_dir = Path(args.run_dir).resolve()
    suites = args.suite or [name for name in ("core", "display") if (run_dir / name).exists()]
    if not suites:
        suites = ["core", "display"]

    catalog = load_catalog()
    windows = [classify_window(run_dir, suite, catalog) for suite in suites]
    result = "PASS"
    for window in windows:
        result = worse(result, str(window["result"]))

    git_shas = {str(window.get("git_sha") or "").strip() for window in windows}
    git_refs = {str(window.get("git_ref") or "").strip() for window in windows}
    payload = {
        "schema_version": 2,
        "kind": "bench_result",
        "run_dir": str(run_dir),
        "git_sha": next(iter(git_shas)) if len(git_shas) == 1 else "",
        "git_ref": next(iter(git_refs)) if len(git_refs) == 1 else "",
        "git_worktree_clean": bool(windows)
        and all(window.get("git_worktree_clean") is True for window in windows),
        "result": result,
        "windows": windows,
    }
    out_path = Path(args.out) if args.out else run_dir / "bench_result.json"
    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    text = render_text(payload)
    (run_dir / "bench_summary.txt").write_text(text, encoding="utf-8")
    sys.stdout.write(text)
    return EXIT_BY_RESULT[result]


if __name__ == "__main__":
    raise SystemExit(main())
