#!/usr/bin/env python3
"""Focused regressions for the advisory bench function hunter."""

from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
from contextlib import redirect_stderr
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import bench_hunt  # noqa: E402
from bench_score import EXPECTED_REPLAY_CHECKPOINTS  # noqa: E402
from camera_artifacts import (  # noqa: E402
    build_capture_manifest,
    capture_input_hashes,
    publish_capture_manifest,
    publish_grade,
)
from run_window import (  # noqa: E402
    DISPLAY_COMMIT_HEADER,
    DISPLAY_COMMIT_METADATA_LINE,
    summarize_display_commit_artifact,
)


SHA = "1" * 40
PRODUCT = "a" * 64
SCENARIO = "b" * 64
GRADER = "c" * 64
SCORER = "d" * 64


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def comparison(*, mismatch: bool = False, abstain: bool = False) -> dict:
    observed_frequency = None if abstain else (34700 if mismatch else 24150)
    return {
        "encounter_id": 1,
        "event": "SAMPLE",
        "replay_time_seconds": 6.0,
        "video_consensus_window": {
            "center_seconds": 8.0,
            "first_sample_seconds": 7.333,
            "last_sample_seconds": 8.667,
        },
        "sample_count": 5,
        "visible_sample_count": 5,
        "expected": {
            "alert_visible": True,
            "frequency_mhz": 24150,
            "direction": "FRONT",
        },
        "observed": {
            "alert_visible": True,
            "frequency_mhz": observed_frequency,
            "direction": "FRONT",
        },
        "consensus_ratio": {
            "alert_visible": 1.0,
            "frequency_mhz": 0.0 if abstain else 1.0,
            "direction": 1.0,
        },
        "outcome": {
            "alert_visible": {"state": "match"},
            "frequency_mhz": (
                {"state": "abstain", "reason": "no_consensus"}
                if abstain
                else (
                    {"state": "mismatch", "reason": "observed_differs_from_expected"}
                    if mismatch
                    else {"state": "match"}
                )
            ),
            "direction": {"state": "match"},
        },
    }


