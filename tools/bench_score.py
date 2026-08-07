#!/usr/bin/env python3
"""Score a unified bench run made of core/display/replay SD CSV windows.

The bench score intentionally has no baseline concept. A window passes when
collection completed, imported metrics are present, hard catalog failures are
zero, and advisory catalog failures are zero. Regression/no-baseline language
is not part of this result.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from import_perf_csv import load_sessions

RESULT_ORDER = {"PASS": 0, "WARN": 1, "FAIL": 2, "COLLECTION_FAILED": 3}
EXIT_BY_RESULT = {"PASS": 0, "WARN": 1, "FAIL": 2, "COLLECTION_FAILED": 3}
ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--suite", action="append", choices=["core", "display", "replay"], default=[])
    parser.add_argument(
        "--camera-suite",
        action="append",
        choices=["core", "display", "replay"],
        default=[],
        help="Suite whose camera evidence is required for this verdict",
    )
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
    """Return absolute gate failures; promoted baselines are comparison aids."""
    failures: list[dict[str, Any]] = []
    for metric in scoring.get("metrics") or []:
        if not isinstance(metric, dict):
            continue
        if metric.get("absolute_state") == "fail" or (
            metric.get("absolute_state") == "missing" and metric.get("required") is True
        ):
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


def window_path(window_dir: Path, raw: Any, fallback_name: str) -> Path:
    text = str(raw or "")
    path = Path(text) if text else window_dir / fallback_name
    if not path.is_absolute():
        path = window_dir / path
    return path


def counter_delta(rows: list[dict[str, int]], column: str) -> int:
    return int(rows[-1].get(column, 0)) - int(rows[0].get(column, 0))


def score_replay_csv(csv_path: Path, _selector: str) -> dict[str, Any]:
    exact = {
        "prioritySelectRowFlag": 708,
        "alertTablePublishes": 708,
        "alertTablePublishes3Bogey": 30,
    }
    zero = (
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
    try:
        sessions = load_sessions(csv_path)
        nonempty_sessions = [
            (index, meta, rows)
            for index, (meta, rows) in enumerate(sessions, start=1)
            if rows
        ]
    except (OSError, RuntimeError, ValueError) as exc:
        return {"result": "COLLECTION_FAILED", "evidence": [f"replay CSV could not be read: {exc}"]}
    if not nonempty_sessions:
        return {"result": "COLLECTION_FAILED", "evidence": ["replay CSV contains no metric rows"]}

    # A BLE reconnect starts a new perf session marker without resetting the
    # boot-lifetime counters. Deterministic replay owns the complete QSTART
    # window, so scoring only the final connection fragment discards earlier
    # phases (including the three-bogey block).
    rows = [row for _index, _meta, session_rows in nonempty_sessions for row in session_rows]
    session_indices = [index for index, _meta, _rows in nonempty_sessions]

    required = set(exact) | set(zero)
    missing = sorted(required - set(rows[0]))
    if missing:
        return {
            "result": "COLLECTION_FAILED",
            "evidence": ["replay CSV is missing required columns: " + ", ".join(missing)],
        }

    observed = {column: counter_delta(rows, column) for column in required}
    failures: list[str] = []
    for column, expected in exact.items():
        if observed[column] != expected:
            failures.append(f"{column} delta={observed[column]} expected={expected}")
    for column in zero:
        if observed[column] != 0:
            failures.append(f"{column} delta={observed[column]} expected=0")
    return {
        "result": "FAIL" if failures else "PASS",
        "segment_scope": "complete_window",
        "session_count": len(nonempty_sessions),
        "session_indices": session_indices,
        "session_index": session_indices[-1],
        "row_count": len(rows),
        "observed_deltas": {column: observed[column] for column in sorted(observed)},
        "evidence": failures,
    }


def classify_window(
    run_dir: Path,
    suite: str,
    catalog: dict[str, dict[str, Any]],
    camera_required: bool = False,
) -> dict[str, Any]:
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
    scoring = load_json(scoring_path)
    if scoring is None:
        return {
            "suite": suite,
            "result": "COLLECTION_FAILED",
            "artifact_dir": str(window_dir),
            "evidence": [f"missing or invalid scoring artifact: {scoring_path}"],
        }
    failures = metric_failures(scoring)
    summary = scoring.get("summary") if isinstance(scoring.get("summary"), dict) else {}
    hard_failures = sum(1 for metric in failures if str(metric.get("score_level") or "hard") == "hard")
    advisory_failures = sum(
        1 for metric in failures if str(metric.get("score_level") or "hard") == "advisory"
    )

    result = "PASS"
    evidence: list[str] = []
    scoring_manifest = scoring.get("manifest") if isinstance(scoring.get("manifest"), dict) else {}
    base_result = str(scoring_manifest.get("base_result") or "PASS")
    if base_result == "FAIL":
        result = "FAIL"
        evidence.append("imported metrics window reported a failed base result")
    elif base_result == "INCONCLUSIVE":
        result = "COLLECTION_FAILED"
        evidence.append("imported metrics window was inconclusive")
    elif hard_failures > 0:
        result = "FAIL"
    elif advisory_failures > 0:
        result = "WARN"

    for metric in failures[:20]:
        absolute_messages = [
            str(message)
            for message in metric.get("messages") or []
            if "baseline" not in str(message).lower()
        ]
        evidence.append(
            f"{metric.get('metric')} current={metric.get('current_value')} "
            f"state={metric.get('absolute_state')} messages={'; '.join(absolute_messages)}"
        )
    replay_checks: dict[str, Any] = {}
    emulator = window.get("v1_emulator") if isinstance(window.get("v1_emulator"), dict) else {}
    if emulator and emulator.get("completed") is not True:
        result = "COLLECTION_FAILED"
        evidence.append("managed V1 emulator did not cover the complete metrics window")
    if suite == "replay":
        replay_process = window.get("replay") if isinstance(window.get("replay"), dict) else {}
        if replay_process.get("completed") is not True:
            result = "COLLECTION_FAILED"
            evidence.append("v1replay did not complete successfully")
        csv_path = window_path(window_dir, window.get("csv_path"), "perf.csv")
        replay_checks = score_replay_csv(csv_path, str(window.get("segment") or "last"))
        result = worse(result, str(replay_checks["result"]))
        evidence.extend(str(item) for item in replay_checks.get("evidence") or [])

    camera: dict[str, Any] = {}
    if camera_required:
        camera_path = window_dir / "camera" / "camera_result.json"
        camera = load_json(camera_path) or {}
        missing_camera_files: list[str] = []
        if camera.get("result") == "CAPTURED":
            for key in ("video", "session_start_still", "bright_still", "dim_still"):
                name = str(camera.get(key) or "")
                evidence_path = camera_path.parent / name if name else camera_path.parent / "__missing__"
                if not evidence_path.is_file() or evidence_path.stat().st_size == 0:
                    missing_camera_files.append(key)
        if camera.get("result") != "CAPTURED" or missing_camera_files:
            result = "COLLECTION_FAILED"
            camera_errors = camera.get("errors") if isinstance(camera.get("errors"), list) else []
            evidence.append(
                "camera evidence was requested but not captured"
                + (f"; missing files: {', '.join(missing_camera_files)}" if missing_camera_files else "")
                + (f": {'; '.join(str(item) for item in camera_errors)}" if camera_errors else "")
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
        "hard_failures": hard_failures,
        "advisory_failures": advisory_failures,
        "metrics_scored": summary.get("metrics_scored"),
        "replay_checks": replay_checks,
        "v1_emulator": {
            "mode": emulator.get("mode", ""),
            "completed": emulator.get("completed") is True,
            "managed_stop": emulator.get("managed_stop") is True,
        }
        if emulator
        else {},
        "camera": {
            "result": camera.get("result", ""),
            "video": camera.get("video", ""),
            "video_duration_seconds": camera.get("video_duration_seconds"),
            "visually_graded": camera.get("visually_graded") is True,
        }
        if camera_required
        else {},
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
        if window.get("v1_emulator", {}).get("mode"):
            detail += f", V1={window['v1_emulator']['mode']}"
        if window.get("replay_checks", {}).get("result"):
            detail += f", replay={window['replay_checks']['result']}"
        if window.get("camera", {}).get("result"):
            detail += f", camera={window['camera']['result']} (ungraded)"
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
    suites = args.suite or [name for name in ("core", "display", "replay") if (run_dir / name).exists()]
    if not suites:
        suites = ["core", "display"]

    catalog = load_catalog()
    camera_suites = set(args.camera_suite)
    windows = [classify_window(run_dir, suite, catalog, suite in camera_suites) for suite in suites]
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
