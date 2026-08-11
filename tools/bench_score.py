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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from bench_identity import current_grader_fingerprint  # noqa: E402
from camera_artifacts import (  # noqa: E402
    CAPTURE_MANIFEST_NAME,
    CameraArtifactError,
    agreed_window_identity,
    camera_result_view,
    load_capture_manifest,
    load_owned_grade,
    resolve_manifest_artifact,
    strict_grade_outcome,
    validate_capture_window_identity,
    verify_capture_files,
)
from camera_contract import camera_evidence_contract  # noqa: E402

RESULT_ORDER = {
    "PASS": 0,
    "WARN": 1,
    "EVIDENCE_FAILED": 2,
    "FAIL": 3,
    "COLLECTION_FAILED": 4,
}
EXIT_BY_RESULT = {
    "PASS": 0,
    "WARN": 1,
    "FAIL": 2,
    "EVIDENCE_FAILED": 3,
    "COLLECTION_FAILED": 3,
}
CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"
CURRENT_GRADER_FINGERPRINT = current_grader_fingerprint(ROOT)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--suite", action="append", choices=["core", "display", "replay"], default=[])
    parser.add_argument(
        "--camera-suite",
        action="append",
        choices=["replay"],
        default=[],
        help="Suite whose camera evidence is required for this verdict",
    )
    parser.add_argument(
        "--out",
        default="",
        help=(
            "JSON result path; custom paths leave the run's canonical summary unchanged "
            "and must not resolve to it"
        ),
    )
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
        if (
            metric.get("absolute_state") == "fail"
            or metric.get("advisory_state") == "fail"
            or (metric.get("absolute_state") == "missing" and metric.get("required") is True)
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
    if window.get("result") == "EVIDENCE_FAILED":
        camera_contract = camera_evidence_contract(suite)
        window_camera = window.get("camera") if isinstance(window.get("camera"), dict) else {}
        preflight_name = Path(str(window_camera.get("preflight") or "camera_preflight.json")).name
        preflight = load_json(window_dir / "camera" / preflight_name) or {}
        diagnostics = (
            preflight.get("diagnostics")
            if isinstance(preflight.get("diagnostics"), list)
            else window_camera.get("preflight_diagnostics") or []
        )
        evidence: list[str] = []
        for item in diagnostics:
            if not isinstance(item, dict):
                evidence.append(f"camera preflight: {item}")
                continue
            detail = str(item.get("message") or item.get("code") or "camera evidence failed")
            measured = item.get("measured") if isinstance(item.get("measured"), dict) else {}
            thresholds = item.get("thresholds") if isinstance(item.get("thresholds"), dict) else {}
            evidence.append(
                f"camera preflight {item.get('code') or 'inconclusive'}: {detail}"
                + (f"; measured={measured}" if measured else "")
                + (f"; thresholds={thresholds}" if thresholds else "")
            )
        if not evidence:
            evidence.append(str(window.get("error") or "camera preflight was inconclusive"))
        return {
            "suite": suite,
            "result": "EVIDENCE_FAILED",
            "git_sha": window.get("git_sha", ""),
            "git_ref": window.get("git_ref", ""),
            "product_fingerprint": window.get("product_fingerprint", ""),
            "grader_fingerprint": window.get("grader_fingerprint", ""),
            "scenario_fingerprint": window.get("scenario_fingerprint", ""),
            "git_worktree_clean": window.get("git_worktree_clean") is True,
            "artifact_dir": str(window_dir),
            "camera": {
                "result": "INCONCLUSIVE",
                "capture_result": window_camera.get("result", "CAPTURE_FAILED"),
                "diagnostics": diagnostics,
                "preflight": preflight_name,
                "role": camera_contract["role"],
                "purpose": camera_contract["purpose"],
                "role_summary": camera_contract["summary"],
                "gate_required": camera_contract["gate_required"],
                "evidence_contract": camera_contract,
            },
            "budget_pressure": [],
            "evidence": evidence,
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
    hard_failures = sum(1 for metric in failures if str(metric.get("score_status") or "") == "fail")
    advisory_failures = sum(
        1 for metric in failures if str(metric.get("score_status") or "") == "warn"
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
            f"state={metric.get('absolute_state')} advisory={metric.get('advisory_state', 'n/a')} "
            f"messages={'; '.join(absolute_messages)}"
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

    manifest = load_json(window_path(window_dir, window.get("manifest_path"), "manifest.json")) or {}
    camera_dir = window_dir / "camera"
    camera_path = camera_dir / "camera_result.json"
    window_camera = window.get("camera") if isinstance(window.get("camera"), dict) else {}
    camera = load_json(camera_path) or dict(window_camera)
    camera_contract = camera_evidence_contract(suite)
    camera_grade: dict[str, Any] = {}
    camera_grade_valid = False
    camera_evidence_inconclusive = False
    capture_manifest: dict[str, Any] = {}
    capture_manifest_name = str(window_camera.get("capture_manifest") or CAPTURE_MANIFEST_NAME)
    capture_manifest_path = camera_dir / Path(capture_manifest_name).name
    if capture_manifest_path.is_file():
        try:
            capture_manifest = load_capture_manifest(capture_manifest_path)
            camera = camera_result_view(capture_manifest)
            camera["capture_id"] = capture_manifest.get("capture_id")
        except CameraArtifactError as exc:
            if camera_required:
                camera_evidence_inconclusive = True
                result = worse(result, "EVIDENCE_FAILED")
                evidence.append(f"camera capture ownership is invalid: {exc}")
    if camera_required:
        missing_camera_files: list[str] = []
        if capture_manifest:
            try:
                accepted_identity = agreed_window_identity(window, manifest)
                validate_capture_window_identity(
                    capture_manifest,
                    suite=suite,
                    product_fingerprint=accepted_identity["product_fingerprint"],
                    scenario_fingerprint=accepted_identity["scenario_fingerprint"],
                )
                verify_capture_files(camera_dir, capture_manifest)
            except CameraArtifactError as exc:
                camera_evidence_inconclusive = True
                result = worse(result, "EVIDENCE_FAILED")
                evidence.append(f"camera capture ownership could not be verified: {exc}")
        if capture_manifest and not camera_evidence_inconclusive and camera.get("result") == "CAPTURED":
            for key in ("video", "session_start_still", "bright_still", "dim_still"):
                evidence_path = resolve_manifest_artifact(camera_dir, capture_manifest, key)
                if evidence_path is None or evidence_path.stat().st_size == 0:
                    missing_camera_files.append(key)
        if not capture_manifest:
            camera_evidence_inconclusive = True
            result = worse(result, "EVIDENCE_FAILED")
            if camera.get("result") == "CAPTURE_FAILED":
                camera_errors = camera.get("errors") if isinstance(camera.get("errors"), list) else []
                evidence.append(
                    "gated replay camera evidence was not captured"
                    + (f": {'; '.join(str(item) for item in camera_errors)}" if camera_errors else "")
                )
            else:
                legacy_grade_name = str(
                    window_camera.get("grade") or camera.get("grade") or "camera_grade.json"
                )
                camera_grade = load_json(camera_dir / Path(legacy_grade_name).name) or {}
                evidence.append(
                    "gated replay camera evidence uses legacy ownership; regrade with the current grader"
                )
        elif camera.get("result") != "CAPTURED" or missing_camera_files:
            camera_evidence_inconclusive = True
            result = worse(result, "EVIDENCE_FAILED")
            camera_errors = camera.get("errors") if isinstance(camera.get("errors"), list) else []
            evidence.append(
                "gated replay camera evidence was not captured"
                + (f"; missing files: {', '.join(missing_camera_files)}" if missing_camera_files else "")
                + (f": {'; '.join(str(item) for item in camera_errors)}" if camera_errors else "")
            )
        elif not camera_evidence_inconclusive:
            try:
                camera_grade = load_owned_grade(
                    camera_dir,
                    capture_manifest,
                    CURRENT_GRADER_FINGERPRINT,
                ) or {}
            except CameraArtifactError as exc:
                camera_evidence_inconclusive = True
                result = worse(result, "EVIDENCE_FAILED")
                evidence.append(f"camera grade ownership could not be verified: {exc}")
            if not camera_grade and not camera_evidence_inconclusive:
                camera_evidence_inconclusive = True
                result = worse(result, "EVIDENCE_FAILED")
                evidence.append("gated replay camera evidence has no current-fingerprint mechanical grade")
            elif camera_grade and not camera_evidence_inconclusive:
                camera_grade_valid = True
                grade_result = str(camera_grade.get("result") or "")
                strict_result, strict_messages = strict_grade_outcome(camera_grade)
                if grade_result == "FAIL":
                    if strict_result != "FAIL":
                        camera_evidence_inconclusive = True
                        result = worse(result, "EVIDENCE_FAILED")
                        evidence.append(
                            "replay camera FAIL lacks passed confidence or has camera diagnostics"
                        )
                    else:
                        result = worse(result, "FAIL")
                        failed_checks = [
                            str(name)
                            for name, check in (camera_grade.get("checks") or {}).items()
                            if isinstance(check, dict) and check.get("result") != "PASS"
                        ]
                        evidence.append(
                            "replay camera evidence disagrees with the same-window display log"
                            + (f"; failed checks: {', '.join(failed_checks)}" if failed_checks else "")
                        )
                elif strict_result != "PASS":
                    camera_evidence_inconclusive = True
                    result = worse(result, "EVIDENCE_FAILED")
                    evidence.append(
                        "replay camera evidence is inconclusive"
                        + (f": {'; '.join(strict_messages)}" if strict_messages else "")
                    )
    budget = top_budget_pressures(scoring, catalog)
    return {
        "suite": suite,
        "result": result,
        "git_sha": manifest.get("git_sha", ""),
        "git_ref": manifest.get("git_ref", ""),
        "product_fingerprint": manifest.get("product_fingerprint", ""),
        "grader_fingerprint": manifest.get("grader_fingerprint", ""),
        "scenario_fingerprint": manifest.get("scenario_fingerprint", ""),
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
            "result": (
                "INCONCLUSIVE"
                if camera_evidence_inconclusive
                else (camera_grade.get("result") if camera_grade_valid else "")
            )
            or (
                "UNGRADED"
                if camera.get("result") == "CAPTURED"
                else camera.get("result") or ("MISSING" if camera_required else "")
            ),
            "capture_result": camera.get("result", ""),
            "capture_id": capture_manifest.get("capture_id", "") if capture_manifest else "",
            "grader_fingerprint": (
                camera_grade.get("grader_fingerprint", "") if camera_grade_valid else ""
            ),
            "mechanical_result": camera_grade.get("result", ""),
            "video": camera.get("video", ""),
            "video_duration_seconds": camera.get("video_duration_seconds"),
            "video_probe": camera.get("video_probe", {}),
            "errors": camera.get("errors", []),
            "visually_graded": camera_grade_valid,
            "checks": camera_grade.get("checks", {}),
            "confidence": camera_grade.get("confidence", {}),
            "diagnostics": camera_grade.get("diagnostics", []),
            "role": camera_contract["role"],
            "purpose": camera_contract["purpose"],
            "role_summary": camera_contract["summary"],
            "gate_required": camera_contract["gate_required"],
            "evidence_contract": camera_contract,
        }
        if camera or camera_required
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
    collection_failed = any(
        window.get("result") == "COLLECTION_FAILED" for window in payload["windows"]
    )
    lines.append(f"collection: {'FAIL' if collection_failed else 'PASS'}")
    camera_windows = [window.get("camera", {}) for window in payload["windows"] if window.get("camera")]
    gated_camera = [camera for camera in camera_windows if camera.get("gate_required")]
    if gated_camera:
        if any(
            camera.get("result") in {"INCONCLUSIVE", "MISSING", "CAPTURE_FAILED", "UNGRADED"}
            for camera in gated_camera
        ):
            camera_status = "INCONCLUSIVE"
        elif any(camera.get("result") == "FAIL" for camera in gated_camera):
            camera_status = "FAIL"
        else:
            camera_status = "PASS"
        lines.append(f"camera evidence: {camera_status} (only replay is gated)")
    elif camera_windows:
        lines.append("camera evidence: NOT_GATED (diagnostic/exercise capture only)")
    for window in payload["windows"]:
        detail = f"{window['suite']}: {window['result']}"
        if window.get("rows") is not None:
            detail += f" ({window.get('rows')} rows, {float(window.get('duration_s') or 0):.1f}s)"
        if window.get("v1_emulator", {}).get("mode"):
            detail += f", V1={window['v1_emulator']['mode']}"
        if window.get("replay_checks", {}).get("result"):
            detail += f", replay={window['replay_checks']['result']}"
        camera = window.get("camera", {})
        if camera.get("result"):
            if camera.get("result") == "UNGRADED":
                detail += (
                    f", camera={camera.get('capture_result')} "
                    f"({camera.get('role_summary') or 'not gated'})"
                )
            elif not camera.get("visually_graded") and camera.get("capture_result"):
                detail += (
                    f", camera={camera.get('capture_result')} "
                    f"({camera.get('role_summary') or 'not gated'})"
                )
            else:
                detail += (
                    f", camera={camera['result']} "
                    f"({camera.get('role_summary') or 'not gated'})"
                )
        lines.append(detail)
    failures = [w for w in payload["windows"] if w["result"] != "PASS"]
    if failures:
        lines.append("")
        lines.append("evidence failure:" if payload["result"] == "EVIDENCE_FAILED" else "failed:")
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
    canonical_result_path = (run_dir / "bench_result.json").resolve()
    canonical_summary_path = (run_dir / "bench_summary.txt").resolve()
    out_path = Path(args.out) if args.out else canonical_result_path
    resolved_out_path = out_path.resolve()
    if resolved_out_path == canonical_summary_path:
        sys.stderr.write("error: --out must not resolve to the canonical bench_summary.txt\n")
        return 2

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
    product_fingerprints = {
        str(window.get("product_fingerprint") or "").strip() for window in windows
    }
    grader_fingerprints = {
        str(window.get("grader_fingerprint") or "").strip() for window in windows
    }
    payload = {
        "schema_version": 3,
        "kind": "bench_result",
        "run_dir": str(run_dir),
        "git_sha": next(iter(git_shas)) if len(git_shas) == 1 else "",
        "git_ref": next(iter(git_refs)) if len(git_refs) == 1 else "",
        "product_fingerprint": (
            next(iter(product_fingerprints)) if len(product_fingerprints) == 1 else ""
        ),
        "grader_fingerprint": (
            next(iter(grader_fingerprints)) if len(grader_fingerprints) == 1 else ""
        ),
        "git_worktree_clean": bool(windows)
        and all(window.get("git_worktree_clean") is True for window in windows),
        "result": result,
        "windows": windows,
    }
    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    text = render_text(payload)
    if resolved_out_path == canonical_result_path:
        canonical_summary_path.write_text(text, encoding="utf-8")
    sys.stdout.write(text)
    return EXIT_BY_RESULT[result]


if __name__ == "__main__":
    raise SystemExit(main())