def fixture(
    root: Path,
    *,
    comparisons: list[dict] | None = None,
    retain_comparisons: bool = True,
    own_display: bool = True,
    encounter_drops: int = 0,
    display_drops: int = 0,
    failed_metric: bool = False,
) -> Path:
    run_dir = root / "run"
    replay_dir = run_dir / "replay"
    camera_dir = replay_dir / "camera"
    camera_dir.mkdir(parents=True)

    identity = {
        "schema_version": 2,
        "kind": "bench_identity",
        "product_fingerprint": PRODUCT,
        "scenario_fingerprint": SCENARIO,
        "grader_fingerprint": GRADER,
        "hardware_scoring_fingerprint": SCORER,
        "traceability": {"repository_sha": SHA, "worktree_clean": True},
    }
    write_json(replay_dir / "identity.json", identity)

    perf = replay_dir / "perf_boot_1-token.csv"
    perf.write_text(
        "millis,rx\n"
        "#session_start,seq=1,bootId=1,uptime_ms=100,token=TOKEN001,schema=46\n"
        "100,1\n200,2\n",
        encoding="utf-8",
    )
    write_json(
        replay_dir / "manifest.json",
        {
            "schema_version": 1,
            "run_id": "fixture",
            "git_sha": SHA,
            "source_input": str(perf),
            "source_schema": 46,
            "selected_segment": {
                "session_index": 1,
                "token": "TOKEN001",
                "schema": 46,
                "row_count": 2,
            },
        },
    )

    encounter = replay_dir / "encounters_1-token.csv"
    encounter_rows = [
        "100,1,1,START,1,1,K,24150,FRONT,128,0,1,0,1,0,0,0",
        f"200,1,2,SAMPLE,1,1,K,24150,FRONT,140,0,2,0,1,0,0,{encounter_drops}",
    ]
    millis = 300
    for sample_seq, (_label, signature) in enumerate(EXPECTED_REPLAY_CHECKPOINTS, 1):
        event = "START" if sample_seq == 1 else "SAMPLE"
        for v1_index, band, frequency, direction, front_bars, rear_bars, priority in signature:
            encounter_rows.append(
                f"{millis},2,{sample_seq},{event},{v1_index},{len(signature)},{band},{frequency},{direction},"
                f"0,0,{front_bars},{rear_bars},{priority},0,0,0"
            )
        millis += 100
    final_signature = EXPECTED_REPLAY_CHECKPOINTS[-1][1]
    for v1_index, band, frequency, direction, front_bars, rear_bars, priority in final_signature:
        encounter_rows.append(
            f"{millis},2,{len(EXPECTED_REPLAY_CHECKPOINTS) + 1},END,{v1_index},{len(final_signature)},"
            f"{band},{frequency},{direction},0,0,{front_bars},{rear_bars},{priority},0,0,0"
        )
    encounter.write_text(
        "# encounter_schema=1,timebase=millis,v1_assignments=raw,no_gps=1,no_speed=1\n"
        "millis,encounter_id,sample_seq,event,v1_index,alert_count,band,frequency_mhz,direction,front_raw,rear_raw,front_bars,rear_bars,priority,junk,photo_type,dropped_snapshots\n"
        + "\n".join(encounter_rows)
        + "\n",
        encoding="utf-8",
    )

    display = replay_dir / "display_commits_1-token.csv"
    row = ["0"] * len(DISPLAY_COMMIT_HEADER)
    row[0], row[1], row[2], row[3], row[-1] = "1", "150", "LIVE", "NONE", str(display_drops)
    display.write_text(
        DISPLAY_COMMIT_METADATA_LINE
        + "\n"
        + ",".join(DISPLAY_COMMIT_HEADER)
        + "\n"
        + ",".join(row)
        + f"\n# display_commit_export_schema=1,terminal_seq=1,dropped_commits={display_drops}\n",
        encoding="utf-8",
    )
    display_summary = summarize_display_commit_artifact(display, replay_dir)

    names = {
        "video": "evidence.mov",
        "session_start_still": "start.jpg",
        "bright_still": "bright.jpg",
        "dim_still": "dim.jpg",
    }
    for name in names.values():
        (camera_dir / name).write_bytes(name.encode("ascii"))
    camera_result = {
        "schema_version": 2,
        "kind": "bench_camera_evidence",
        "result": "CAPTURED",
        "camera_name": "fixture",
        "camera_device_index": 0,
        "profile": {},
        "expected_duration_seconds": 300,
        "video_duration_seconds": 300.0,
        "profile_validation": {"result": "PASS"},
        "errors": [],
        **names,
    }
    capture = build_capture_manifest(
        camera_dir=camera_dir,
        camera_result=camera_result,
        suite="replay",
        product_fingerprint=PRODUCT,
        scenario_fingerprint=SCENARIO,
        encounter_csv_path=encounter,
        timing_anchor={"kind": "first_emitted_replay_sample", "video_seconds": 2.0},
        traceability={"repository_sha": SHA},
    )
    publish_capture_manifest(camera_dir, capture)
    grade = {
        "schema_version": 4,
        "kind": "bench_camera_grade",
        "capture_id": capture["capture_id"],
        "grader_fingerprint": GRADER,
        "grade_id": hashlib.sha256(f"{capture['capture_id']}:{GRADER}".encode("ascii")).hexdigest(),
        "input_hashes": capture_input_hashes(capture),
        "suite": "replay",
        "result": "PASS",
        "confidence": {"result": "PASS", "gates": {}},
        "checks": {},
        "diagnostics": [],
        "errors": [],
        "video": names["video"],
    }
    if retain_comparisons:
        grade["encounter_comparisons"] = comparisons if comparisons is not None else [comparison()]
    publish_grade(camera_dir, capture, GRADER, grade)

    metric = {
        "git_sha": SHA,
        "metric": "notify_to_display_max_ms",
        "run_id": "fixture",
        "run_kind": "real_fw_soak",
        "sample": "value",
        "schema_version": 1,
        "suite_or_profile": "drive_wifi_off",
        "tags": {},
        "unit": "ms",
        "value": 113.0 if failed_metric else 28.0,
    }
    (replay_dir / "metrics.ndjson").write_text(json.dumps(metric) + "\n", encoding="utf-8")
    score_metric = {
        "metric": metric["metric"],
        "unit": "ms",
        "current_value": metric["value"],
        "score_status": "fail" if failed_metric else "pass",
        "messages": ["value 113 above max 100"] if failed_metric else [],
    }
    write_json(
        replay_dir / "scoring.json",
        {
            "schema_version": 1,
            "manifest": {
                "path": str(replay_dir / "manifest.json"),
                "run_id": "fixture",
                "git_sha": SHA,
                "hardware_scoring_fingerprint": SCORER,
                "selected_segment": {
                    "session_index": 1,
                    "token": "TOKEN001",
                    "schema": 46,
                    "row_count": 2,
                },
            },
            "metrics": [score_metric],
        },
    )

    window = {
        "schema_version": 3,
        "suite": "replay",
        "result": "COLLECTED",
        "git_sha": SHA,
        "product_fingerprint": PRODUCT,
        "scenario_fingerprint": SCENARIO,
        "hardware_scoring_fingerprint": SCORER,
        "identity_manifest": "identity.json",
        "csv_path": str(perf),
        "encounter_csv_path": str(encounter),
        "manifest_path": str(replay_dir / "manifest.json"),
        "scoring_path": str(replay_dir / "scoring.json"),
        "grader_fingerprint": GRADER,
        "camera": {
            "capture_manifest": "capture_manifest.json",
            "grade": f"grades/{GRADER}.json",
            "grade_result": "PASS",
        },
        "artifacts": {"display_commits": display_summary} if own_display else {},
    }
    write_json(replay_dir / "window_result.json", window)
    write_json(
        run_dir / "bench_result.json",
        {"schema_version": 4, "kind": "bench_result", "git_sha": SHA, "result": "PASS", "windows": []},
    )
    return run_dir


