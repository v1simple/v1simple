#!/usr/bin/env python3
"""Deterministic regression tests for hardware manifest scoring and extraction."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import score_hardware_run  # type: ignore  # noqa: E402


CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_metrics(path: Path, records: list[dict[str, object]]) -> None:
    with path.open("w", encoding="utf-8") as handle:
        for record in records:
            handle.write(json.dumps(record))
            handle.write("\n")


def write_manifest(
    path: Path,
    *,
    run_id: str,
    git_sha: str,
    git_ref: str,
    run_kind: str,
    board_id: str,
    env: str,
    lane: str,
    suite_or_profile: str,
    stress_class: str,
    result: str,
    metrics_file: str,
    scoring_file: str = "scoring.json",
    base_result: str | None = None,
    tracks: list[str] | None = None,
    unsupported_metrics: list[str] | None = None,
    source_type: str | None = None,
    source_schema: int | None = None,
    coverage_status: str | None = None,
    mode_coverage: dict[str, object] | None = None,
) -> None:
    payload = {
        "schema_version": 1,
        "run_id": run_id,
        "timestamp_utc": "2026-03-12T00:00:00Z",
        "git_sha": git_sha,
        "git_ref": git_ref,
        "run_kind": run_kind,
        "board_id": board_id,
        "env": env,
        "lane": lane,
        "suite_or_profile": suite_or_profile,
        "stress_class": stress_class,
        "result": result,
        "metrics_file": metrics_file,
        "scoring_file": scoring_file,
    }
    if base_result is not None:
        payload["base_result"] = base_result
    if tracks is not None:
        payload["tracks"] = tracks
    if unsupported_metrics is not None:
        payload["unsupported_metrics"] = unsupported_metrics
    if source_type is not None:
        payload["source_type"] = source_type
    if source_schema is not None:
        payload["source_schema"] = source_schema
    if coverage_status is not None:
        payload["coverage_status"] = coverage_status
    if mode_coverage is not None:
        payload["mode_coverage"] = mode_coverage
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def device_metric(suite: str, metric: str, value: float) -> dict[str, object]:
    return {
        "schema_version": 1,
        "run_id": "run-1",
        "git_sha": "abc1234",
        "run_kind": "device_suite",
        "suite_or_profile": suite,
        "metric": metric,
        "sample": "value",
        "value": value,
        "unit": "count",
        "tags": {},
    }


def soak_metric(track: str, metric: str, value: float) -> dict[str, object]:
    return {
        "schema_version": 1,
        "run_id": "soak-1",
        "git_sha": "abc1234",
        "run_kind": "real_fw_soak",
        "suite_or_profile": track,
        "metric": metric,
        "sample": "value",
        "value": value,
        "unit": "count",
        "tags": {},
    }


def standard_soak_records(
    track: str = "drive_wifi_ap",
    *,
    dma_largest_min_bytes: float = 14000,
    disp_pipe_max_peak_us: float = 30000,
    disp_pipe_p95_us: float = 21000,
    display_drive_activity_delta: float | None = None,
    display_updates_delta: float | None = None,
) -> list[dict[str, object]]:
    records = [
        soak_metric(track, "metrics_ok_samples", 10),
        soak_metric(track, "rx_packets_delta", 120),
        soak_metric(track, "parse_successes_delta", 120),
        soak_metric(track, "parse_failures_delta", 0),
        soak_metric(track, "queue_drops_delta", 0),
        soak_metric(track, "perf_drop_delta", 0),
        soak_metric(track, "event_drop_delta", 0),
        soak_metric(track, "oversize_drops_delta", 0),
        soak_metric(track, "loop_max_peak_us", 200000),
        soak_metric(track, "flush_max_peak_us", 30000),
        soak_metric(track, "wifi_max_peak_us", 3000),
        soak_metric(track, "ble_drain_max_peak_us", 5000),
        soak_metric(track, "sd_max_peak_us", 10000),
        soak_metric(track, "fs_max_peak_us", 10000),
        soak_metric(track, "queue_high_water_peak", 4),
        soak_metric(track, "wifi_connect_deferred_delta", 1),
        soak_metric(track, "dma_free_min_bytes", 25000),
        soak_metric(track, "dma_largest_min_bytes", dma_largest_min_bytes),
        soak_metric(track, "ble_process_max_peak_us", 40000),
        soak_metric(track, "disp_pipe_max_peak_us", disp_pipe_max_peak_us),
        soak_metric(track, "ble_mutex_timeout_delta", 0),
        soak_metric(track, "display_skips_delta", 0),
        soak_metric(track, "reconnects_delta", 0),
        soak_metric(track, "disconnects_delta", 0),
        soak_metric(track, "wifi_p95_us", 1800),
        soak_metric(track, "disp_pipe_p95_us", disp_pipe_p95_us),
        soak_metric(track, "dma_fragmentation_pct_p95", 18),
        # notify_to_display_max_ms is advisory on the drive_wifi_ap track.
        # Without it every consumer of standard_soak_records() trips
        # advisory_failures=1 and downgrades to PASS_WITH_WARNINGS. 25 ms
        # sits comfortably under the catalog's absolute_max=50 and produces
        # no regression when copied to baseline.
        soak_metric(track, "notify_to_display_max_ms", 25),
    ]
    if display_drive_activity_delta is not None:
        records.append(soak_metric(track, "display_drive_activity_delta", display_drive_activity_delta))
    if display_updates_delta is not None:
        records.append(soak_metric(track, "display_updates_delta", display_updates_delta))
    return records


def run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(ROOT / "tools" / "score_hardware_run.py"), *args],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def test_no_baseline_and_run_variance_commit_regression(tmpdir: Path) -> None:
    case_dir = tmpdir / "no_baseline"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    baseline_same_path = case_dir / "baseline_same.json"
    baseline_diff_path = case_dir / "baseline_diff.json"

    write_metrics(
        metrics_path,
        [
            device_metric("test_device_heap", "baseline_internal_free_bytes", 140000),
            device_metric("test_device_heap", "baseline_internal_largest_block_bytes", 60000),
            device_metric("test_device_heap", "internal_alloc_recovery_delta_bytes", 32),
            device_metric("test_device_heap", "spiram_alloc_recovery_delta_bytes", 64)
        ],
    )
    write_manifest(
        manifest_path,
        run_id="run-1",
        git_sha="abc1234",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )

    no_baseline = score_hardware_run.score_run(manifest_path, CATALOG_PATH)
    assert_true(no_baseline["result"] == "NO_BASELINE", "missing baseline should return NO_BASELINE")
    assert_true(no_baseline["comparison_kind"] == "no_baseline", "missing baseline should not classify as regression")

    write_manifest(
        baseline_same_path,
        run_id="run-0",
        git_sha="abc1234",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )
    same_sha = score_hardware_run.score_run(manifest_path, CATALOG_PATH, baseline_same_path)
    assert_true(same_sha["comparison_kind"] == "run_variance", "same git sha must classify as run_variance")

    write_manifest(
        baseline_diff_path,
        run_id="run-base",
        git_sha="def5678",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )
    diff_sha = score_hardware_run.score_run(manifest_path, CATALOG_PATH, baseline_diff_path)
    assert_true(diff_sha["comparison_kind"] == "commit_regression", "different git sha must classify as commit regression")


def test_explicit_failing_baseline_is_still_compared(tmpdir: Path) -> None:
    case_dir = tmpdir / "explicit_failing_baseline"
    case_dir.mkdir(parents=True, exist_ok=True)
    current_metrics = case_dir / "current_metrics.ndjson"
    baseline_metrics = case_dir / "baseline_metrics.ndjson"
    current_manifest = case_dir / "current.json"
    baseline_manifest = case_dir / "baseline.json"

    write_metrics(current_metrics, standard_soak_records())
    write_metrics(baseline_metrics, standard_soak_records())

    write_manifest(
        current_manifest,
        run_id="soak-current",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="display_preview",
        result="FAIL",
        metrics_file="current_metrics.ndjson",
        base_result="FAIL",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        baseline_manifest,
        run_id="soak-baseline",
        git_sha="def5678",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="display_preview",
        result="FAIL",
        metrics_file="baseline_metrics.ndjson",
        base_result="FAIL",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(current_manifest, CATALOG_PATH, baseline_manifest)
    assert_true(result["comparison_kind"] == "commit_regression", "explicit failing baselines should still be compared")
    assert_true(result["baseline_window"]["candidate_count"] == 1, "explicit failing baseline should remain in the candidate window")
    assert_true(result["baseline_manifest"] is not None, "explicit failing baseline should still be selected")
    # The point of keeping a failing baseline as a fallback anchor is that we
    # still get real metric deltas — not just a non-null manifest pointer.
    # Assert at least one metric was actually compared against the baseline.
    metrics_with_baseline = [m for m in result["metrics"] if m["baseline_value"] is not None]
    assert_true(
        len(metrics_with_baseline) > 0,
        f"failing baseline should still produce metric deltas, got none: {result['metrics']}",
    )
    deltas_computed = [m for m in metrics_with_baseline if m["delta_abs"] is not None]
    assert_true(
        len(deltas_computed) > 0,
        f"failing baseline should produce numeric delta_abs values, got none: {metrics_with_baseline}",
    )


def test_inconclusive_baseline_is_still_compared(tmpdir: Path) -> None:
    """INCONCLUSIVE baselines flow through the same fallback tier as FAIL.

    Pins the behavior so a future "be more lenient with INCONCLUSIVE" tweak
    doesn't silently revert this case to NO_BASELINE.
    """
    case_dir = tmpdir / "inconclusive_baseline"
    case_dir.mkdir(parents=True, exist_ok=True)
    current_metrics = case_dir / "current_metrics.ndjson"
    baseline_metrics = case_dir / "baseline_metrics.ndjson"
    current_manifest = case_dir / "current.json"
    baseline_manifest = case_dir / "baseline.json"

    write_metrics(current_metrics, standard_soak_records())
    write_metrics(baseline_metrics, standard_soak_records())

    write_manifest(
        current_manifest,
        run_id="soak-current",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="display_preview",
        result="INCONCLUSIVE",
        metrics_file="current_metrics.ndjson",
        base_result="INCONCLUSIVE",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        baseline_manifest,
        run_id="soak-baseline",
        git_sha="def5678",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="display_preview",
        result="INCONCLUSIVE",
        metrics_file="baseline_metrics.ndjson",
        base_result="INCONCLUSIVE",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(current_manifest, CATALOG_PATH, baseline_manifest)
    assert_true(
        result["comparison_kind"] == "commit_regression",
        f"inconclusive baselines should still be compared, got {result['comparison_kind']}",
    )
    assert_true(
        result["baseline_window"]["candidate_count"] == 1,
        f"inconclusive baseline should remain in the candidate window: {result['baseline_window']}",
    )
    assert_true(
        result["baseline_manifest"] is not None,
        "inconclusive baseline should still be selected as the fallback anchor",
    )
    metrics_with_baseline = [m for m in result["metrics"] if m["baseline_value"] is not None]
    assert_true(
        len(metrics_with_baseline) > 0,
        f"inconclusive baseline should still produce metric deltas, got none: {result['metrics']}",
    )


def test_passing_baseline_wins_over_failing(tmpdir: Path) -> None:
    """When PASS and FAIL baselines are both available, the PASS one is selected.

    Locks in the tier-preference rule at the post-collection filter (lines
    ~452-468 of score_hardware_run.py). A regression that re-introduced an
    inline pre-collection filter would still pass this test (the FAIL one
    would just be filtered out earlier instead of later) — but a regression
    that REMOVED the post-collection passing-tier preference and treated all
    candidates equally would fail it.
    """
    case_dir = tmpdir / "mixed_tier_baselines"
    case_dir.mkdir(parents=True, exist_ok=True)
    current_metrics = case_dir / "current.ndjson"
    current_manifest = case_dir / "current.json"
    pass_metrics = case_dir / "pass_baseline.ndjson"
    pass_manifest = case_dir / "pass_baseline.json"
    fail_metrics = case_dir / "fail_baseline.ndjson"
    fail_manifest = case_dir / "fail_baseline.json"

    write_metrics(current_metrics, standard_soak_records())
    write_metrics(pass_metrics, standard_soak_records())
    write_metrics(fail_metrics, standard_soak_records())

    write_manifest(
        current_manifest,
        run_id="soak-current",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="display_preview",
        result="PASS",
        metrics_file="current.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        pass_manifest,
        run_id="soak-pass-baseline",
        git_sha="def5678",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="display_preview",
        result="PASS",
        metrics_file="pass_baseline.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        fail_manifest,
        run_id="soak-fail-baseline",
        git_sha="ghi9012",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="display_preview",
        result="FAIL",
        metrics_file="fail_baseline.ndjson",
        base_result="FAIL",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(
        current_manifest, CATALOG_PATH, [pass_manifest, fail_manifest]
    )
    baseline_window = result["baseline_window"]
    assert_true(
        baseline_window["candidate_count"] == 1,
        f"only the passing baseline should be selected: {baseline_window}",
    )
    candidate_results = [c["result"] for c in baseline_window["candidates"]]
    assert_true(
        candidate_results == ["PASS"],
        f"selected candidate should be PASS, got {candidate_results}",
    )
    assert_true(
        result["baseline_manifest"]["run_id"] == "soak-pass-baseline",
        f"passing baseline should anchor the comparison, got {result['baseline_manifest']}",
    )


def test_selector_and_track_matching(tmpdir: Path) -> None:
    case_dir = tmpdir / "selector_matching"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    current_manifest = case_dir / "current.json"
    baseline_manifest = case_dir / "baseline.json"

    write_metrics(
        metrics_path,
        [
            soak_metric("drive_wifi_ap", "metrics_ok_samples", 10),
            soak_metric("drive_wifi_ap", "rx_packets_delta", 120),
            soak_metric("drive_wifi_ap", "parse_successes_delta", 120),
            soak_metric("drive_wifi_ap", "parse_failures_delta", 0),
            soak_metric("drive_wifi_ap", "queue_drops_delta", 0),
            soak_metric("drive_wifi_ap", "perf_drop_delta", 0),
            soak_metric("drive_wifi_ap", "event_drop_delta", 0),
            soak_metric("drive_wifi_ap", "oversize_drops_delta", 0),
            soak_metric("drive_wifi_ap", "loop_max_peak_us", 200000),
            soak_metric("drive_wifi_ap", "flush_max_peak_us", 30000),
            soak_metric("drive_wifi_ap", "wifi_max_peak_us", 3000),
            soak_metric("drive_wifi_ap", "ble_drain_max_peak_us", 5000),
            soak_metric("drive_wifi_ap", "sd_max_peak_us", 10000),
            soak_metric("drive_wifi_ap", "fs_max_peak_us", 10000),
            soak_metric("drive_wifi_ap", "queue_high_water_peak", 4),
            soak_metric("drive_wifi_ap", "wifi_connect_deferred_delta", 1),
            soak_metric("drive_wifi_ap", "dma_free_min_bytes", 25000),
            soak_metric("drive_wifi_ap", "dma_largest_min_bytes", 14000),
            soak_metric("drive_wifi_ap", "ble_process_max_peak_us", 40000),
            soak_metric("drive_wifi_ap", "disp_pipe_max_peak_us", 30000),
            soak_metric("drive_wifi_ap", "ble_mutex_timeout_delta", 0),
            soak_metric("drive_wifi_ap", "display_skips_delta", 0),
            soak_metric("drive_wifi_ap", "reconnects_delta", 0),
            soak_metric("drive_wifi_ap", "disconnects_delta", 0),
            soak_metric("drive_wifi_ap", "wifi_p95_us", 1800),
            soak_metric("drive_wifi_ap", "disp_pipe_p95_us", 21000),
            soak_metric("drive_wifi_ap", "dma_fragmentation_pct_p95", 18)
        ],
    )
    write_manifest(
        current_manifest,
        run_id="soak-current",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        baseline_manifest,
        run_id="soak-base",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="radio",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(current_manifest, CATALOG_PATH, baseline_manifest)
    assert_true(result["comparison_kind"] == "no_baseline", "board mismatch must skip baseline comparison")


def test_lab_no_external_activity_downgrades_packet_count_gates(tmpdir: Path) -> None:
    case_dir = tmpdir / "lab_no_external_activity"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"

    records = [
        record
        for record in standard_soak_records("drive_wifi_ap")
        if record["metric"] not in {"rx_packets_delta", "parse_successes_delta"}
    ]
    records.extend(
        [
            soak_metric("drive_wifi_ap", "rx_packets_delta", 0),
            soak_metric("drive_wifi_ap", "parse_successes_delta", 0),
        ]
    )
    write_metrics(metrics_path, records)
    write_manifest(
        manifest_path,
        run_id="lab-no-external",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="perf-csv-import",
        lane="hardware-test-sd-serial-proxy",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
        mode_coverage={
            "mode": "proxy",
            "policy": "lab",
            "ok": True,
            "reasons": [],
            "warnings": ["no_proxy_activity_observed"],
            "proxy_rows": 0,
            "obd_rows": 0,
        },
    )

    scored = score_hardware_run.score_run(manifest_path, CATALOG_PATH)
    assert_true(scored["summary"]["hard_failures"] == 0, f"lab no-activity packet counts must not hard-fail: {scored}")
    scored_by_metric = {metric["metric"]: metric for metric in scored["metrics"]}
    for metric_name in ("rx_packets_delta", "parse_successes_delta"):
        metric = scored_by_metric[metric_name]
        assert_true(metric["score_status"] == "info", f"{metric_name} should be informational: {metric}")
        assert_true(metric["classification"] == "coverage_observation", f"{metric_name} classification mismatch: {metric}")


def test_drive_wifi_ap_loop_peak_is_informational(tmpdir: Path) -> None:
    case_dir = tmpdir / "wifi_ap_loop_info"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    current_manifest = case_dir / "current.json"
    baseline_manifest = case_dir / "baseline.json"

    current_records = [
        soak_metric("drive_wifi_ap", "metrics_ok_samples", 10),
        soak_metric("drive_wifi_ap", "rx_packets_delta", 120),
        soak_metric("drive_wifi_ap", "parse_successes_delta", 120),
        soak_metric("drive_wifi_ap", "parse_failures_delta", 0),
        soak_metric("drive_wifi_ap", "queue_drops_delta", 0),
        soak_metric("drive_wifi_ap", "perf_drop_delta", 0),
        soak_metric("drive_wifi_ap", "event_drop_delta", 0),
        soak_metric("drive_wifi_ap", "oversize_drops_delta", 0),
        soak_metric("drive_wifi_ap", "loop_max_peak_us", 220000),
        soak_metric("drive_wifi_ap", "flush_max_peak_us", 30000),
        soak_metric("drive_wifi_ap", "wifi_max_peak_us", 3000),
        soak_metric("drive_wifi_ap", "ble_drain_max_peak_us", 5000),
        soak_metric("drive_wifi_ap", "sd_max_peak_us", 10000),
        soak_metric("drive_wifi_ap", "fs_max_peak_us", 10000),
        soak_metric("drive_wifi_ap", "queue_high_water_peak", 4),
        soak_metric("drive_wifi_ap", "wifi_connect_deferred_delta", 1),
        soak_metric("drive_wifi_ap", "dma_free_min_bytes", 25000),
        soak_metric("drive_wifi_ap", "dma_largest_min_bytes", 14000),
        soak_metric("drive_wifi_ap", "ble_process_max_peak_us", 40000),
        soak_metric("drive_wifi_ap", "disp_pipe_max_peak_us", 30000),
        soak_metric("drive_wifi_ap", "ble_mutex_timeout_delta", 0),
        soak_metric("drive_wifi_ap", "display_skips_delta", 0),
        soak_metric("drive_wifi_ap", "reconnects_delta", 0),
        soak_metric("drive_wifi_ap", "disconnects_delta", 0),
        soak_metric("drive_wifi_ap", "wifi_p95_us", 1800),
        soak_metric("drive_wifi_ap", "disp_pipe_p95_us", 21000),
        soak_metric("drive_wifi_ap", "dma_fragmentation_pct_p95", 18),
        # notify_to_display_max_ms is advisory on drive_wifi_ap. Without it
        # the score reports advisory_failures=1 (missing required-at-advisory
        # metric) and the run becomes PASS_WITH_WARNINGS, which violates the
        # test's "loop peak alone is informational" invariant. 25 ms is
        # comfortably under the catalog's absolute_max=50 and identical
        # between current/baseline, so it scores pass and the test focuses
        # on the loop peak regression as
        # designed.
        soak_metric("drive_wifi_ap", "notify_to_display_max_ms", 25),
    ]
    baseline_records = [dict(item) for item in current_records]
    for record in baseline_records:
        if record["metric"] == "loop_max_peak_us":
            record["value"] = 180000
            break

    write_metrics(metrics_path, current_records)
    baseline_metrics_path = case_dir / "baseline.ndjson"
    write_metrics(baseline_metrics_path, baseline_records)

    write_manifest(
        current_manifest,
        run_id="soak-current",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="real-fw-soak",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        baseline_manifest,
        run_id="soak-base",
        git_sha="base567",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="real-fw-soak",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="baseline.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(current_manifest, CATALOG_PATH, baseline_manifest)
    assert_true(result["result"] == "PASS", f"wifi-ap loop peak should not fail run: {result}")
    loop_metric = next(metric for metric in result["metrics"] if metric["metric"] == "loop_max_peak_us")
    assert_true(loop_metric["score_level"] == "info", f"wifi-ap loop peak should be info: {loop_metric}")
    assert_true(loop_metric["score_status"] == "info", f"wifi-ap loop regression should stay informational: {loop_metric}")


def test_dma_largest_uses_fixed_absolute_threshold(tmpdir: Path) -> None:
    case_dir = tmpdir / "dma_fixed_threshold"
    case_dir.mkdir(parents=True, exist_ok=True)
    current_metrics = case_dir / "current.ndjson"
    baseline_metrics = case_dir / "baseline.ndjson"
    current_manifest = case_dir / "current.json"
    baseline_manifest = case_dir / "baseline.json"

    write_metrics(current_metrics, standard_soak_records(dma_largest_min_bytes=22516))
    write_metrics(baseline_metrics, standard_soak_records(dma_largest_min_bytes=25588))

    write_manifest(
        current_manifest,
        run_id="dma-current",
        git_sha="abc1234",
        git_ref="dev",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="real-fw-soak",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="current.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        baseline_manifest,
        run_id="dma-baseline",
        git_sha="abc1234",
        git_ref="dev",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="real-fw-soak",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="baseline.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(current_manifest, CATALOG_PATH, baseline_manifest)
    dma_metric = next(metric for metric in result["metrics"] if metric["metric"] == "dma_largest_min_bytes")
    assert_true(result["result"] == "PASS", f"3,072-byte DMA drop should pass fixed threshold scoring: {result}")
    assert_true(dma_metric["baseline_value"] == 25588, f"unexpected DMA baseline value: {dma_metric}")
    assert_true(dma_metric["regression_state"] == "pass", f"DMA metric should stay within fixed threshold: {dma_metric}")
    assert_true(dma_metric["classification"] == "unchanged", f"DMA delta inside threshold should be unchanged: {dma_metric}")


def test_multi_baseline_median_ignores_failed_candidates(tmpdir: Path) -> None:
    case_dir = tmpdir / "median_baseline"
    case_dir.mkdir(parents=True, exist_ok=True)
    current_metrics = case_dir / "current.ndjson"
    current_manifest = case_dir / "current.json"
    baseline_specs = [
        ("baseline_latest", 25588, "PASS"),
        ("baseline_failed", 50000, "FAIL"),
        ("baseline_mid", 21492, "PASS"),
        ("baseline_oldest", 21492, "PASS_WITH_WARNINGS"),
    ]
    baseline_paths: list[Path] = []

    write_metrics(current_metrics, standard_soak_records(dma_largest_min_bytes=22516))
    write_manifest(
        current_manifest,
        run_id="median-current",
        git_sha="abc1234",
        git_ref="dev",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="real-fw-soak",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="current.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )

    for name, dma_value, result_name in baseline_specs:
        metrics_path = case_dir / f"{name}.ndjson"
        manifest_path = case_dir / f"{name}.json"
        write_metrics(metrics_path, standard_soak_records(dma_largest_min_bytes=dma_value))
        write_manifest(
            manifest_path,
            run_id=name,
            git_sha="abc1234",
            git_ref="dev",
            run_kind="real_fw_soak",
            board_id="release",
            env="waveshare-349",
            lane="real-fw-soak",
            suite_or_profile="drive_wifi_ap",
            stress_class="core",
            result=result_name,
            metrics_file=metrics_path.name,
            base_result=result_name,
            tracks=["drive_wifi_ap"],
        )
        baseline_paths.append(manifest_path)

    result = score_hardware_run.score_run(current_manifest, CATALOG_PATH, baseline_paths)
    dma_metric = next(metric for metric in result["metrics"] if metric["metric"] == "dma_largest_min_bytes")
    baseline_window = result["baseline_window"]

    assert_true(result["result"] == "PASS", f"median baseline should keep clean DMA run passing: {result}")
    assert_true(dma_metric["baseline_value"] == 21492, f"expected median DMA baseline of 21492: {dma_metric}")
    assert_true(baseline_window["strategy"] == "median_last_3_trustworthy", f"unexpected baseline strategy: {baseline_window}")
    assert_true(baseline_window["candidate_count"] == 3, f"failed baseline should be ignored: {baseline_window}")
    assert_true(
        [candidate["result"] for candidate in baseline_window["candidates"]] == ["PASS", "PASS", "PASS_WITH_WARNINGS"],
        f"unexpected selected baseline candidates: {baseline_window}",
    )
    assert_true(
        result["baseline_manifest"]["run_id"] == "baseline_latest",
        f"newest passing baseline should remain provenance anchor: {result}",
    )


def test_disp_pipe_peak_spike_is_diagnostic_only(tmpdir: Path) -> None:
    case_dir = tmpdir / "display_peak_diagnostic"
    case_dir.mkdir(parents=True, exist_ok=True)
    current_metrics = case_dir / "current.ndjson"
    baseline_metrics = case_dir / "baseline.ndjson"
    current_manifest = case_dir / "current.json"
    baseline_manifest = case_dir / "baseline.json"

    write_metrics(
        current_metrics,
        standard_soak_records(
            disp_pipe_max_peak_us=46817,
            disp_pipe_p95_us=27003.7,
            display_drive_activity_delta=40,
            display_updates_delta=40,
        ),
    )
    write_metrics(
        baseline_metrics,
        standard_soak_records(
            disp_pipe_max_peak_us=27320,
            disp_pipe_p95_us=26902.0,
            display_drive_activity_delta=38,
            display_updates_delta=38,
        ),
    )

    write_manifest(
        current_manifest,
        run_id="display-current",
        git_sha="abc1234",
        git_ref="dev",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="real-fw-soak",
        suite_or_profile="drive_wifi_ap",
        stress_class="display_preview",
        result="PASS",
        metrics_file="current.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        baseline_manifest,
        run_id="display-baseline",
        git_sha="abc1234",
        git_ref="dev",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="real-fw-soak",
        suite_or_profile="drive_wifi_ap",
        stress_class="display_preview",
        result="PASS",
        metrics_file="baseline.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(current_manifest, CATALOG_PATH, baseline_manifest)
    peak_metric = next(metric for metric in result["metrics"] if metric["metric"] == "disp_pipe_max_peak_us")
    p95_metric = next(metric for metric in result["metrics"] if metric["metric"] == "disp_pipe_p95_us")

    assert_true(result["result"] == "PASS", f"single-frame display spike should not fail trend scoring: {result}")
    assert_true(peak_metric["score_level"] == "info", f"display peak should be diagnostic-only: {peak_metric}")
    assert_true(peak_metric["required"] is False, f"display peak should not be required: {peak_metric}")
    assert_true(peak_metric["score_status"] != "fail", f"display peak should never hard-fail trend scoring: {peak_metric}")
    assert_true(p95_metric["score_level"] == "hard", f"display p95 should be the hard gate: {p95_metric}")
    assert_true(p95_metric["score_status"] == "pass", f"flat display p95 should keep the run clean: {p95_metric}")


def test_optional_metric_gap_warns_but_is_not_inconclusive(tmpdir: Path) -> None:
    case_dir = tmpdir / "inconclusive"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    catalog_path = case_dir / "catalog.json"

    write_metrics(
        metrics_path,
        [
            {
                "schema_version": 1,
                "run_id": "custom-run",
                "git_sha": "abc1234",
                "run_kind": "custom_kind",
                "suite_or_profile": "track-a",
                "metric": "required_metric",
                "sample": "value",
                "value": 10,
                "unit": "count",
                "tags": {},
            }
        ],
    )
    write_manifest(
        manifest_path,
        run_id="custom-run",
        git_sha="abc1234",
        git_ref="main",
        run_kind="custom_kind",
        board_id="release",
        env="custom",
        lane="custom",
        suite_or_profile="track-a",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["track-a"],
    )
    catalog_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "metrics": [
                    {
                        "metric": "required_metric",
                        "run_kind": "custom_kind",
                        "selector": {"suite_or_profile": "track-a"},
                        "unit": "count",
                        "aggregation": "last",
                        "direction": "higher_better",
                        "score_level": "hard",
                        "required": True,
                        "absolute_min": 1,
                        "absolute_max": None,
                        "regress_abs": None,
                        "regress_pct": None,
                    },
                    {
                        "metric": "optional_metric",
                        "run_kind": "custom_kind",
                        "selector": {"suite_or_profile": "track-a"},
                        "unit": "count",
                        "aggregation": "last",
                        "direction": "higher_better",
                        "score_level": "advisory",
                        "required": False,
                        "absolute_min": None,
                        "absolute_max": None,
                        "regress_abs": None,
                        "regress_pct": None,
                    },
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    result = score_hardware_run.score_run(manifest_path, catalog_path)
    assert_true(result["result"] == "PASS_WITH_WARNINGS", "missing optional advisory metrics should warn, not become inconclusive")


def test_regression_severity_can_be_advisory_under_hard_absolute_gate(tmpdir: Path) -> None:
    case_dir = tmpdir / "regression_severity"
    case_dir.mkdir(parents=True, exist_ok=True)
    catalog_path = case_dir / "catalog.json"
    baseline_metrics_path = case_dir / "baseline_metrics.ndjson"
    current_metrics_path = case_dir / "current_metrics.ndjson"
    absolute_fail_metrics_path = case_dir / "absolute_fail_metrics.ndjson"
    baseline_manifest_path = case_dir / "baseline_manifest.json"
    current_manifest_path = case_dir / "current_manifest.json"
    absolute_fail_manifest_path = case_dir / "absolute_fail_manifest.json"

    catalog_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "metrics": [
                    {
                        "metric": "storage_peak_us",
                        "run_kind": "real_fw_soak",
                        "selector": {"suite_or_profile": "track-a"},
                        "unit": "us",
                        "aggregation": "last",
                        "direction": "lower_better",
                        "score_level": "hard",
                        "regression_score_level": "advisory",
                        "required": True,
                        "absolute_min": None,
                        "absolute_max": 100,
                        "regress_abs": 10,
                        "regress_pct": None,
                    }
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    write_metrics(baseline_metrics_path, [soak_metric("track-a", "storage_peak_us", 20)])
    write_metrics(current_metrics_path, [soak_metric("track-a", "storage_peak_us", 40)])
    write_metrics(absolute_fail_metrics_path, [soak_metric("track-a", "storage_peak_us", 120)])

    common_manifest = {
        "run_kind": "real_fw_soak",
        "board_id": "release",
        "env": "custom",
        "lane": "custom",
        "suite_or_profile": "track-a",
        "stress_class": "core",
        "result": "PASS",
        "base_result": "PASS",
        "tracks": ["track-a"],
    }
    write_manifest(
        baseline_manifest_path,
        run_id="baseline",
        git_sha="base123",
        git_ref="main",
        metrics_file=baseline_metrics_path.name,
        **common_manifest,
    )
    write_manifest(
        current_manifest_path,
        run_id="current",
        git_sha="cur1234",
        git_ref="main",
        metrics_file=current_metrics_path.name,
        **common_manifest,
    )
    write_manifest(
        absolute_fail_manifest_path,
        run_id="absolute-fail",
        git_sha="cur1234",
        git_ref="main",
        metrics_file=absolute_fail_metrics_path.name,
        **common_manifest,
    )

    regression_result = score_hardware_run.score_run(current_manifest_path, catalog_path, baseline_manifest_path)
    metric = regression_result["metrics"][0]
    assert_true(regression_result["result"] == "PASS_WITH_WARNINGS", f"advisory regression should warn: {regression_result}")
    assert_true(regression_result["summary"]["hard_failures"] == 0, f"advisory regression should not be hard: {regression_result}")
    assert_true(regression_result["summary"]["advisory_failures"] == 1, f"advisory regression should count as advisory: {regression_result}")
    assert_true(metric["score_level"] == "hard", f"absolute gate should remain hard: {metric}")
    assert_true(metric["regression_score_level"] == "advisory", f"regression gate should be advisory: {metric}")
    assert_true(metric["score_status"] == "warn", f"advisory regression should warn: {metric}")

    absolute_result = score_hardware_run.score_run(absolute_fail_manifest_path, catalog_path, baseline_manifest_path)
    assert_true(absolute_result["result"] == "FAIL", f"absolute hard gate should still fail: {absolute_result}")
    assert_true(absolute_result["summary"]["hard_failures"] == 1, f"absolute over-budget should remain hard: {absolute_result}")


def test_unsupported_metrics_do_not_fail_run(tmpdir: Path) -> None:
    case_dir = tmpdir / "unsupported_metrics"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    catalog_path = case_dir / "catalog.json"

    write_metrics(
        metrics_path,
        [
            {
                "schema_version": 1,
                "run_id": "custom-run",
                "git_sha": "abc1234",
                "run_kind": "custom_kind",
                "suite_or_profile": "track-a",
                "metric": "required_metric",
                "sample": "value",
                "value": 10,
                "unit": "count",
                "tags": {},
            }
        ],
    )
    write_manifest(
        manifest_path,
        run_id="custom-run",
        git_sha="abc1234",
        git_ref="main",
        run_kind="custom_kind",
        board_id="release",
        env="custom",
        lane="custom",
        suite_or_profile="track-a",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["track-a"],
        unsupported_metrics=["required_unsupported"],
        source_type="perf_csv",
        source_schema=12,
        coverage_status="partial_legacy_import",
    )
    catalog_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "metrics": [
                    {
                        "metric": "required_metric",
                        "run_kind": "custom_kind",
                        "selector": {"suite_or_profile": "track-a"},
                        "unit": "count",
                        "aggregation": "last",
                        "direction": "higher_better",
                        "score_level": "hard",
                        "required": True,
                        "absolute_min": 1,
                        "absolute_max": None,
                        "regress_abs": None,
                        "regress_pct": None,
                    },
                    {
                        "metric": "required_unsupported",
                        "run_kind": "custom_kind",
                        "selector": {"suite_or_profile": "track-a"},
                        "unit": "count",
                        "aggregation": "last",
                        "direction": "lower_better",
                        "score_level": "hard",
                        "required": True,
                        "absolute_min": 0,
                        "absolute_max": 0,
                        "regress_abs": None,
                        "regress_pct": None,
                    },
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    result = score_hardware_run.score_run(manifest_path, catalog_path)
    assert_true(result["result"] == "NO_BASELINE", f"unsupported required metrics must not fail the run: {result}")
    unsupported = [metric for metric in result["metrics"] if metric["classification"] == "unsupported"]
    assert_true(len(unsupported) == 1, f"expected one unsupported metric: {result}")
    assert_true(result["summary"]["unsupported_metrics"] == 1, f"summary should count unsupported metrics: {result}")


def test_uncataloged_metric_rejected(tmpdir: Path) -> None:
    case_dir = tmpdir / "uncataloged"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    write_metrics(
        metrics_path,
        [device_metric("test_device_heap", "uncataloged_metric", 1)],
    )
    write_manifest(
        manifest_path,
        run_id="bad-run",
        git_sha="abc1234",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )
    result = run_cli(str(manifest_path), "--catalog", str(CATALOG_PATH))
    assert_true(result.returncode == 3, "uncataloged emitted metrics must fail scoring setup")


def test_connect_burst_metrics_are_cataloged(tmpdir: Path) -> None:
    case_dir = tmpdir / "connect_burst_cataloged"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"

    write_metrics(
        metrics_path,
        [
            soak_metric("drive_wifi_ap", "metrics_ok_samples", 10),
            soak_metric("drive_wifi_ap", "rx_packets_delta", 100),
            soak_metric("drive_wifi_ap", "parse_successes_delta", 100),
            soak_metric("drive_wifi_ap", "parse_failures_delta", 0),
            soak_metric("drive_wifi_ap", "queue_drops_delta", 0),
            soak_metric("drive_wifi_ap", "perf_drop_delta", 0),
            soak_metric("drive_wifi_ap", "event_drop_delta", 0),
            soak_metric("drive_wifi_ap", "oversize_drops_delta", 0),
            soak_metric("drive_wifi_ap", "flush_max_peak_us", 30000),
            soak_metric("drive_wifi_ap", "loop_max_peak_us", 70000),
            soak_metric("drive_wifi_ap", "wifi_max_peak_us", 2500),
            soak_metric("drive_wifi_ap", "ble_drain_max_peak_us", 5000),
            soak_metric("drive_wifi_ap", "sd_max_peak_us", 12000),
            soak_metric("drive_wifi_ap", "fs_max_peak_us", 0),
            soak_metric("drive_wifi_ap", "queue_high_water_peak", 4),
            soak_metric("drive_wifi_ap", "wifi_connect_deferred_delta", 0),
            soak_metric("drive_wifi_ap", "dma_free_min_bytes", 25000),
            soak_metric("drive_wifi_ap", "dma_largest_min_bytes", 14000),
            soak_metric("drive_wifi_ap", "ble_process_max_peak_us", 200),
            soak_metric("drive_wifi_ap", "disp_pipe_max_peak_us", 50000),
            soak_metric("drive_wifi_ap", "ble_mutex_timeout_delta", 0),
            soak_metric("drive_wifi_ap", "display_updates_delta", 200),
            soak_metric("drive_wifi_ap", "display_skips_delta", 0),
            soak_metric("drive_wifi_ap", "reconnects_delta", 0),
            soak_metric("drive_wifi_ap", "disconnects_delta", 0),
            soak_metric("drive_wifi_ap", "wifi_p95_us", 1800),
            soak_metric("drive_wifi_ap", "disp_pipe_p95_us", 35000),
            soak_metric("drive_wifi_ap", "dma_fragmentation_pct_p95", 18),
            soak_metric("drive_wifi_ap", "samples_to_stable", 2),
            soak_metric("drive_wifi_ap", "time_to_stable_ms", 500),
            soak_metric("drive_wifi_ap", "connect_burst_samples_to_stable", 3),
            soak_metric("drive_wifi_ap", "connect_burst_time_to_stable_ms", 1200),
            soak_metric("drive_wifi_ap", "connect_burst_pre_ble_process_peak_us", 150),
            soak_metric("drive_wifi_ap", "connect_burst_pre_disp_pipe_peak_us", 48000),
            soak_metric("drive_wifi_ap", "connect_burst_ble_followup_request_alert_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_ble_followup_request_version_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_ble_connect_stable_callback_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_ble_proxy_start_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_disp_render_peak_us", 47000),
            soak_metric("drive_wifi_ap", "connect_burst_display_gap_recover_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_display_frequency_peak_us", 14000),
            soak_metric("drive_wifi_ap", "connect_burst_display_flush_subphase_peak_us", 21000),
            soak_metric("drive_wifi_ap", "display_partial_flush_area_peak_px", 8192),
            soak_metric("drive_wifi_ap", "display_flush_max_area_px", 153664),
            soak_metric("drive_wifi_ap", "display_frequency_peak_us", 14000),
            soak_metric("drive_wifi_ap", "display_flush_subphase_peak_us", 21000),
            soak_metric("drive_wifi_ap", "display_restore_render_peak_us", 13000),
            soak_metric("drive_wifi_ap", "notify_to_display_max_ms", 25),
        ],
    )

    write_manifest(
        manifest_path,
        run_id="soak-connect-burst",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(manifest_path, CATALOG_PATH)
    assert_true(result["result"] == "NO_BASELINE", f"cataloged connect-burst metrics should score cleanly: {result}")
    metrics = {item["metric"] for item in result["metrics"]}
    assert_true("connect_burst_samples_to_stable" in metrics, f"missing connect-burst metric in score output: {result}")
    assert_true("connect_burst_disp_render_peak_us" in metrics, f"missing render peak metric in score output: {result}")
    assert_true("display_frequency_peak_us" in metrics, f"missing display frequency metric in score output: {result}")
    assert_true("display_partial_flush_area_peak_px" in metrics, f"missing display area metric in score output: {result}")


def test_extract_device_metrics_smoke(tmpdir: Path) -> None:
    case_dir = tmpdir / "extract_smoke"
    case_dir.mkdir(parents=True, exist_ok=True)
    log_path = case_dir / "suite.log"
    out_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    log_path.write_text(
        "\n".join(
            [
                "[platformio] noise",
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": "",
                        "git_sha": "",
                        "run_kind": "device_suite",
                        "suite_or_profile": "test_device_heap",
                        "metric": "baseline_internal_free_bytes",
                        "sample": "baseline",
                        "value": 140000,
                        "unit": "bytes",
                        "tags": {},
                    }
                ),
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": "",
                        "git_sha": "",
                        "run_kind": "device_suite",
                        "suite_or_profile": "test_device_heap",
                        "metric": "baseline_internal_largest_block_bytes",
                        "sample": "baseline",
                        "value": 60000,
                        "unit": "bytes",
                        "tags": {},
                    }
                ),
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": "",
                        "git_sha": "",
                        "run_kind": "device_suite",
                        "suite_or_profile": "test_device_heap",
                        "metric": "internal_alloc_recovery_delta_bytes",
                        "sample": "recovery",
                        "value": 16,
                        "unit": "bytes",
                        "tags": {},
                    }
                ),
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": "",
                        "git_sha": "",
                        "run_kind": "device_suite",
                        "suite_or_profile": "test_device_heap",
                        "metric": "spiram_alloc_recovery_delta_bytes",
                        "sample": "recovery",
                        "value": 32,
                        "unit": "bytes",
                        "tags": {},
                    }
                ),
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    extract = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "extract_device_metrics.py"),
            str(log_path),
            str(out_path),
            "--run-id",
            "suite-run",
            "--git-sha",
            "abc1234",
            "--suite",
            "test_device_heap",
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert_true(extract.returncode == 0, f"extract_device_metrics.py failed: {extract.stderr}")
    assert_true(out_path.read_text(encoding="utf-8").count("\n") == 4, "extractor must retain all structured metric lines")

    write_manifest(
        manifest_path,
        run_id="suite-run",
        git_sha="abc1234",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )
    scored = score_hardware_run.score_run(manifest_path, CATALOG_PATH)
    assert_true(scored["summary"]["metrics_scored"] >= 4, "extracted metrics should feed the scorer")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="hardware-run-scoring-") as tmp:
        tmpdir = Path(tmp)
        test_no_baseline_and_run_variance_commit_regression(tmpdir)
        test_explicit_failing_baseline_is_still_compared(tmpdir)
        test_inconclusive_baseline_is_still_compared(tmpdir)
        test_passing_baseline_wins_over_failing(tmpdir)
        test_selector_and_track_matching(tmpdir)
        test_lab_no_external_activity_downgrades_packet_count_gates(tmpdir)
        test_drive_wifi_ap_loop_peak_is_informational(tmpdir)
        test_dma_largest_uses_fixed_absolute_threshold(tmpdir)
        test_multi_baseline_median_ignores_failed_candidates(tmpdir)
        test_disp_pipe_peak_spike_is_diagnostic_only(tmpdir)
        test_optional_metric_gap_warns_but_is_not_inconclusive(tmpdir)
        test_regression_severity_can_be_advisory_under_hard_absolute_gate(tmpdir)
        test_unsupported_metrics_do_not_fail_run(tmpdir)
        test_uncataloged_metric_rejected(tmpdir)
        test_connect_burst_metrics_are_cataloged(tmpdir)
        test_extract_device_metrics_smoke(tmpdir)
    print("hardware run scoring tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
