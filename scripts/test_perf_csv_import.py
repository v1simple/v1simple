#!/usr/bin/env python3
"""Deterministic regression tests for perf CSV import into hardware scoring."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
FIXTURES_DIR = ROOT / "test" / "fixtures" / "perf"
sys.path.insert(0, str(TOOLS_DIR))

import import_perf_csv  # type: ignore  # noqa: E402

HEADER_COLUMNS = [
    line.strip()
    for line in (ROOT / "test" / "contracts" / "perf_csv_column_contract.txt").read_text(encoding="utf-8").splitlines()
    if line.strip() and not line.startswith("#")
]
CURRENT_HEADER_SCHEMA = 30
DIRECT_SPEED_COLUMNS = {
    "speedSourceSelected",
    "speedSourceValid",
    "speedSelectedMph_x10",
    "speedSelectedAgeMs",
    "speedSourceSwitches",
    "speedNoSourceSelections",
}
CONNECTION_CYCLE_COLUMNS = {
    "cycleState",
    "cycleTransitionsTotal",
    "cycleTimeInStateMs",
    "cycleTeardownDurationMs",
    "cycleObdRetryAttemptsTotal",
    "cycleWifiManualPhoneKicksTotal",
    "cycleProxyNoClientLatched",
}
SCHEMA13_REMOVED_COLUMNS = {
    "bleState",
    "subscribeStep",
    "connectInProgress",
    "asyncConnectPending",
    "pendingDisconnectCleanup",
    "proxyAdvertising",
    "proxyAdvertisingLastTransitionReason",
    "wifiPriorityMode",
}
LEGACY_REMOVED_COLUMNS = {
    "pendingDisconnectCleanup",
    "proxyAdvertising",
    "proxyAdvertisingLastTransitionReason",
    "wifiPriorityMode",
}
NO_DIRECT_SPEED_HEADER_COLUMNS = [
    column
    for column in HEADER_COLUMNS
    if column not in DIRECT_SPEED_COLUMNS and column not in CONNECTION_CYCLE_COLUMNS
]
SCHEMA13_HEADER_COLUMNS = [
    column for column in NO_DIRECT_SPEED_HEADER_COLUMNS if column not in SCHEMA13_REMOVED_COLUMNS
]
LEGACY_HEADER_COLUMNS = [
    column for column in NO_DIRECT_SPEED_HEADER_COLUMNS if column not in LEGACY_REMOVED_COLUMNS
]
# Optional direct-speed extension used for importer compatibility tests.
COMPAT_SPEED_HEADER_COLUMNS = NO_DIRECT_SPEED_HEADER_COLUMNS + ["obdSpeedMph_x10"]


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def apply_drive_speed(row: dict[str, int], speed_mph_x10: int) -> None:
    if "speedSourceSelected" in row:
        row["speedSourceSelected"] = 3
    if "speedSourceValid" in row:
        row["speedSourceValid"] = 1
    if "speedSelectedMph_x10" in row:
        row["speedSelectedMph_x10"] = speed_mph_x10
    if "speedSelectedAgeMs" in row:
        row["speedSelectedAgeMs"] = 200
    if "speedSourceSwitches" in row:
        row["speedSourceSwitches"] = 1
    if "speedNoSourceSelections" in row:
        row["speedNoSourceSelections"] = 0
    if "obdSpeedMph_x10" in row:
        row["obdSpeedMph_x10"] = speed_mph_x10


def base_row(millis: int, *, connected: bool, header_columns: list[str]) -> dict[str, int]:
    row = {column: 0 for column in header_columns}
    row.update(
        {
            "millis": millis,
            "timeValid": 1,
            "timeSource": 1,
            "qDrop": 0,
            "parseFail": 0,
            "parseResync": 0,
            "oversizeDrops": 0,
            "bleMutexTimeout": 0,
            "loopMax_us": 80000,
            "bleDrainMax_us": 4000,
            "bleProcessMax_us": 40000,
            "dispPipeMax_us": 30000,
            "flushMax_us": 20000,
            "sdMax_us": 10000,
            "fsMax_us": 10000,
            "queueHighWater": 5,
            "freeDma": 30000,
            "largestDma": 18000,
            "dmaLargestMin": 9000,
            "dmaFreeMin": 12000,
            "wifiConnectDeferred": 0,
            "wifiMax_us": 600,
            "displayUpdates": 0,
            "displaySkips": 0,
            "cmdPaceNotYet": 0,
            "reconn": 0,
            "disc": 0,
            "rx": 0,
            "parseOK": 0,
        }
    )
    if "freeDmaMin" in row:
        row["freeDmaMin"] = 26000
    if "largestDmaMin" in row:
        row["largestDmaMin"] = 15000
    if "perfDrop" in row:
        row["perfDrop"] = 0
    if "eventBusDrops" in row:
        row["eventBusDrops"] = 0
    if "speedSourceSelected" in row:
        row["speedSourceSelected"] = 0
    if "speedSourceValid" in row:
        row["speedSourceValid"] = 0
    if "speedSelectedMph_x10" in row:
        row["speedSelectedMph_x10"] = 0
    if "speedSelectedAgeMs" in row:
        row["speedSelectedAgeMs"] = 4294967295
    if "speedSourceSwitches" in row:
        row["speedSourceSwitches"] = 0
    if "speedNoSourceSelections" in row:
        row["speedNoSourceSelections"] = 0
    if connected:
        row["rx"] = 100
        row["parseOK"] = 100
        if "bleState" in row:
            row["bleState"] = 8
        if "subscribeStep" in row:
            row["subscribeStep"] = 11
    return row


def make_session(
    *,
    seq: int,
    token: str,
    schema: int,
    header_columns: list[str],
    duration_ms: int,
    connected: bool,
    drive_like: bool = False,
    row_count: int = 5,
    end_overrides: dict[str, int] | None = None,
) -> dict[str, object]:
    rows = []
    for index in range(row_count):
        frac = index / max(row_count - 1, 1)
        row = base_row(int(frac * duration_ms), connected=connected, header_columns=header_columns)
        if connected:
            row["rx"] = 100 + 50 * index
            row["parseOK"] = 100 + 50 * index
            row["displayUpdates"] = 8 * index
        if drive_like:
            apply_drive_speed(row, 350 + index * 10)
        rows.append(row)
    if end_overrides:
        rows[-1].update(end_overrides)
    return {
        "meta": f"#session_start,seq={seq},bootId={seq},uptime_ms={duration_ms},token={token},schema={schema}",
        "rows": rows,
    }


def write_capture(
    path: Path,
    *,
    header_columns: list[str],
    sessions: list[dict[str, object]],
    leading_rows: list[dict[str, int]] | None = None,
) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=header_columns)
        if leading_rows:
            for row in leading_rows:
                writer.writerow({key: row.get(key, 0) for key in header_columns})
        for session in sessions:
            writer.writeheader()
            handle.write(f"{session['meta']}\n")
            for row in session["rows"]:
                writer.writerow({key: row.get(key, 0) for key in header_columns})


def run_import(
    csv_path: Path,
    out_dir: Path,
    *extra_args: str,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "import_perf_csv.py"),
            "--input",
            str(csv_path),
            "--out-dir",
            str(out_dir),
            "--profile",
            "drive_wifi_ap",
            "--board-id",
            "test",
            "--git-sha",
            "abc1234",
            "--git-ref",
            "test-branch",
            *extra_args,
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def test_dma_fragmentation_uses_current_values() -> None:
    rows: list[dict[str, int]] = []
    for index, free_dma in enumerate([20000, 18000, 16000, 14000, 12000]):
        row = base_row(index * 1000, connected=True, header_columns=LEGACY_HEADER_COLUMNS)
        row["freeDma"] = free_dma
        row["largestDma"] = 10000
        row["dmaFreeMin"] = 50000
        row["dmaLargestMin"] = 49000
        rows.append(row)
    metrics, _peaks, unsupported = import_perf_csv.extract_metrics(rows, 12)
    expected = import_perf_csv.percentile(
        [(1.0 - (10000.0 / free_dma)) * 100.0 for free_dma in [20000, 18000, 16000, 14000, 12000]],
        95,
    )
    assert_true(abs(metrics["dma_fragmentation_pct_p95"][0] - float(expected)) < 0.01, "fragmentation must use current freeDma/largestDma values")
    assert_true("perf_drop_delta" in unsupported and "event_drop_delta" in unsupported, "legacy schema should mark drop deltas unsupported")


def test_sd_start_runtime_split_uses_fixed_window(tmpdir: Path) -> None:
    csv_path = tmpdir / "sd_split.csv"
    out_dir = tmpdir / "sd_split_out"
    rows: list[dict[str, int]] = []
    for index, (millis, sd_max) in enumerate(
        [
            (1000, 65000),
            (6000, 47000),
            (11000, 32000),
            (16000, 28000),
        ]
    ):
        row = base_row(millis, connected=True, header_columns=HEADER_COLUMNS)
        row["rx"] = 100 + index * 50
        row["parseOK"] = 100 + index * 50
        row["sdMax_us"] = sd_max
        rows.append(row)

    metrics, _peaks, _unsupported = import_perf_csv.extract_metrics(rows, CURRENT_HEADER_SCHEMA)
    assert_true(metrics["sd_max_peak_us"][0] == 65000.0, f"raw SD peak wrong: {metrics}")
    assert_true(metrics["sd_start_max_peak_us"][0] == 65000.0, f"start SD peak wrong: {metrics}")
    assert_true(metrics["sd_runtime_max_peak_us"][0] == 32000.0, f"runtime SD peak wrong: {metrics}")

    split = import_perf_csv.build_sd_latency_split(rows)
    assert_true(split["start_window_ms"] == 10000, f"wrong start window: {split}")
    assert_true(split["start_row_count"] == 2, f"wrong start row count: {split}")
    assert_true(split["runtime_row_count"] == 2, f"wrong runtime row count: {split}")
    assert_true(split["runtime_peak"]["relative_ms"] == 10000, f"runtime split should be segment-relative: {split}")

    write_capture(
        csv_path,
        header_columns=HEADER_COLUMNS,
        sessions=[
            {
                "meta": "#session_start,seq=1,bootId=1,uptime_ms=1000,token=SDSPLIT1,schema=30",
                "rows": rows,
            }
        ],
    )
    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"SD split import errored: rc={result.returncode} stderr={result.stderr}")
    emitted_metrics = {
        json.loads(line)["metric"]: json.loads(line)["value"]
        for line in (out_dir / "metrics.ndjson").read_text(encoding="utf-8").splitlines()
        if line.strip()
    }
    assert_true(emitted_metrics["sd_start_max_peak_us"] == 65000.0, f"emitted start metric wrong: {emitted_metrics}")
    assert_true(emitted_metrics["sd_runtime_max_peak_us"] == 32000.0, f"emitted runtime metric wrong: {emitted_metrics}")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    assert_true(diagnostics["sd_latency_split"]["start_row_count"] == 2, f"diagnostics missing SD split: {diagnostics}")
    comparison = (out_dir / "comparison.txt").read_text(encoding="utf-8")
    assert_true("## SD Start vs Runtime" in comparison, "comparison should show SD start/runtime split")


def test_display_peak_metrics_are_imported_from_csv_windows() -> None:
    rows: list[dict[str, int]] = []
    for index in range(4):
        row = base_row(index * 1000, connected=True, header_columns=HEADER_COLUMNS)
        row["displayPreviewRenderCount"] = index
        row["displayPreviewScenarioRenderCount"] = index
        row["displayPreviewRenderMax_us"] = [0, 81000, 0, 0][index]
        row["displayPreviewFirstRenderMax_us"] = [0, 81000, 0, 0][index]
        row["displayPreviewSteadyRenderMax_us"] = [0, 0, 59000, 0][index]
        row["displayFlushSubphaseMax_us"] = [0, 34000, 21000, 0][index]
        row["displayRestingRenderMax_us"] = [0, 0, 0, 37000][index]
        rows.append(row)

    metrics, _peaks, _unsupported = import_perf_csv.extract_metrics(rows, CURRENT_HEADER_SCHEMA)
    assert_true(
        metrics["display_preview_render_peak_us"][0] == 81000.0,
        f"preview render peak must use the max observed window, not the final row: {metrics}",
    )
    assert_true(
        metrics["display_preview_first_render_peak_us"][0] == 81000.0,
        f"preview first-frame peak missing/wrong: {metrics}",
    )
    assert_true(
        metrics["display_preview_steady_render_peak_us"][0] == 59000.0,
        f"preview steady-frame peak missing/wrong: {metrics}",
    )
    assert_true(
        metrics["display_flush_subphase_peak_us"][0] == 34000.0,
        f"display flush subphase peak missing/wrong: {metrics}",
    )
    assert_true(
        metrics["display_resting_render_peak_us"][0] == 37000.0,
        f"resting render peak missing/wrong: {metrics}",
    )


def test_connect_burst_and_flush_metrics_are_imported_from_csv_windows() -> None:
    rows: list[dict[str, int]] = []
    for index in range(5):
        row = base_row(index * 1000, connected=True, header_columns=HEADER_COLUMNS)
        row["bleState"] = 8
        row["subscribeStep"] = 11
        row["bleProcessMax_us"] = [100, 48100, 200, 900, 80][index]
        row["dispPipeMax_us"] = [50, 32000, 37600, 100, 90][index]
        row["bleFollowupRequestAlertMax_us"] = [0, 260, 0, 0, 0][index]
        row["bleFollowupRequestVersionMax_us"] = [0, 0, 120, 0, 0][index]
        row["bleConnectStableCallbackMax_us"] = [0, 640, 0, 0, 0][index]
        row["bleProxyStartMax_us"] = [0, 660, 0, 0, 0][index]
        row["dispMax_us"] = [0, 18000, 27000, 0, 0][index]
        row["displayGapRecoverMax_us"] = [0, 0, 1200, 0, 0][index]
        row["displayBaseFrameMax_us"] = [0, 0, 0, 0, 0][index]
        row["displayStatusStripMax_us"] = [0, 10, 4150, 0, 0][index]
        row["displayFrequencyMax_us"] = [0, 30, 0, 0, 0][index]
        row["displayBandsBarsMax_us"] = [0, 0, 7, 0, 0][index]
        row["displayArrowsIconsMax_us"] = [0, 29, 0, 0, 0][index]
        row["displayFlushSubphaseMax_us"] = [0, 31000, 33300, 0, 0][index]
        row["displayFullFlushCount"] = index
        row["displayPartialFlushCount"] = index * 2
        row["displayPartialFlushAreaPeakPx"] = [0, 4096, 29890, 0, 0][index]
        row["displayFlushMaxAreaPx"] = [0, 153664, 0, 0, 0][index]
        rows.append(row)

    metrics, _peaks, _unsupported = import_perf_csv.extract_metrics(rows, CURRENT_HEADER_SCHEMA)

    assert_true(metrics["connect_burst_samples_to_stable"][0] == 3.0, f"wrong burst sample count: {metrics}")
    assert_true(metrics["connect_burst_time_to_stable_ms"][0] == 2000.0, f"wrong burst settle time: {metrics}")
    assert_true(metrics["connect_burst_pre_ble_process_peak_us"][0] == 48100.0, f"wrong burst ble peak: {metrics}")
    assert_true(metrics["connect_burst_pre_disp_pipe_peak_us"][0] == 37600.0, f"wrong burst display pipe peak: {metrics}")
    assert_true(metrics["connect_burst_ble_proxy_start_peak_us"][0] == 660.0, f"wrong proxy start peak: {metrics}")
    assert_true(metrics["connect_burst_disp_render_peak_us"][0] == 27000.0, f"wrong render peak: {metrics}")
    assert_true(metrics["connect_burst_display_flush_subphase_peak_us"][0] == 33300.0, f"wrong flush subphase peak: {metrics}")
    assert_true(metrics["display_full_flush_count_delta"][0] == 4.0, f"wrong full flush delta: {metrics}")
    assert_true(metrics["display_partial_flush_count_delta"][0] == 8.0, f"wrong partial flush delta: {metrics}")
    assert_true(metrics["display_partial_flush_area_peak_px"][0] == 29890.0, f"wrong partial flush area peak: {metrics}")
    assert_true(metrics["display_flush_max_area_px"][0] == 153664.0, f"wrong flush max area: {metrics}")


def test_legacy_import_reports_partial_coverage(tmpdir: Path) -> None:
    csv_path = tmpdir / "legacy.csv"
    out_dir = tmpdir / "legacy_out"
    write_capture(
        csv_path,
        header_columns=LEGACY_HEADER_COLUMNS,
        sessions=[
            make_session(
                seq=1,
                token="LEGACY01",
                schema=12,
                header_columns=LEGACY_HEADER_COLUMNS,
                duration_ms=60000,
                connected=True,
                drive_like=True,
            )
        ],
    )
    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"legacy import error: {result.stderr}")
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    scoring = json.loads((out_dir / "scoring.json").read_text(encoding="utf-8"))
    assert_true(manifest["source_type"] == "perf_csv", "legacy import should set source_type")
    assert_true(manifest["source_schema"] == 12, f"wrong source schema: {manifest}")
    assert_true(manifest["coverage_status"] == "partial_legacy_import", f"wrong coverage status: {manifest}")
    assert_true(set(["perf_drop_delta", "event_drop_delta", "samples_to_stable", "time_to_stable_ms"]).issubset(set(manifest["unsupported_metrics"])), f"unsupported metrics missing: {manifest}")
    unsupported_metrics = {metric["metric"] for metric in scoring["metrics"] if metric["classification"] == "unsupported"}
    assert_true("perf_drop_delta" in unsupported_metrics, "legacy scoring should mark perf_drop_delta unsupported")
    assert_true("event_drop_delta" in unsupported_metrics, "legacy scoring should mark event_drop_delta unsupported")
    assert_true(scoring["summary"]["hard_failures"] == 0, f"legacy unsupported fields must not hard-fail: {scoring}")
    comparison = (out_dir / "comparison.txt").read_text(encoding="utf-8")
    assert_true("coverage_status: partial_legacy_import" in comparison, "comparison should show coverage status")
    assert_true("UNSUPPORTED" in comparison, "comparison should show unsupported metrics")


def test_schema13_import_supports_drop_metrics(tmpdir: Path) -> None:
    csv_path = tmpdir / "schema13.csv"
    out_dir = tmpdir / "schema13_out"
    write_capture(
        csv_path,
        header_columns=SCHEMA13_HEADER_COLUMNS,
        sessions=[
            make_session(
                seq=1,
                token="SCHEMA013",
                schema=13,
                header_columns=SCHEMA13_HEADER_COLUMNS,
                duration_ms=60000,
                connected=True,
                drive_like=True,
                end_overrides={"perfDrop": 0, "eventBusDrops": 0},
            )
        ],
    )
    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"schema13 import error: {result.stderr}")
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    scoring = json.loads((out_dir / "scoring.json").read_text(encoding="utf-8"))
    metric_names = {metric["metric"] for metric in scoring["metrics"] if metric["classification"] != "unsupported"}
    assert_true(manifest["coverage_status"] == "full_runtime_gates", f"wrong coverage status: {manifest}")
    assert_true("perf_drop_delta" in metric_names, "schema13 import should emit perf_drop_delta")
    assert_true("event_drop_delta" in metric_names, "schema13 import should emit event_drop_delta")
    assert_true("perf_drop_delta" not in set(manifest["unsupported_metrics"]), "schema13 perf_drop_delta should not be unsupported")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    assert_true("loop_max_peak_us" in diagnostics["peaks"], "peak diagnostics missing loop_max_peak_us")
    assert_true(diagnostics["coverage_status"] == "full_runtime_gates", f"diagnostics coverage mismatch: {diagnostics}")


def test_segment_selection_and_listing_prefers_direct_speed_evidence_when_available(tmpdir: Path) -> None:
    csv_path = tmpdir / "multi.csv"
    out_dir = tmpdir / "multi_out"
    session_1 = make_session(
        seq=1,
        token="NODRIVE1",
        schema=CURRENT_HEADER_SCHEMA,
        header_columns=HEADER_COLUMNS,
        duration_ms=120000,
        connected=True,
        drive_like=False,
    )
    session_2 = make_session(
        seq=2,
        token="DRIVE002",
        schema=CURRENT_HEADER_SCHEMA,
        header_columns=HEADER_COLUMNS,
        duration_ms=60000,
        connected=True,
        drive_like=True,
    )
    write_capture(csv_path, header_columns=HEADER_COLUMNS, sessions=[session_1, session_2])

    listed = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "import_perf_csv.py"),
            "--input",
            str(csv_path),
            "--list-segments",
            "--segment",
            "auto",
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert_true(listed.returncode == 0, f"list-segments failed: {listed.stderr}")
    assert_true("DRIVE002" in listed.stdout, f"list-segments missing drive session: {listed.stdout}")

    auto_result = run_import(csv_path, out_dir)
    assert_true(auto_result.returncode != 3, f"auto import failed: {auto_result.stderr}")
    auto_manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    assert_true(auto_manifest["selected_segment"]["session_index"] == 2, f"auto selector chose wrong segment: {auto_manifest}")
    assert_true(auto_manifest["selected_segment"]["speed_active_rows_supported"] is True, f"speed support should be detected: {auto_manifest}")
    assert_true(auto_manifest["selected_segment"]["speed_active_column"] == "speedSelectedMph_x10", f"wrong speed evidence column: {auto_manifest}")

    explicit_out = tmpdir / "explicit_out"
    explicit_result = run_import(csv_path, explicit_out, "--segment", "1")
    assert_true(explicit_result.returncode != 3, f"explicit import failed: {explicit_result.stderr}")
    explicit_manifest = json.loads((explicit_out / "manifest.json").read_text(encoding="utf-8"))
    assert_true(explicit_manifest["selected_segment"]["session_index"] == 1, f"explicit selector chose wrong segment: {explicit_manifest}")


def test_segment_selection_without_direct_speed_column_falls_back_to_longest_connected(tmpdir: Path) -> None:
    csv_path = tmpdir / "multi_contract.csv"
    out_dir = tmpdir / "multi_contract_out"
    session_1 = make_session(
        seq=1,
        token="LONG001",
        schema=24,
        header_columns=NO_DIRECT_SPEED_HEADER_COLUMNS,
        duration_ms=120000,
        connected=True,
        drive_like=False,
    )
    session_2 = make_session(
        seq=2,
        token="SHORT002",
        schema=24,
        header_columns=NO_DIRECT_SPEED_HEADER_COLUMNS,
        duration_ms=60000,
        connected=True,
        drive_like=False,
    )
    write_capture(csv_path, header_columns=NO_DIRECT_SPEED_HEADER_COLUMNS, sessions=[session_1, session_2])

    listed = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "import_perf_csv.py"),
            "--input",
            str(csv_path),
            "--list-segments",
            "--segment",
            "auto",
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert_true(listed.returncode == 0, f"list-segments failed: {listed.stderr}")
    assert_true("n/a" in listed.stdout, f"list-segments should mark speed evidence unsupported: {listed.stdout}")

    auto_result = run_import(csv_path, out_dir)
    assert_true(auto_result.returncode != 3, f"auto import failed: {auto_result.stderr}")
    auto_manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    assert_true(auto_manifest["selected_segment"]["session_index"] == 1, f"auto selector should fall back to longest connected: {auto_manifest}")
    assert_true(auto_manifest["selected_segment"]["speed_active_rows_supported"] is False, f"speed evidence should be unsupported for contract header: {auto_manifest}")
    comparison = (out_dir / "comparison.txt").read_text(encoding="utf-8")
    assert_true("speed_rows=n/a (schema lacks direct speed column)" in comparison, f"comparison should explain missing direct speed evidence: {comparison}")


def test_segment_selection_compat_optional_speed_column_still_supported(tmpdir: Path) -> None:
    csv_path = tmpdir / "multi_compat.csv"
    out_dir = tmpdir / "multi_compat_out"
    session_1 = make_session(
        seq=1,
        token="NODRIVE1",
        schema=24,
        header_columns=COMPAT_SPEED_HEADER_COLUMNS,
        duration_ms=120000,
        connected=True,
        drive_like=False,
    )
    session_2 = make_session(
        seq=2,
        token="COMPAT02",
        schema=24,
        header_columns=COMPAT_SPEED_HEADER_COLUMNS,
        duration_ms=60000,
        connected=True,
        drive_like=True,
    )
    write_capture(csv_path, header_columns=COMPAT_SPEED_HEADER_COLUMNS, sessions=[session_1, session_2])

    listed = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "import_perf_csv.py"),
            "--input",
            str(csv_path),
            "--list-segments",
            "--segment",
            "auto",
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert_true(listed.returncode == 0, f"list-segments failed: {listed.stderr}")
    assert_true("COMPAT02" in listed.stdout, f"compat list-segments missing drive session: {listed.stdout}")

    auto_result = run_import(csv_path, out_dir)
    assert_true(auto_result.returncode != 3, f"auto import failed: {auto_result.stderr}")
    auto_manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    assert_true(auto_manifest["selected_segment"]["session_index"] == 2, f"compat auto selector chose wrong segment: {auto_manifest}")
    assert_true(auto_manifest["selected_segment"]["speed_active_rows_supported"] is True, f"compat speed evidence should be supported: {auto_manifest}")
    assert_true(auto_manifest["selected_segment"]["speed_active_column"] == "obdSpeedMph_x10", f"compat selector should use optional speed column: {auto_manifest}")


def test_peak_diagnostics_classify_spike_and_attribute_phase(tmpdir: Path) -> None:
    csv_path = tmpdir / "spike.csv"
    out_dir = tmpdir / "spike_out"
    session = make_session(
        seq=1,
        token="SPIKE001",
        schema=14,
        header_columns=HEADER_COLUMNS,
        duration_ms=120000,
        connected=True,
        drive_like=True,
        row_count=40,
    )
    rows = session["rows"]
    assert isinstance(rows, list)
    spike_row = rows[0]
    assert isinstance(spike_row, dict)
    spike_row["loopMax_us"] = 430000
    spike_row["bleProcessMax_us"] = 400000
    spike_row["bleState"] = 6
    spike_row["subscribeStep"] = 4
    spike_row["connectInProgress"] = 1
    spike_row["asyncConnectPending"] = 0
    spike_row["pendingDisconnectCleanup"] = 0
    spike_row["proxyAdvertising"] = 0
    spike_row["wifiPriorityMode"] = 0
    write_capture(csv_path, header_columns=HEADER_COLUMNS, sessions=[session])

    result = run_import(csv_path, out_dir)
    assert_true(result.returncode == 2, f"expected failing spike import, got rc={result.returncode} stderr={result.stderr}")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    loop_diag = diagnostics["peaks"]["loop_max_peak_us"]
    ble_diag = diagnostics["peaks"]["ble_process_max_peak_us"]
    partition = diagnostics["latency_partitions"]["metrics"]
    assert_true(loop_diag["classification"] == "spike", f"loop classification wrong: {loop_diag}")
    assert_true(loop_diag["exceed_count"] == 1, f"loop exceed_count wrong: {loop_diag}")
    assert_true(loop_diag["longest_exceed_run"] == 1, f"loop exceed run wrong: {loop_diag}")
    assert_true(loop_diag["segment_position"] == "start", f"loop segment position wrong: {loop_diag}")
    assert_true(loop_diag["likely_phase_bucket"] == "boundary spike during connect/discovery/subscribe", f"loop phase bucket wrong: {loop_diag}")
    assert_true(loop_diag["wrapper_symptom_of"] == "ble_process_max_peak_us", f"wrapper attribution missing: {loop_diag}")
    assert_true(loop_diag["top_5_rows"][0]["bleState"] == "SUBSCRIBING", f"bleState missing from top row: {loop_diag}")
    assert_true(loop_diag["top_5_rows"][0]["subscribeStep"] == "SUBSCRIBE_DISPLAY", f"subscribeStep missing from top row: {loop_diag}")
    assert_true(ble_diag["classification"] == "spike", f"ble classification wrong: {ble_diag}")
    assert_true(partition["loop_max_peak_us"]["diagnosis"] == "boundary_only", f"loop partition diagnosis wrong: {partition}")
    assert_true(partition["ble_process_max_peak_us"]["diagnosis"] == "boundary_only", f"ble partition diagnosis wrong: {partition}")
    assert_true(partition["loop_max_peak_us"]["steady_state_peak"]["classification"] == "clean", f"steady-state split should be clean: {partition}")
    comparison = (out_dir / "comparison.txt").read_text(encoding="utf-8")
    assert_true("classification=spike" in comparison, "comparison should show spike classification")
    assert_true("likely_phase_bucket=boundary spike during connect/discovery/subscribe" in comparison, "comparison should show phase bucket")
    assert_true("top_row:" in comparison, "comparison should show top rows")
    assert_true("## Boundary vs Steady-State" in comparison, "comparison should show latency split section")
    assert_true("diagnosis=boundary_only" in comparison, "comparison should show boundary-only diagnosis")


def test_peak_diagnostics_classify_sustained_runs(tmpdir: Path) -> None:
    csv_path = tmpdir / "sustained.csv"
    out_dir = tmpdir / "sustained_out"
    session = make_session(
        seq=1,
        token="SUSTAIN1",
        schema=14,
        header_columns=HEADER_COLUMNS,
        duration_ms=120000,
        connected=True,
        drive_like=True,
        row_count=20,
    )
    rows = session["rows"]
    assert isinstance(rows, list)
    for idx in range(4, 9):
        row = rows[idx]
        assert isinstance(row, dict)
        row["bleProcessMax_us"] = 390000
        row["loopMax_us"] = 420000
        row["bleState"] = 8
        row["subscribeStep"] = 11
    write_capture(csv_path, header_columns=HEADER_COLUMNS, sessions=[session])

    result = run_import(csv_path, out_dir)
    assert_true(result.returncode == 2, f"expected failing sustained import, got rc={result.returncode} stderr={result.stderr}")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    loop_diag = diagnostics["peaks"]["loop_max_peak_us"]
    partition = diagnostics["latency_partitions"]["metrics"]
    assert_true(loop_diag["classification"] == "sustained", f"loop classification wrong: {loop_diag}")
    assert_true(loop_diag["exceed_count"] == 5, f"loop exceed_count wrong: {loop_diag}")
    assert_true(loop_diag["longest_exceed_run"] == 5, f"loop exceed run wrong: {loop_diag}")
    assert_true(loop_diag["likely_phase_bucket"] == "sustained steady-state BLE runtime issue", f"loop phase bucket wrong: {loop_diag}")
    assert_true(len(loop_diag["top_5_rows"]) >= 5, f"top_5_rows missing sustained rows: {loop_diag}")
    assert_true(partition["loop_max_peak_us"]["diagnosis"] == "steady_state", f"loop partition diagnosis wrong: {partition}")
    assert_true(partition["loop_max_peak_us"]["steady_state_peak"]["classification"] == "sustained", f"steady-state partition should be sustained: {partition}")


def test_reduced_perf_boot_fixture_attributes_obd_and_wifi_stalls(tmpdir: Path) -> None:
    csv_path = FIXTURES_DIR / "perf_boot_6_reduced.csv"
    out_dir = tmpdir / "perf_boot_6_reduced_out"
    result = run_import(csv_path, out_dir)
    assert_true(result.returncode == 2, f"expected failing reduced fixture import, got rc={result.returncode} stderr={result.stderr}")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    loop_diag = diagnostics["peaks"]["loop_max_peak_us"]
    wifi_diag = diagnostics["peaks"]["wifi_max_peak_us"]

    assert_true(loop_diag["millis"] == 200097, f"loop peak millis wrong: {loop_diag}")
    assert_true(loop_diag["wrapper_symptom_of"] == "obdMax_us", f"loop peak should attribute to OBD: {loop_diag}")
    assert_true(loop_diag["likely_phase_bucket"] == "steady-state OBD runtime stall", f"loop phase bucket wrong: {loop_diag}")
    assert_true(loop_diag["top_5_rows"][0]["obdMax_us"] == 4147506, f"loop row missing obd max: {loop_diag}")
    assert_true(loop_diag["root_cause_hint"] == "inline OBD runtime stall", f"loop root cause hint wrong: {loop_diag}")

    assert_true(wifi_diag["millis"] == 235205, f"wifi peak millis wrong: {wifi_diag}")
    assert_true(wifi_diag["wrapper_symptom_of"] == "fsMax_us", f"wifi peak should attribute to fs serve: {wifi_diag}")
    assert_true(wifi_diag["likely_phase_bucket"] == "steady-state WiFi/AP file serving stall", f"wifi phase bucket wrong: {wifi_diag}")
    assert_true(wifi_diag["top_5_rows"][0]["fsMax_us"] == 266110, f"wifi row missing fs max: {wifi_diag}")
    assert_true(wifi_diag["root_cause_hint"] == "LittleFS static file serving during AP", f"wifi root cause hint wrong: {wifi_diag}")


def test_reduced_connect_burst_fixture_attributes_first_connected_spike(tmpdir: Path) -> None:
    csv_path = FIXTURES_DIR / "perf_boot_6_connect_burst_reduced.csv"
    out_dir = tmpdir / "perf_boot_6_connect_burst_reduced_out"
    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"connect-burst fixture import errored: rc={result.returncode} stderr={result.stderr}")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    loop_diag = diagnostics["peaks"]["loop_max_peak_us"]
    ble_diag = diagnostics["peaks"]["ble_process_max_peak_us"]
    disp_diag = diagnostics["peaks"]["disp_pipe_max_peak_us"]

    assert_true(loop_diag["millis"] == 9419, f"loop peak millis wrong: {loop_diag}")
    assert_true(loop_diag["likely_phase_bucket"] == "boundary spike during first-connected burst", f"loop phase bucket wrong: {loop_diag}")
    assert_true(loop_diag["top_5_rows"][0]["proxyAdvertising"] == 1, f"loop row should show proxy advertising: {loop_diag}")
    assert_true(loop_diag["top_5_rows"][0]["subscribeStep"] == "COMPLETE", f"loop row should show completed subscribe step: {loop_diag}")
    assert_true(loop_diag["top_5_rows"][0]["bleState"] == "CONNECTED", f"loop row should show connected state: {loop_diag}")

    assert_true(ble_diag["millis"] == 9419, f"ble peak millis wrong: {ble_diag}")
    assert_true(ble_diag["likely_phase_bucket"] == "boundary spike during first-connected burst", f"ble phase bucket wrong: {ble_diag}")
    assert_true(disp_diag["millis"] == 9419, f"display peak millis wrong: {disp_diag}")
    assert_true(disp_diag["likely_phase_bucket"] == "boundary spike during first-connected burst", f"display phase bucket wrong: {disp_diag}")


def test_peak_diagnostics_surface_named_obd_and_wifi_root_causes(tmpdir: Path) -> None:
    csv_path = tmpdir / "named_root_causes.csv"
    out_dir = tmpdir / "named_root_causes_out"
    rows = []
    for index, millis in enumerate((0, 1000, 2000, 3000, 4000)):
        row = base_row(millis, connected=True, header_columns=HEADER_COLUMNS)
        row["rx"] = 150 + (index * 50)
        row["parseOK"] = 150 + (index * 50)
        row["displayUpdates"] = 10 + (index * 5)
        row["bleState"] = 8
        row["subscribeStep"] = 11
        rows.append(row)

    rows[2].update(
        {
            "loopMax_us": 610000,
            "obdMax_us": 602000,
            "obdWriteCallMax_us": 598000,
            "wifiMax_us": 1200,
        }
    )
    rows[4].update(
        {
            "wifiMax_us": 182000,
            "wifiHandleClientMax_us": 179000,
            "fsMax_us": 0,
            "loopMax_us": 110000,
            "obdMax_us": 500,
        }
    )

    write_capture(
        csv_path,
        header_columns=HEADER_COLUMNS,
        sessions=[
            {
                "meta": "#session_start,seq=1,bootId=1,uptime_ms=4000,token=CAUSE001,schema=13",
                "rows": rows,
            }
        ],
    )

    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"named root-cause fixture import errored: rc={result.returncode} stderr={result.stderr}")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    loop_diag = diagnostics["peaks"]["loop_max_peak_us"]
    wifi_diag = diagnostics["peaks"]["wifi_max_peak_us"]

    assert_true(loop_diag["wrapper_symptom_of"] == "obdMax_us", f"loop wrapper symptom wrong: {loop_diag}")
    assert_true(loop_diag["obd_dominant_sync_call_column"] == "obdWriteCallMax_us", f"loop dominant obd call wrong: {loop_diag}")
    assert_true(loop_diag["obd_dominant_sync_call_label"] == "command write", f"loop dominant obd label wrong: {loop_diag}")
    assert_true(loop_diag["root_cause_hint"] == "inline OBD command write stall", f"loop root cause hint wrong: {loop_diag}")

    assert_true(wifi_diag["wifi_dominant_subphase_column"] == "wifiHandleClientMax_us", f"wifi dominant subphase wrong: {wifi_diag}")
    assert_true(wifi_diag["wifi_dominant_subphase_label"] == "HTTP client handling", f"wifi dominant subphase label wrong: {wifi_diag}")
    assert_true(wifi_diag["root_cause_hint"] == "WiFi subphase stall: HTTP client handling", f"wifi root cause hint wrong: {wifi_diag}")


def test_peak_diagnostics_surface_named_wifi_teardown_root_causes(tmpdir: Path) -> None:
    csv_path = tmpdir / "named_wifi_teardown_root_causes.csv"
    out_dir = tmpdir / "named_wifi_teardown_root_causes_out"
    rows = []
    for index, millis in enumerate((0, 1000, 2000, 3000, 4000)):
        row = base_row(millis, connected=True, header_columns=HEADER_COLUMNS)
        row["rx"] = 120 + (index * 10)
        row["parseOK"] = 120 + (index * 10)
        row["bleState"] = 8
        row["subscribeStep"] = 11
        rows.append(row)

    rows[3].update(
        {
            "wifiMax_us": 194000,
            "wifiStopModeOffMax_us": 181000,
            "wifiStopApDisableMax_us": 6000,
            "loopMax_us": 102000,
        }
    )

    write_capture(
        csv_path,
        header_columns=HEADER_COLUMNS,
        sessions=[
            {
                "meta": "#session_start,seq=1,bootId=1,uptime_ms=4000,token=CAUSE002,schema=22",
                "rows": rows,
            }
        ],
    )

    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"wifi teardown fixture import errored: rc={result.returncode} stderr={result.stderr}")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    wifi_diag = diagnostics["peaks"]["wifi_max_peak_us"]

    assert_true(wifi_diag["wifi_dominant_subphase_column"] == "wifiStopModeOffMax_us", f"wifi teardown dominant subphase wrong: {wifi_diag}")
    assert_true(wifi_diag["wifi_dominant_subphase_label"] == "radio off", f"wifi teardown dominant label wrong: {wifi_diag}")
    assert_true(wifi_diag["root_cause_hint"] == "WiFi subphase stall: radio off", f"wifi teardown root cause hint wrong: {wifi_diag}")


def test_peak_diagnostics_surface_named_connect_burst_root_causes(tmpdir: Path) -> None:
    csv_path = tmpdir / "named_connect_burst_root_causes.csv"
    out_dir = tmpdir / "named_connect_burst_root_causes_out"
    rows = []
    for index, millis in enumerate((0, 1000, 2000, 3000, 4000)):
        row = base_row(millis, connected=True, header_columns=HEADER_COLUMNS)
        row["rx"] = 40 + (index * 25)
        row["parseOK"] = 30 + (index * 20)
        row["displayUpdates"] = 5 + (index * 4)
        row["bleState"] = 8
        row["subscribeStep"] = 11
        row["proxyAdvertising"] = 1 if index >= 1 else 0
        rows.append(row)

    rows[1].update(
        {
            "loopMax_us": 138000,
            "bleProcessMax_us": 73100,
            "bleProxyStartMax_us": 70200,
            "dispPipeMax_us": 52000,
            "dispMax_us": 26000,
        }
    )
    rows[2].update(
        {
            "dispPipeMax_us": 76400,
            "dispMax_us": 12000,
            "displayGapRecoverMax_us": 65000,
            "bleProcessMax_us": 600,
            "loopMax_us": 82000,
        }
    )

    write_capture(
        csv_path,
        header_columns=HEADER_COLUMNS,
        sessions=[
            {
                "meta": "#session_start,seq=1,bootId=1,uptime_ms=4000,token=BURST001,schema=18",
                "rows": rows,
            }
        ],
    )

    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"named connect-burst fixture import errored: rc={result.returncode} stderr={result.stderr}")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    loop_diag = diagnostics["peaks"]["loop_max_peak_us"]
    ble_diag = diagnostics["peaks"]["ble_process_max_peak_us"]
    disp_diag = diagnostics["peaks"]["disp_pipe_max_peak_us"]

    assert_true(loop_diag["root_cause_hint"] == "connect-burst BLE subphase: proxy advertising start", f"loop root cause hint wrong: {loop_diag}")
    assert_true(loop_diag["connect_burst_ble_subphase_column"] == "bleProxyStartMax_us", f"loop connect-burst column wrong: {loop_diag}")
    assert_true(ble_diag["root_cause_hint"] == "connect-burst BLE subphase: proxy advertising start", f"ble root cause hint wrong: {ble_diag}")
    assert_true(disp_diag["root_cause_hint"] == "connect-burst display subphase: display gap recovery", f"display root cause hint wrong: {disp_diag}")
    assert_true(disp_diag["connect_burst_display_subphase_column"] == "displayGapRecoverMax_us", f"display connect-burst column wrong: {disp_diag}")


def test_leading_rows_form_implicit_segment(tmpdir: Path) -> None:
    csv_path = tmpdir / "leading_rows.csv"
    out_dir = tmpdir / "leading_rows_out"
    leading_row = base_row(5000, connected=True, header_columns=HEADER_COLUMNS)
    leading_row["rx"] = 150
    leading_row["parseOK"] = 150
    later_session = make_session(
        seq=2,
        token="LATER002",
        schema=13,
        header_columns=HEADER_COLUMNS,
        duration_ms=10000,
        connected=True,
        drive_like=False,
    )
    write_capture(csv_path, header_columns=HEADER_COLUMNS, sessions=[later_session], leading_rows=[leading_row])
    result = run_import(csv_path, out_dir, "--segment", "1")
    assert_true(result.returncode != 3, f"leading-row import failed: {result.stderr}")
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    assert_true(manifest["selected_segment"]["session_index"] == 1, f"leading rows should become segment 1: {manifest}")


def test_import_compare_to_baseline_scores_run_variance(tmpdir: Path) -> None:
    baseline_csv = tmpdir / "baseline.csv"
    candidate_csv = tmpdir / "candidate.csv"
    baseline_out = tmpdir / "baseline_out"
    candidate_out = tmpdir / "candidate_out"
    session = make_session(
        seq=1,
        token="BASECMP1",
        schema=CURRENT_HEADER_SCHEMA,
        header_columns=HEADER_COLUMNS,
        duration_ms=60000,
        connected=True,
        drive_like=True,
    )
    write_capture(baseline_csv, header_columns=HEADER_COLUMNS, sessions=[session])
    write_capture(candidate_csv, header_columns=HEADER_COLUMNS, sessions=[session])

    baseline_result = run_import(baseline_csv, baseline_out, "--stress-class", "core")
    assert_true(baseline_result.returncode != 3, f"baseline import failed: {baseline_result.stderr}")
    candidate_result = run_import(
        candidate_csv,
        candidate_out,
        "--stress-class",
        "core",
        "--compare-to",
        str(baseline_out / "manifest.json"),
    )
    assert_true(candidate_result.returncode == 0, f"candidate compare failed: rc={candidate_result.returncode} stderr={candidate_result.stderr}")
    scoring = json.loads((candidate_out / "scoring.json").read_text(encoding="utf-8"))
    assert_true(scoring["comparison_kind"] == "run_variance", f"expected baseline comparison: {scoring}")
    assert_true(scoring["baseline_window"]["candidate_count"] == 1, f"expected one baseline candidate: {scoring}")
    comparison = (candidate_out / "comparison.txt").read_text(encoding="utf-8")
    assert_true("comparison: run_variance" in comparison, f"comparison should show baseline mode: {comparison}")


def main() -> int:
    test_dma_fragmentation_uses_current_values()
    test_display_peak_metrics_are_imported_from_csv_windows()
    test_connect_burst_and_flush_metrics_are_imported_from_csv_windows()
    print("[perf-csv-import] metric derivation tests passed")

    with tempfile.TemporaryDirectory(prefix="perf_csv_import_") as tmp:
        tmpdir = Path(tmp)
        test_sd_start_runtime_split_uses_fixed_window(tmpdir)
        test_legacy_import_reports_partial_coverage(tmpdir)
        test_schema13_import_supports_drop_metrics(tmpdir)
        test_segment_selection_and_listing_prefers_direct_speed_evidence_when_available(tmpdir)
        test_segment_selection_without_direct_speed_column_falls_back_to_longest_connected(tmpdir)
        test_segment_selection_compat_optional_speed_column_still_supported(tmpdir)
        test_peak_diagnostics_classify_spike_and_attribute_phase(tmpdir)
        test_peak_diagnostics_classify_sustained_runs(tmpdir)
        test_reduced_perf_boot_fixture_attributes_obd_and_wifi_stalls(tmpdir)
        test_reduced_connect_burst_fixture_attributes_first_connected_spike(tmpdir)
        test_peak_diagnostics_surface_named_obd_and_wifi_root_causes(tmpdir)
        test_peak_diagnostics_surface_named_wifi_teardown_root_causes(tmpdir)
        test_peak_diagnostics_surface_named_connect_burst_root_causes(tmpdir)
        test_leading_rows_form_implicit_segment(tmpdir)
        test_import_compare_to_baseline_scores_run_variance(tmpdir)

    print("[perf-csv-import] integration tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[perf-csv-import] FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