def file_hashes(run_dir: Path) -> dict[str, str]:
    return {
        path.relative_to(run_dir).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in run_dir.rglob("*")
        if path.is_file() and path.name != "hunt.json"
    }


def test_complete_report_is_deterministic_and_advisory() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp))
        before = file_hashes(run_dir)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["verdict_effect"] == "none", "hunter gained verdict authority")
        assert_true(report["run"]["git_sha"] == SHA, "agreed run identity was lost")
        assert_true(
            all(item["status"] == "complete" for item in report["evidence"].values()),
            f"complete fixture became partial: {report['evidence']}",
        )
        ids = {item["id"] for item in report["findings"]}
        assert_true(
            ids == {"renderer-camera-correlation-unmeasured", "transition-latency-unmeasured"},
            f"matching evidence produced noise: {ids}",
        )
        first_path, created = bench_hunt.publish_report(run_dir)
        first = first_path.read_bytes()
        second_path, created_again = bench_hunt.publish_report(run_dir)
        assert_true(created and not created_again and first == second_path.read_bytes(), "report is not deterministic")
        assert_true(before == file_hashes(run_dir), "hunter modified source evidence")
        assert_true(str(run_dir) not in first.decode("utf-8"), "report leaked an absolute run path")


def test_partial_run_points_to_failures_without_inventing_correlation() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(
            Path(temp),
            retain_comparisons=False,
            own_display=False,
            encounter_drops=2,
            failed_metric=True,
        )
        report = bench_hunt.build_report(run_dir)
        by_id = {item["id"]: item for item in report["findings"]}
        assert_true(by_id["metric-threshold-notify_to_display_max_ms"]["state"] == "confirmed", "score failure was hidden")
        assert_true(by_id["encounter-snapshots-dropped"]["state"] == "confirmed", "reported encounter loss was hidden")
        assert_true(by_id["evidence-encounters"]["state"] == "unknown", "encounter loss became success")
        assert_true(by_id["evidence-display_commits"]["state"] == "unknown", "missing renderer evidence became success")
        assert_true(by_id["evidence-camera_grade"]["state"] == "unknown", "legacy grade became complete")
        assert_true("renderer-camera-correlation-unmeasured" not in by_id, "missing streams were correlated")


