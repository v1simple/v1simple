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


def write_replay_csv(path: Path, *, publishes: int = 708) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    exact = {
        "prioritySelectRowFlag": 708,
        "alertTablePublishes": publishes,
        "alertTablePublishes3Bogey": 30,
    }
    zero = [
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
    ]
    columns = ["millis", *exact, *zero]
    start = {column: 0 for column in columns}
    end = {**start, "millis": 300_000, **exact}
    path.write_text(
        ",".join(columns)
        + "\n"
        + ",".join(str(start[column]) for column in columns)
        + "\n"
        + ",".join(str(end[column]) for column in columns)
        + "\n",
        encoding="utf-8",
    )


def write_window(
    root: Path,
    suite: str,
    *,
    hard: int = 0,
    advisory: int = 0,
    result: str = "NO_BASELINE",
    replay_publishes: int = 708,
    replay_completed: bool = True,
    camera_result: str = "",
    camera_grade_result: str = "PASS",
    camera_grade_errors: tuple[str, ...] = (),
) -> None:
    step = root / suite
    metric_payload = [
        {
            "metric": "ble_process_max_peak_us",
            "score_status": "pass",
            "absolute_state": "pass",
            "score_level": "hard",
            "current_value": 60000,
            "unit": "us",
            "messages": [],
        },
        {
            "metric": "display_preview_render_peak_us",
            "score_status": "info",
            "absolute_state": "missing",
            "score_level": "advisory",
            "required": False,
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
                "score_level": "hard",
                "current_value": 1,
                "messages": ["value 1 above max 0"],
            }
        )
    window_payload = {
        "schema_version": 1,
        "result": "COLLECTED",
        "suite": suite,
        "git_worktree_clean": True,
        "scoring_path": str(step / "scoring.json"),
        "manifest_path": str(step / "manifest.json"),
    }
    if suite == "replay":
        csv_path = step / "perf.csv"
        write_replay_csv(csv_path, publishes=replay_publishes)
        window_payload.update(
            {
                "csv_path": str(csv_path),
                "segment": "last",
                "replay": {
                    "started": True,
                    "completed": replay_completed,
                    "returncode": 0 if replay_completed else 2,
                },
            }
        )
    write_json(
        step / "window_result.json",
        window_payload,
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
    if camera_result:
        camera_dir = step / "camera"
        camera_dir.mkdir(parents=True, exist_ok=True)
        evidence_names = {
            "video": "evidence_exp156.mp4",
            "session_start_still": "session_start_exp156.jpg",
            "bright_still": "final_exp5.jpg",
            "dim_still": "final_exp1250.jpg",
        }
        if camera_result == "CAPTURED":
            for name in evidence_names.values():
                (camera_dir / name).write_bytes(b"evidence")
        write_json(
            camera_dir / "camera_result.json",
            {
                "schema_version": 1,
                "result": camera_result,
                **{key: value if camera_result == "CAPTURED" else "" for key, value in evidence_names.items()},
                "video_duration_seconds": 300.0 if camera_result == "CAPTURED" else 0.0,
                "visually_graded": camera_result == "CAPTURED",
                "grade": "camera_grade.json" if camera_result == "CAPTURED" else "",
                "errors": [] if camera_result == "CAPTURED" else ["camera unavailable"],
            },
        )
        if camera_result == "CAPTURED" and camera_grade_result:
            write_json(
                camera_dir / "camera_grade.json",
                {
                    "schema_version": 1,
                    "kind": "bench_camera_grade",
                    "suite": suite,
                    "video": evidence_names["video"],
                    "result": camera_grade_result,
                    "checks": {
                        "display_matches_log": {
                            "result": camera_grade_result,
                            "ratio": 1.0 if camera_grade_result == "PASS" else 0.0,
                        }
                    },
                    "errors": list(camera_grade_errors),
                },
            )


def run_score(
    root: Path,
    *suites: str,
    camera_suites: tuple[str, ...] = (),
) -> subprocess.CompletedProcess[str]:
    cmd = [sys.executable, str(SCORER), "--run-dir", str(root)]
    for suite in suites:
        cmd.extend(["--suite", suite])
    for suite in camera_suites:
        cmd.extend(["--camera-suite", suite])
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
        assert_true(result["schema_version"] == 3, f"unexpected schema: {result}")
        assert_true(result["git_sha"] == FULL_SHA, f"missing full Git binding: {result}")
        assert_true(result["git_worktree_clean"] is True, f"dirty binding: {result}")
        assert_true("NO_BASELINE" not in proc.stdout, f"bench output leaked old baseline language: {proc.stdout}")
        assert_true("top budget pressure:" in proc.stdout, f"bench output should surface budget pressure: {proc.stdout}")
        assert_true("ble_process_max_peak_us" in proc.stdout, f"bench output should name top pressure metric: {proc.stdout}")
        assert_true("display_preview_render_peak_us" not in proc.stdout, f"optional missing metrics are not actionable PASS evidence: {proc.stdout}")


def test_baseline_only_regression_is_comparison_not_verdict() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        scoring_path = root / "core" / "scoring.json"
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["summary"]["hard_failures"] = 1
        scoring["metrics"].append(
            {
                "metric": "parse_successes_delta",
                "score_level": "hard",
                "score_status": "fail",
                "absolute_state": "pass",
                "regression_state": "fail",
                "current_value": 900,
                "messages": ["value regressed below local baseline"],
            }
        )
        write_json(scoring_path, scoring)
        proc = run_score(root, "core")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        assert_true("local baseline" not in proc.stdout, proc.stdout)


def test_required_missing_metric_remains_a_hard_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        scoring_path = root / "core" / "scoring.json"
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["metrics"].append(
            {
                "metric": "queue_drops_delta",
                "score_level": "hard",
                "required": True,
                "score_status": "fail",
                "absolute_state": "missing",
                "current_value": None,
                "messages": ["metric missing from run output"],
            }
        )
        write_json(scoring_path, scoring)
        proc = run_score(root, "core")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("queue_drops_delta" in proc.stdout, proc.stdout)


def test_failed_base_result_remains_a_hard_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        scoring_path = root / "core" / "scoring.json"
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["manifest"] = {"base_result": "FAIL"}
        write_json(scoring_path, scoring)
        proc = run_score(root, "core")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("failed base result" in proc.stdout, proc.stdout)


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


def test_replay_exact_invariants_are_part_of_the_verdict() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        checks = result["windows"][0]["replay_checks"]
        assert_true(checks["result"] == "PASS", f"unexpected replay checks: {checks}")
        assert_true(checks["observed_deltas"]["alertTablePublishes"] == 708, f"wrong replay deltas: {checks}")


def test_replay_scores_complete_window_across_connection_sessions() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        csv_path = root / "replay" / "perf.csv"
        columns = [
            "millis",
            "prioritySelectRowFlag",
            "alertTablePublishes",
            "alertTablePublishes3Bogey",
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
        ]

        def row(millis: int, publishes: int, three: int) -> str:
            values = {column: 0 for column in columns}
            values.update(
                {
                    "millis": millis,
                    "prioritySelectRowFlag": publishes,
                    "alertTablePublishes": publishes,
                    "alertTablePublishes3Bogey": three,
                }
            )
            return ",".join(str(values[column]) for column in columns)

        csv_path.write_text(
            ",".join(columns)
            + "\n#session_start,seq=1,uptime_ms=0,token=FIRST,schema=45\n"
            + row(0, 0, 0)
            + "\n"
            + row(80_000, 200, 30)
            + "\n"
            + ",".join(columns)
            + "\n#session_start,seq=2,uptime_ms=85000,token=SECOND,schema=45\n"
            + row(85_000, 200, 30)
            + "\n"
            + row(300_000, 708, 30)
            + "\n",
            encoding="utf-8",
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        checks = result["windows"][0]["replay_checks"]
        assert_true(checks["session_count"] == 2, f"wrong session scope: {checks}")
        assert_true(checks["observed_deltas"]["alertTablePublishes3Bogey"] == 30, f"wrong deltas: {checks}")


def test_replay_mismatch_is_actionable_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay", replay_publishes=707)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("alertTablePublishes delta=707 expected=708" in proc.stdout, proc.stdout)


def test_replay_process_failure_is_collection_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay", replay_completed=False)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("v1replay did not complete successfully" in proc.stdout, proc.stdout)


def test_managed_emulator_must_cover_every_live_window() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        window_path = root / "core" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["v1_emulator"] = {
            "mode": "idle",
            "completed": False,
            "managed_stop": True,
        }
        write_json(window_path, window)
        proc = run_score(root, "core")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("managed V1 emulator did not cover" in proc.stdout, proc.stdout)


def test_requested_replay_camera_separates_product_and_evidence_failures() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay", camera_result="CAPTURED")
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        assert_true("camera evidence: PASS (only replay is gated)" in proc.stdout, proc.stdout)
        assert_true("camera=PASS (gated replay validator)" in proc.stdout, proc.stdout)

        write_window(root, "replay", camera_result="CAPTURED", camera_grade_result="FAIL")
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("replay camera evidence disagrees" in proc.stdout, proc.stdout)

        write_window(
            root,
            "replay",
            camera_result="CAPTURED",
            camera_grade_result="INCONCLUSIVE",
            camera_grade_errors=("camera registration failed",),
        )
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("bench result: EVIDENCE_FAILED" in proc.stdout, proc.stdout)
        assert_true("collection: PASS" in proc.stdout, proc.stdout)
        assert_true("camera evidence: INCONCLUSIVE" in proc.stdout, proc.stdout)
        assert_true("replay camera evidence is inconclusive" in proc.stdout, proc.stdout)

        write_window(
            root,
            "replay",
            hard=1,
            camera_result="CAPTURED",
            camera_grade_result="INCONCLUSIVE",
            camera_grade_errors=("camera registration failed",),
        )
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("bench result: FAIL" in proc.stdout, proc.stdout)
        assert_true("camera evidence: INCONCLUSIVE" in proc.stdout, proc.stdout)

        write_window(root, "replay", camera_result="CAPTURED")
        (root / "replay" / "camera" / "camera_grade.json").unlink()
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("bench result: EVIDENCE_FAILED" in proc.stdout, proc.stdout)
        assert_true("has no mechanical grade" in proc.stdout, proc.stdout)

        (root / "replay" / "camera" / "camera_result.json").unlink()
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("gated replay camera evidence was not captured" in proc.stdout, proc.stdout)


def test_only_replay_camera_grade_is_required_by_the_full_bench() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core", camera_result="CAPTURED", camera_grade_result="FAIL")
        write_window(root, "display", camera_result="CAPTURED", camera_grade_result="FAIL")
        write_window(root, "replay", camera_result="CAPTURED", camera_grade_result="PASS")
        proc = run_score(
            root,
            "core",
            "display",
            "replay",
            camera_suites=("replay",),
        )
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        assert_true(
            "core: PASS" in proc.stdout and "camera=CAPTURED (diagnostic only)" in proc.stdout,
            proc.stdout,
        )
        assert_true(
            "display: PASS" in proc.stdout and "camera=CAPTURED (exercise only)" in proc.stdout,
            proc.stdout,
        )
        assert_true(
            "replay: PASS" in proc.stdout and "camera=PASS (gated replay validator)" in proc.stdout,
            proc.stdout,
        )


def main() -> int:
    test_no_baseline_language_does_not_make_bench_fail()
    test_baseline_only_regression_is_comparison_not_verdict()
    test_required_missing_metric_remains_a_hard_failure()
    test_failed_base_result_remains_a_hard_failure()
    test_core_metric_failure_is_actionable_failure()
    test_display_metric_failure_is_actionable_failure()
    test_missing_window_artifact_is_collection_failure()
    test_replay_exact_invariants_are_part_of_the_verdict()
    test_replay_scores_complete_window_across_connection_sessions()
    test_replay_mismatch_is_actionable_failure()
    test_replay_process_failure_is_collection_failure()
    test_managed_emulator_must_cover_every_live_window()
    test_requested_replay_camera_separates_product_and_evidence_failures()
    test_only_replay_camera_grade_is_required_by_the_full_bench()
    print("bench scorer tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
