#!/usr/bin/env python3
"""Regression tests for the bench scorer contract."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCORER = ROOT / "tools" / "bench_score.py"
FULL_SHA = "0123456789abcdef0123456789abcdef01234567"


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def write_window(root: Path, suite: str, *, hard: int = 0, advisory: int = 0, result: str = "NO_BASELINE") -> None:
    step = root / suite
    metric_payload = [
        {
            "metric": "ble_process_max_peak_us",
            "score_status": "pass",
            "absolute_state": "pass",
            "current_value": 60000,
            "unit": "us",
            "messages": [],
        },
        {
            "metric": "display_preview_render_peak_us",
            "score_status": "info",
            "absolute_state": "missing",
            "current_value": None,
            "unit": "us",
            "messages": ["metric missing from run output for applicable track"],
        },
    ]
    if hard:
        metric_payload.append(
            {
                "metric": "queue_drops_delta",
                "score_status": "fail",
                "absolute_state": "fail",
                "current_value": 1,
                "messages": ["value 1 above max 0"],
            }
        )
    write_json(
        step / "window_result.json",
        {
            "schema_version": 1,
            "result": "COLLECTED",
            "suite": suite,
            "git_worktree_clean": True,
            "scoring_path": str(step / "scoring.json"),
            "csv_scorecard_path": str(step / "csv_scorecard.json"),
            "manifest_path": str(step / "manifest.json"),
        },
    )
    write_json(
        step / "manifest.json",
        {
            "schema_version": 1,
            "git_sha": FULL_SHA,
            "git_ref": "dev/test",
            "rows": 61,
            "duration_s": 300.0,
        },
    )
    write_json(
        step / "scoring.json",
        {
            "schema_version": 1,
            "result": result,
            "summary": {
                "metrics_scored": 10,
                "hard_failures": hard,
                "advisory_failures": advisory,
            },
            "metrics": metric_payload,
        },
    )
    write_json(
        step / "csv_scorecard.json",
        {
            "schema_version": 1,
            "result": "PASS" if hard == 0 else "FAIL",
            "hard_failures": 0,
            "advisory_warnings": 0,
            "checks": [],
        },
    )


def run_score(root: Path, *suites: str) -> subprocess.CompletedProcess[str]:
    cmd = [sys.executable, str(SCORER), "--run-dir", str(root)]
    for suite in suites:
        cmd.extend(["--suite", suite])
    return subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, check=False)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_no_baseline_language_does_not_make_bench_fail() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core", result="NO_BASELINE")
        write_window(root, "display", result="NO_BASELINE")
        proc = run_score(root, "core", "display")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        assert_true(result["result"] == "PASS", f"unexpected result: {result}")
        assert_true(result["schema_version"] == 2, f"unexpected schema: {result}")
        assert_true(result["git_sha"] == FULL_SHA, f"missing full Git binding: {result}")
        assert_true(result["git_worktree_clean"] is True, f"dirty binding: {result}")
        assert_true("NO_BASELINE" not in proc.stdout, f"bench output leaked old baseline language: {proc.stdout}")
        assert_true("top budget pressure:" in proc.stdout, f"bench output should surface budget pressure: {proc.stdout}")
        assert_true("ble_process_max_peak_us" in proc.stdout, f"bench output should name top pressure metric: {proc.stdout}")
        assert_true("display_preview_render_peak_us" not in proc.stdout, f"optional missing metrics are not actionable PASS evidence: {proc.stdout}")


def test_core_metric_failure_is_actionable_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core", hard=1, result="FAIL")
        write_window(root, "display")
        proc = run_score(root, "core", "display")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("core.queue_drops_delta" in proc.stdout, proc.stdout)


def test_display_metric_failure_is_actionable_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        write_window(root, "display", hard=1, result="FAIL")
        proc = run_score(root, "core", "display")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("display.queue_drops_delta" in proc.stdout, proc.stdout)


def test_missing_window_artifact_is_collection_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "display")
        proc = run_score(root, "core", "display")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("collection: FAIL" in proc.stdout, proc.stdout)
        assert_true("missing or invalid" in proc.stdout, proc.stdout)


def main() -> int:
    test_no_baseline_language_does_not_make_bench_fail()
    test_core_metric_failure_is_actionable_failure()
    test_display_metric_failure_is_actionable_failure()
    test_missing_window_artifact_is_collection_failure()
    print("bench scorer tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