def test_malformed_performance_is_unknown() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp))
        (run_dir / "replay" / "perf_boot_1-token.csv").write_text(
            "not,a,performance,csv\n", encoding="utf-8"
        )
        report = bench_hunt.build_report(run_dir)
        performance = report["evidence"]["performance"]
        assert_true(performance["status"] == "partial", "malformed performance was trusted")
        assert_true(performance["reason"] == "selected_segment_mismatch", "wrong performance reason")
        findings = {item["id"]: item for item in report["findings"]}
        assert_true(findings["evidence-performance"]["state"] == "unknown", "malformed performance confirmed")

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp))
        (run_dir / "replay" / "perf_boot_1-token.csv").unlink()
        (run_dir / "replay" / "manifest.json").unlink()
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["performance"]["status"] == "missing", "fully absent performance became partial")


def test_reported_renderer_loss_is_confirmed_but_incomplete() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), display_drops=3)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["display_commits"]["status"] == "partial", "renderer loss looked complete")
        findings = {item["id"]: item for item in report["findings"]}
        assert_true(findings["renderer-commits-dropped"]["state"] == "confirmed", "reported renderer loss was hidden")
        assert_true(findings["evidence-display_commits"]["state"] == "unknown", "missing commits were reconstructed")


def test_stale_scoring_and_unsupported_owner_schemas_are_unknown() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), failed_metric=True)
        replay_dir = run_dir / "replay"
        scoring_path = replay_dir / "scoring.json"
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["manifest"]["git_sha"] = "f" * 40
        write_json(scoring_path, scoring)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["scoring"]["reason"] == "manifest_identity_mismatch", "stale score was owned")
        assert_true(
            not any(item["id"].startswith("metric-threshold-") for item in report["findings"]),
            "stale score produced a confirmed metric",
        )

        bench_path = run_dir / "bench_result.json"
        bench = json.loads(bench_path.read_text(encoding="utf-8"))
        bench["schema_version"] = 999
        write_json(bench_path, bench)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["bench_result"]["reason"] == "unsupported_schema", "foreign bench result was owned")

        window_path = replay_dir / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["schema_version"] = 999
        write_json(window_path, window)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["window_result"]["reason"] == "unsupported_schema", "foreign window was owned")

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), failed_metric=True)
        manifest_path = run_dir / "replay" / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["schema_version"] = 999
        write_json(manifest_path, manifest)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["performance"]["reason"] == "manifest_unsupported_schema", "foreign manifest was owned")
        assert_true(report["evidence"]["scoring"]["status"] == "partial", "score outlived its foreign manifest")

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp))
        scoring_path = run_dir / "replay" / "scoring.json"
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["metrics"][0]["score_status"] = "info"
        write_json(scoring_path, scoring)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["scoring"]["status"] == "complete", "valid informational score was rejected")


def test_camera_mismatch_and_abstention_keep_exact_video_refs() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True), comparison(abstain=True)])
        grade_path = run_dir / "replay" / "camera" / "grades" / f"{GRADER}.json"
        grade = json.loads(grade_path.read_text(encoding="utf-8"))
        grade["video"] = "bright.jpg"
        write_json(grade_path, grade)
        report = bench_hunt.build_report(run_dir)
        by_id = {item["id"]: item for item in report["findings"]}
        mismatch = by_id["camera-mismatch-0001"]
        abstain = by_id["camera-abstention-0002"]
        assert_true(mismatch["state"] == "confirmed", "owned visual mismatch was weakened")
        assert_true(abstain["state"] == "unknown", "camera abstention became a failure")
        assert_true(
            "replay/camera/evidence.mov#t=7.333-8.667" in mismatch["evidence_refs"],
            f"camera finding lost its video interval: {mismatch}",
        )
        assert_true(mismatch["code_refs"] == [] and abstain["code_refs"] == [], "hunter guessed code owners")


def test_malformed_camera_and_scoring_records_cannot_confirm_findings() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), failed_metric=True)
        replay_dir = run_dir / "replay"
        grade_path = replay_dir / "camera" / "grades" / f"{GRADER}.json"
        grade = json.loads(grade_path.read_text(encoding="utf-8"))
        grade["encounter_comparisons"] = [
            {"outcome": {"frequency_mhz": {"state": "mismatch"}}}
        ]
        write_json(grade_path, grade)
        write_json(
            replay_dir / "scoring.json",
            {
                "schema_version": 1,
                "manifest": {"hardware_scoring_fingerprint": SCORER},
                "metrics": [{"score_status": "fail"}],
            },
        )

        report = bench_hunt.build_report(run_dir)
        assert_true(
            report["evidence"]["camera_grade"]["reason"] == "comparison_invalid:0",
            "malformed camera record became complete",
        )
        assert_true(
            report["evidence"]["scoring"]["reason"] == "metric_invalid:0",
            "malformed scoring record became complete",
        )
        ids = {item["id"] for item in report["findings"]}
        assert_true(not any(item.startswith("camera-mismatch-") for item in ids), "malformed camera mismatch was confirmed")
        assert_true(not any(item.startswith("metric-threshold-") for item in ids), "malformed metric failure was confirmed")


def test_camera_claims_require_owned_intervals_and_sources() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True)])
        grade_path = run_dir / "replay" / "camera" / "grades" / f"{GRADER}.json"
        grade = json.loads(grade_path.read_text(encoding="utf-8"))
        grade["encounter_comparisons"][0]["sample_count"] = 0
        grade["encounter_comparisons"][0]["video_consensus_window"] = {
            "center_seconds": 9999.0,
            "first_sample_seconds": 9998.0,
            "last_sample_seconds": 10000.0,
        }
        write_json(grade_path, grade)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["camera_grade"]["reason"] == "comparison_invalid:0", "impossible video interval was trusted")
        assert_true(
            not any(item["id"].startswith("camera-mismatch-") for item in report["findings"]),
            "impossible video interval confirmed a mismatch",
        )

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True)])
        grade_path = run_dir / "replay" / "camera" / "grades" / f"{GRADER}.json"
        grade = json.loads(grade_path.read_text(encoding="utf-8"))
        grade["encounter_comparisons"][0]["consensus_ratio"]["frequency_mhz"] = 0.0
        write_json(grade_path, grade)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["camera_grade"]["reason"] == "comparison_invalid:0", "low-consensus mismatch was trusted")

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True)])
        replay_dir = run_dir / "replay"
        alternate = replay_dir / "alternate_encounters.csv"
        alternate.write_bytes((replay_dir / "encounters_1-token.csv").read_bytes())
        window_path = replay_dir / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["encounter_csv_path"] = str(alternate)
        write_json(window_path, window)
        report = bench_hunt.build_report(run_dir)
        assert_true(
            report["evidence"]["camera_grade"]["reason"] == "encounter_reference_mismatch",
            "camera and window used different encounter owners",
        )

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True)])
        camera_dir = run_dir / "replay" / "camera"
        capture_path = camera_dir / "capture_manifest.json"
        capture = json.loads(capture_path.read_text(encoding="utf-8"))
        capture["capture"]["video_duration_seconds"] = 10000.0
        write_json(capture_path, capture)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["camera_grade"]["reason"] == "capture_summary_mismatch", "unowned video duration was trusted")

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True)])
        window_path = run_dir / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["camera"]["capture_id"] = "e" * 64
        write_json(window_path, window)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["camera_grade"]["reason"] == "window_summary_mismatch", "camera owner summary was ignored")

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True)])
        grade_path = run_dir / "replay" / "camera" / "grades" / f"{GRADER}.json"
        grade = json.loads(grade_path.read_text(encoding="utf-8"))
        grade["encounter_comparisons"][0]["expected"]["direction"] = "SENSITIVE_SENTINEL"
        write_json(grade_path, grade)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["camera_grade"]["reason"] == "comparison_invalid:0", "foreign direction was trusted")
        assert_true("SENSITIVE_SENTINEL" not in json.dumps(report), "foreign direction leaked private text")


def test_inconclusive_camera_and_foreign_scoring_stay_unknown_and_private() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True)], failed_metric=True)
        replay_dir = run_dir / "replay"
        grade_path = replay_dir / "camera" / "grades" / f"{GRADER}.json"
        grade = json.loads(grade_path.read_text(encoding="utf-8"))
        grade["result"] = "INCONCLUSIVE"
        grade["confidence"]["result"] = "INCONCLUSIVE"
        grade["diagnostics"] = [{"code": "camera_uncertain", "message": "camera uncertain"}]
        write_json(grade_path, grade)
        write_json(
            replay_dir / "scoring.json",
            {
                "schema_version": 999,
                "manifest": {"hardware_scoring_fingerprint": "e" * 64},
                "metrics": [
                    {
                        "metric": "notify_to_display_max_ms",
                        "score_status": "fail",
                        "current_value": 113,
                        "unit": "ms",
                        "messages": ["SENSITIVE_SENTINEL"],
                    }
                ],
            },
        )
        identity_path = replay_dir / "identity.json"
        identity = json.loads(identity_path.read_text(encoding="utf-8"))
        identity["product_fingerprint"] = "f" * 64
        write_json(identity_path, identity)

        report = bench_hunt.build_report(run_dir)
        ids = {item["id"] for item in report["findings"]}
        assert_true(report["evidence"]["camera_grade"]["reason"] == "grade_inconclusive", "inconclusive grade became complete")
        assert_true(report["evidence"]["scoring"]["reason"] == "unsupported_schema", "foreign scoring became complete")
        assert_true("evidence-identity-disagreement" in ids, "product identity mismatch was ignored")
        assert_true(not any(item.startswith("camera-mismatch-") for item in ids), "inconclusive mismatch was confirmed")
        assert_true(not any(item.startswith("metric-threshold-") for item in ids), "foreign metric was confirmed")
        assert_true("SENSITIVE_SENTINEL" not in json.dumps(report), "raw private scoring text leaked")
        assert_true("camera-diagnostic-001" in ids, "owned inconclusive diagnostic was discarded")


def test_outside_artifact_is_rejected_and_existing_report_is_immutable() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        run_dir = fixture(root)
        outside = root / "outside.csv"
        outside.write_text("not evidence\n", encoding="utf-8")
        window_path = run_dir / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["artifacts"]["display_commits"]["path"] = str(outside)
        window["manifest_path"] = str(outside)
        write_json(window_path, window)
        report = bench_hunt.build_report(run_dir)
        display = report["evidence"]["display_commits"]
        assert_true(display["status"] == "partial" and display["reason"] == "outside_run", "outside file was adopted")
        performance = report["evidence"]["performance"]
        assert_true(
            performance["status"] == "partial" and performance["reason"] == "manifest_outside_run",
            "adjacent performance manifest was adopted",
        )
        assert_true(str(outside) not in json.dumps(report), "outside path leaked into report")

        bench_hunt.publish_report(run_dir)
        scoring_path = run_dir / "replay" / "scoring.json"
        write_json(scoring_path, {"metrics": []})
        try:
            bench_hunt.publish_report(run_dir)
        except bench_hunt.HuntError:
            pass
        else:
            raise AssertionError("existing hunt.json was silently overwritten")


def test_missing_owned_video_is_named_without_trusting_the_old_grade() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp))
        (run_dir / "replay" / "camera" / "evidence.mov").unlink()
        report = bench_hunt.build_report(run_dir)
        camera = report["evidence"]["camera_grade"]
        assert_true(
            camera["status"] == "partial" and camera["reason"] == "capture_input_missing:video",
            f"missing video cause was collapsed: {camera}",
        )
        finding_by_id = {item["id"]: item for item in report["findings"]}
        assert_true(
            "capture_input_missing:video" in finding_by_id["evidence-camera_grade"]["observed"],
            "hunter did not surface the missing owned video",
        )


def test_symlinked_owner_and_capture_bytes_outside_run_are_rejected() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        run_dir = fixture(root)
        bench_path = run_dir / "bench_result.json"
        outside_bench = root / "outside_bench.json"
        outside_bench.write_bytes(bench_path.read_bytes())
        bench_path.unlink()
        try:
            bench_path.symlink_to(outside_bench)
        except (OSError, NotImplementedError):
            return
        report = bench_hunt.build_report(run_dir)
        assert_true(
            report["evidence"]["bench_result"]["status"] == "partial"
            and report["evidence"]["bench_result"]["reason"] == "outside_run",
            "symlinked bench owner escaped containment",
        )

        video = run_dir / "replay" / "camera" / "evidence.mov"
        outside_video = root / "outside_video.mov"
        video.rename(outside_video)
        video.symlink_to(outside_video)
        report = bench_hunt.build_report(run_dir)
        assert_true(
            report["evidence"]["camera_grade"]["status"] == "partial"
            and report["evidence"]["camera_grade"]["reason"] == "capture_input_outside:video",
            "symlinked capture bytes escaped containment",
        )
        assert_true(str(outside_video) not in json.dumps(report), "outside capture path leaked")
        finding_by_id = {item["id"]: item for item in report["findings"]}
        assert_true(
            "capture_input_outside:video" in finding_by_id["evidence-camera_grade"]["observed"],
            "hunter did not surface the outside owned video",
        )


def test_variable_owned_paths_are_used_in_finding_references() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True)], failed_metric=True)
        replay_dir = run_dir / "replay"
        renames = {
            "identity.json": "owned_identity.json",
            "manifest.json": "performance_manifest.json",
            "scoring.json": "score_output.json",
        }
        for source, destination in renames.items():
            (replay_dir / source).rename(replay_dir / destination)
        identity_path = replay_dir / renames["identity.json"]
        identity = json.loads(identity_path.read_text(encoding="utf-8"))
        identity["traceability"]["repository_sha"] = "2" * 40
        write_json(identity_path, identity)
        window_path = replay_dir / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["identity_manifest"] = renames["identity.json"]
        window["manifest_path"] = str(replay_dir / renames["manifest.json"])
        window["scoring_path"] = str(replay_dir / renames["scoring.json"])
        write_json(window_path, window)
        scoring_path = replay_dir / renames["scoring.json"]
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["manifest"]["path"] = str(replay_dir / renames["manifest.json"])
        write_json(scoring_path, scoring)

        report = bench_hunt.build_report(run_dir)
        by_id = {item["id"]: item for item in report["findings"]}
        metric_refs = by_id["metric-threshold-notify_to_display_max_ms"]["evidence_refs"]
        identity_refs = by_id["evidence-identity-disagreement"]["evidence_refs"]
        assert_true("replay/score_output.json#/metrics/0" in metric_refs, "metric ref ignored its owner")
        assert_true("replay/owned_identity.json" in identity_refs, "identity ref ignored its owner")
        assert_true("replay/performance_manifest.json" in identity_refs, "manifest ref ignored its owner")
        assert_true(by_id["metric-threshold-notify_to_display_max_ms"]["state"] == "unknown", "identity mismatch confirmed a metric")
        assert_true(by_id["camera-mismatch-0001"]["state"] == "unknown", "identity mismatch confirmed a camera claim")


def test_code_oracle_identity_and_renderer_summary_are_truthful() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp))
        encounter_path = run_dir / "replay" / "encounters_1-token.csv"
        payload = encounter_path.read_text(encoding="utf-8").replace(
            "100,1,1,START,1,1,K,24150,FRONT,128,0,1,0,1,0,0,0",
            "100,1,1,START,1,1,K,24150,FRONT,128,0,1,0,2,0,0,0",
        )
        encounter_path.write_text(payload, encoding="utf-8")
        report = bench_hunt.build_report(run_dir)
        findings = {item["id"]: item for item in report["findings"]}
        assert_true(findings["encounter-code-oracle-mismatch"]["state"] == "confirmed", "code-oracle mismatch disappeared")

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp))
        window_path = run_dir / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["artifacts"]["display_commits"]["terminal_sequence"] = 999
        write_json(window_path, window)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["evidence"]["display_commits"]["reason"] == "window_summary_mismatch", "renderer owner summary was ignored")

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp), comparisons=[comparison(mismatch=True)], failed_metric=True)
        (run_dir / "replay" / "identity.json").unlink()
        report = bench_hunt.build_report(run_dir)
        findings = {item["id"]: item for item in report["findings"]}
        assert_true(findings["metric-threshold-notify_to_display_max_ms"]["state"] == "unknown", "missing identity confirmed metric")
        assert_true(findings["camera-mismatch-0001"]["state"] == "unknown", "missing identity confirmed camera claim")

    with tempfile.TemporaryDirectory() as temp:
        run_dir = fixture(Path(temp))
        private_value = "SENSITIVE_SENTINEL"
        bench_path = run_dir / "bench_result.json"
        window_path = run_dir / "replay" / "window_result.json"
        identity_path = run_dir / "replay" / "identity.json"
        manifest_path = run_dir / "replay" / "manifest.json"
        for path in (bench_path, window_path, manifest_path):
            value = json.loads(path.read_text(encoding="utf-8"))
            value["git_sha"] = private_value
            write_json(path, value)
        identity = json.loads(identity_path.read_text(encoding="utf-8"))
        identity["traceability"]["repository_sha"] = private_value
        write_json(identity_path, identity)
        report = bench_hunt.build_report(run_dir)
        assert_true(report["run"]["git_sha"] == "unknown", "invalid SHA became run identity")
        assert_true(private_value not in json.dumps(report), "invalid SHA leaked private text")


def test_cli_failure_does_not_echo_private_paths() -> None:
    with tempfile.TemporaryDirectory() as temp:
        missing = Path(temp) / "private-name" / "missing-run"
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            result = bench_hunt.main(["--run-dir", str(missing)])
        assert_true(result == 2, "missing run root succeeded")
        assert_true(str(missing) not in stderr.getvalue(), "CLI leaked the private run path")


def main() -> int:
    test_complete_report_is_deterministic_and_advisory()
    test_partial_run_points_to_failures_without_inventing_correlation()
    test_malformed_performance_is_unknown()
    test_reported_renderer_loss_is_confirmed_but_incomplete()
    test_stale_scoring_and_unsupported_owner_schemas_are_unknown()
    test_camera_mismatch_and_abstention_keep_exact_video_refs()
    test_malformed_camera_and_scoring_records_cannot_confirm_findings()
    test_camera_claims_require_owned_intervals_and_sources()
    test_inconclusive_camera_and_foreign_scoring_stay_unknown_and_private()
    test_outside_artifact_is_rejected_and_existing_report_is_immutable()
    test_missing_owned_video_is_named_without_trusting_the_old_grade()
    test_symlinked_owner_and_capture_bytes_outside_run_are_rejected()
    test_variable_owned_paths_are_used_in_finding_references()
    test_code_oracle_identity_and_renderer_summary_are_truthful()
    test_cli_failure_does_not_echo_private_paths()
    print("bench hunt tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
