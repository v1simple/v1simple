#!/usr/bin/env python3
"""Regression tests for soak_parse_metrics connect-burst diagnostics."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "soak_parse_metrics.py"
FIXTURE = ROOT / "test" / "fixtures" / "perf" / "core_soak_connect_burst_reduced.metrics.jsonl"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_metrics(path: Path, *extra_args: str) -> dict[str, str]:
    completed = subprocess.run(
        [sys.executable, str(SCRIPT), str(path), *extra_args],
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode == 0, f"parser failed: {completed.stdout}\n{completed.stderr}")
    result: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key] = value
    return result


def write_metrics_jsonl(path: Path) -> None:
    start = datetime(2026, 3, 18, 11, 29, 37, tzinfo=timezone.utc)
    rows = [
        {
            "ts": start.isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTING_WAIT",
                "bleStateCode": 4,
                "subscribeStep": "GET_SERVICE",
                "subscribeStepCode": 0,
                "proxyAdvertising": 0,
                "bleProcessMaxUs": 900,
                "dispPipeMaxUs": 0,
                "dispMaxUs": 0,
                "displayGapRecoverMaxUs": 0,
                "displayFullRenderCount": 0,
                "displayRestingFullRenderCount": 0,
                "displayRestingIncrementalRenderCount": 0,
                "displayPersistedRenderCount": 0,
                "displayPreviewRenderCount": 0,
                "displayRestoreRenderCount": 0,
                "displayLiveScenarioRenderCount": 0,
                "displayRestingScenarioRenderCount": 0,
                "displayPersistedScenarioRenderCount": 0,
                "displayPreviewScenarioRenderCount": 0,
                "displayRestoreScenarioRenderCount": 0,
                "displayRedrawReasonFirstRunCount": 0,
                "displayRedrawReasonEnterLiveCount": 0,
                "displayRedrawReasonLeaveLiveCount": 0,
                "displayRedrawReasonLeavePersistedCount": 0,
                "displayRedrawReasonForceRedrawCount": 0,
                "displayRedrawReasonFrequencyChangeCount": 0,
                "displayRedrawReasonBandSetChangeCount": 0,
                "displayRedrawReasonArrowChangeCount": 0,
                "displayRedrawReasonSignalBarChangeCount": 0,
                "displayRedrawReasonVolumeChangeCount": 0,
                "displayRedrawReasonBogeyCounterChangeCount": 0,
                "displayRedrawReasonRssiRefreshCount": 0,
                "displayRedrawReasonFlashTickCount": 0,
                "displayFullFlushCount": 0,
                "displayPartialFlushCount": 0,
                "displayPartialFlushAreaPeakPx": 0,
                "displayPartialFlushAreaTotalPx": 0,
                "displayFlushEquivalentAreaTotalPx": 0,
                "displayFlushMaxAreaPx": 0,
                "displayBaseFrameMaxUs": 0,
                "displayStatusStripMaxUs": 0,
                "displayFrequencyMaxUs": 0,
                "displayBandsBarsMaxUs": 0,
                "displayArrowsIconsMaxUs": 0,
                "displayFlushSubphaseMaxUs": 0,
                "displayLiveRenderMaxUs": 0,
                "displayRestingRenderMaxUs": 0,
                "displayPersistedRenderMaxUs": 0,
                "displayPreviewRenderMaxUs": 0,
                "displayRestoreRenderMaxUs": 0,
                "displayPreviewFirstRenderMaxUs": 0,
                "displayPreviewSteadyRenderMaxUs": 0,
                "bleFollowupRequestAlertMaxUs": 0,
                "bleFollowupRequestVersionMaxUs": 0,
                "bleConnectStableCallbackMaxUs": 0,
                "bleProxyStartMaxUs": 0,
                "eventBus": {"publishCount": 0, "dropCount": 0, "size": 1},
            },
        },
        {
            "ts": (start + timedelta(seconds=1)).isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTED",
                "bleStateCode": 8,
                "subscribeStep": "COMPLETE",
                "subscribeStepCode": 11,
                "proxyAdvertising": 1,
                "bleProcessMaxUs": 62000,
                "dispPipeMaxUs": 71000,
                "dispMaxUs": 44000,
                "displayGapRecoverMaxUs": 3000,
                "displayFullRenderCount": 1,
                "displayRestingFullRenderCount": 0,
                "displayRestingIncrementalRenderCount": 0,
                "displayPersistedRenderCount": 0,
                "displayPreviewRenderCount": 0,
                "displayRestoreRenderCount": 0,
                "displayLiveScenarioRenderCount": 1,
                "displayRestingScenarioRenderCount": 0,
                "displayPersistedScenarioRenderCount": 0,
                "displayPreviewScenarioRenderCount": 0,
                "displayRestoreScenarioRenderCount": 0,
                "displayRedrawReasonFirstRunCount": 1,
                "displayRedrawReasonEnterLiveCount": 1,
                "displayRedrawReasonLeaveLiveCount": 0,
                "displayRedrawReasonLeavePersistedCount": 0,
                "displayRedrawReasonForceRedrawCount": 0,
                "displayRedrawReasonFrequencyChangeCount": 1,
                "displayRedrawReasonBandSetChangeCount": 1,
                "displayRedrawReasonArrowChangeCount": 1,
                "displayRedrawReasonSignalBarChangeCount": 0,
                "displayRedrawReasonVolumeChangeCount": 0,
                "displayRedrawReasonBogeyCounterChangeCount": 0,
                "displayRedrawReasonRssiRefreshCount": 0,
                "displayRedrawReasonFlashTickCount": 0,
                "displayFullFlushCount": 1,
                "displayPartialFlushCount": 0,
                "displayPartialFlushAreaPeakPx": 0,
                "displayPartialFlushAreaTotalPx": 0,
                "displayFlushEquivalentAreaTotalPx": 153664,
                "displayFlushMaxAreaPx": 153664,
                "displayBaseFrameMaxUs": 9000,
                "displayStatusStripMaxUs": 2500,
                "displayFrequencyMaxUs": 14000,
                "displayBandsBarsMaxUs": 6000,
                "displayArrowsIconsMaxUs": 3500,
                "displayFlushSubphaseMaxUs": 21000,
                "displayLiveRenderMaxUs": 44000,
                "displayRestingRenderMaxUs": 0,
                "displayPersistedRenderMaxUs": 0,
                "displayPreviewRenderMaxUs": 0,
                "displayRestoreRenderMaxUs": 0,
                "displayPreviewFirstRenderMaxUs": 0,
                "displayPreviewSteadyRenderMaxUs": 0,
                "bleFollowupRequestAlertMaxUs": 8000,
                "bleFollowupRequestVersionMaxUs": 17000,
                "bleConnectStableCallbackMaxUs": 21000,
                "bleProxyStartMaxUs": 58000,
                "eventBus": {"publishCount": 3, "dropCount": 0, "size": 2},
            },
        },
        {
            "ts": (start + timedelta(seconds=2)).isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTED",
                "bleStateCode": 8,
                "subscribeStep": "COMPLETE",
                "subscribeStepCode": 11,
                "proxyAdvertising": 1,
                "bleProcessMaxUs": 12000,
                "dispPipeMaxUs": 32000,
                "dispMaxUs": 15000,
                "displayGapRecoverMaxUs": 1200,
                "displayFullRenderCount": 1,
                "displayRestingFullRenderCount": 0,
                "displayRestingIncrementalRenderCount": 0,
                "displayPersistedRenderCount": 0,
                "displayPreviewRenderCount": 0,
                "displayRestoreRenderCount": 0,
                "displayLiveScenarioRenderCount": 2,
                "displayRestingScenarioRenderCount": 0,
                "displayPersistedScenarioRenderCount": 0,
                "displayPreviewScenarioRenderCount": 0,
                "displayRestoreScenarioRenderCount": 0,
                "displayRedrawReasonFirstRunCount": 1,
                "displayRedrawReasonEnterLiveCount": 1,
                "displayRedrawReasonLeaveLiveCount": 0,
                "displayRedrawReasonLeavePersistedCount": 0,
                "displayRedrawReasonForceRedrawCount": 0,
                "displayRedrawReasonFrequencyChangeCount": 1,
                "displayRedrawReasonBandSetChangeCount": 1,
                "displayRedrawReasonArrowChangeCount": 1,
                "displayRedrawReasonSignalBarChangeCount": 0,
                "displayRedrawReasonVolumeChangeCount": 0,
                "displayRedrawReasonBogeyCounterChangeCount": 0,
                "displayRedrawReasonRssiRefreshCount": 1,
                "displayRedrawReasonFlashTickCount": 0,
                "displayFullFlushCount": 1,
                "displayPartialFlushCount": 1,
                "displayPartialFlushAreaPeakPx": 4096,
                "displayPartialFlushAreaTotalPx": 4096,
                "displayFlushEquivalentAreaTotalPx": 157760,
                "displayFlushMaxAreaPx": 153664,
                "displayBaseFrameMaxUs": 9000,
                "displayStatusStripMaxUs": 2500,
                "displayFrequencyMaxUs": 14000,
                "displayBandsBarsMaxUs": 6000,
                "displayArrowsIconsMaxUs": 3500,
                "displayFlushSubphaseMaxUs": 21000,
                "displayLiveRenderMaxUs": 44000,
                "displayRestingRenderMaxUs": 0,
                "displayPersistedRenderMaxUs": 0,
                "displayPreviewRenderMaxUs": 0,
                "displayRestoreRenderMaxUs": 0,
                "displayPreviewFirstRenderMaxUs": 0,
                "displayPreviewSteadyRenderMaxUs": 0,
                "bleFollowupRequestAlertMaxUs": 0,
                "bleFollowupRequestVersionMaxUs": 0,
                "bleConnectStableCallbackMaxUs": 0,
                "bleProxyStartMaxUs": 0,
                "eventBus": {"publishCount": 5, "dropCount": 1, "size": 4},
            },
        },
        {
            "ts": (start + timedelta(seconds=3)).isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTED",
                "bleStateCode": 8,
                "subscribeStep": "COMPLETE",
                "subscribeStepCode": 11,
                "proxyAdvertising": 1,
                "bleProcessMaxUs": 14000,
                "dispPipeMaxUs": 28000,
                "dispMaxUs": 11000,
                "displayGapRecoverMaxUs": 800,
                "displayFullRenderCount": 1,
                "displayRestingFullRenderCount": 0,
                "displayRestingIncrementalRenderCount": 0,
                "displayPersistedRenderCount": 0,
                "displayPreviewRenderCount": 0,
                "displayRestoreRenderCount": 0,
                "displayLiveScenarioRenderCount": 3,
                "displayRestingScenarioRenderCount": 0,
                "displayPersistedScenarioRenderCount": 0,
                "displayPreviewScenarioRenderCount": 0,
                "displayRestoreScenarioRenderCount": 0,
                "displayRedrawReasonFirstRunCount": 1,
                "displayRedrawReasonEnterLiveCount": 1,
                "displayRedrawReasonLeaveLiveCount": 0,
                "displayRedrawReasonLeavePersistedCount": 0,
                "displayRedrawReasonForceRedrawCount": 0,
                "displayRedrawReasonFrequencyChangeCount": 1,
                "displayRedrawReasonBandSetChangeCount": 1,
                "displayRedrawReasonArrowChangeCount": 1,
                "displayRedrawReasonSignalBarChangeCount": 0,
                "displayRedrawReasonVolumeChangeCount": 0,
                "displayRedrawReasonBogeyCounterChangeCount": 1,
                "displayRedrawReasonRssiRefreshCount": 1,
                "displayRedrawReasonFlashTickCount": 1,
                "displayFullFlushCount": 1,
                "displayPartialFlushCount": 2,
                "displayPartialFlushAreaPeakPx": 8192,
                "displayPartialFlushAreaTotalPx": 12288,
                "displayFlushEquivalentAreaTotalPx": 165952,
                "displayFlushMaxAreaPx": 153664,
                "displayBaseFrameMaxUs": 9000,
                "displayStatusStripMaxUs": 2500,
                "displayFrequencyMaxUs": 14000,
                "displayBandsBarsMaxUs": 6000,
                "displayArrowsIconsMaxUs": 3500,
                "displayFlushSubphaseMaxUs": 21000,
                "displayLiveRenderMaxUs": 44000,
                "displayRestingRenderMaxUs": 0,
                "displayPersistedRenderMaxUs": 0,
                "displayPreviewRenderMaxUs": 0,
                "displayRestoreRenderMaxUs": 0,
                "displayPreviewFirstRenderMaxUs": 0,
                "displayPreviewSteadyRenderMaxUs": 0,
                "bleFollowupRequestAlertMaxUs": 0,
                "bleFollowupRequestVersionMaxUs": 0,
                "bleConnectStableCallbackMaxUs": 0,
                "bleProxyStartMaxUs": 0,
                "eventBus": {"publishCount": 6, "dropCount": 1, "size": 3},
            },
        },
        {
            "ts": (start + timedelta(seconds=4)).isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTED",
                "bleStateCode": 8,
                "subscribeStep": "COMPLETE",
                "subscribeStepCode": 11,
                "proxyAdvertising": 1,
                "bleProcessMaxUs": 9000,
                "dispPipeMaxUs": 12000,
                "dispMaxUs": 6000,
                "displayGapRecoverMaxUs": 0,
                "displayFullRenderCount": 1,
                "displayRestingFullRenderCount": 0,
                "displayRestingIncrementalRenderCount": 0,
                "displayPersistedRenderCount": 0,
                "displayPreviewRenderCount": 2,
                "displayRestoreRenderCount": 1,
                "displayLiveScenarioRenderCount": 4,
                "displayRestingScenarioRenderCount": 0,
                "displayPersistedScenarioRenderCount": 0,
                "displayPreviewScenarioRenderCount": 2,
                "displayRestoreScenarioRenderCount": 1,
                "displayRedrawReasonFirstRunCount": 1,
                "displayRedrawReasonEnterLiveCount": 1,
                "displayRedrawReasonLeaveLiveCount": 0,
                "displayRedrawReasonLeavePersistedCount": 0,
                "displayRedrawReasonForceRedrawCount": 1,
                "displayRedrawReasonFrequencyChangeCount": 1,
                "displayRedrawReasonBandSetChangeCount": 1,
                "displayRedrawReasonArrowChangeCount": 1,
                "displayRedrawReasonSignalBarChangeCount": 0,
                "displayRedrawReasonVolumeChangeCount": 1,
                "displayRedrawReasonBogeyCounterChangeCount": 1,
                "displayRedrawReasonRssiRefreshCount": 1,
                "displayRedrawReasonFlashTickCount": 1,
                "displayFullFlushCount": 1,
                "displayPartialFlushCount": 2,
                "displayPartialFlushAreaPeakPx": 8192,
                "displayPartialFlushAreaTotalPx": 12288,
                "displayFlushEquivalentAreaTotalPx": 165952,
                "displayFlushMaxAreaPx": 153664,
                "displayBaseFrameMaxUs": 9000,
                "displayStatusStripMaxUs": 2500,
                "displayFrequencyMaxUs": 14000,
                "displayBandsBarsMaxUs": 6000,
                "displayArrowsIconsMaxUs": 3500,
                "displayFlushSubphaseMaxUs": 21000,
                "displayLiveRenderMaxUs": 44000,
                "displayRestingRenderMaxUs": 7000,
                "displayPersistedRenderMaxUs": 9000,
                "displayPreviewRenderMaxUs": 19000,
                "displayRestoreRenderMaxUs": 13000,
                "displayPreviewFirstRenderMaxUs": 19000,
                "displayPreviewSteadyRenderMaxUs": 11000,
                "bleFollowupRequestAlertMaxUs": 0,
                "bleFollowupRequestVersionMaxUs": 0,
                "bleConnectStableCallbackMaxUs": 0,
                "bleProxyStartMaxUs": 0,
                "eventBus": {"publishCount": 8, "dropCount": 1, "size": 5},
            },
        },
    ]
    partial_shape_metrics = [
        {
            "displayPartialFlushLogicalWidthPeakPx": 0,
            "displayPartialFlushLogicalHeightPeakPx": 0,
            "displayPartialFlushRowCallsPeak": 0,
            "displayPartialFlushPixelsPerRowPeakPx": 0,
            "displayPartialFlushUsPeak": 0,
            "displayPartialFlushWorstUsLogicalWidthPx": 0,
            "displayPartialFlushWorstUsLogicalHeightPx": 0,
            "displayPartialFlushWorstUsAreaPx": 0,
            "displayPartialFlushWouldFullRows64Count": 0,
            "displayPartialFlushWouldFullRows128Count": 0,
            "displayPartialFlushWouldFullRows256Count": 0,
        },
        {
            "displayPartialFlushLogicalWidthPeakPx": 0,
            "displayPartialFlushLogicalHeightPeakPx": 0,
            "displayPartialFlushRowCallsPeak": 0,
            "displayPartialFlushPixelsPerRowPeakPx": 0,
            "displayPartialFlushUsPeak": 0,
            "displayPartialFlushWorstUsLogicalWidthPx": 0,
            "displayPartialFlushWorstUsLogicalHeightPx": 0,
            "displayPartialFlushWorstUsAreaPx": 0,
            "displayPartialFlushWouldFullRows64Count": 0,
            "displayPartialFlushWouldFullRows128Count": 0,
            "displayPartialFlushWouldFullRows256Count": 0,
        },
        {
            "displayPartialFlushLogicalWidthPeakPx": 80,
            "displayPartialFlushLogicalHeightPeakPx": 52,
            "displayPartialFlushRowCallsPeak": 80,
            "displayPartialFlushPixelsPerRowPeakPx": 52,
            "displayPartialFlushUsPeak": 2500,
            "displayPartialFlushWorstUsLogicalWidthPx": 80,
            "displayPartialFlushWorstUsLogicalHeightPx": 52,
            "displayPartialFlushWorstUsAreaPx": 4096,
            "displayPartialFlushWouldFullRows64Count": 1,
            "displayPartialFlushWouldFullRows128Count": 0,
            "displayPartialFlushWouldFullRows256Count": 0,
        },
        {
            "displayPartialFlushLogicalWidthPeakPx": 160,
            "displayPartialFlushLogicalHeightPeakPx": 52,
            "displayPartialFlushRowCallsPeak": 160,
            "displayPartialFlushPixelsPerRowPeakPx": 52,
            "displayPartialFlushUsPeak": 4200,
            "displayPartialFlushWorstUsLogicalWidthPx": 160,
            "displayPartialFlushWorstUsLogicalHeightPx": 52,
            "displayPartialFlushWorstUsAreaPx": 8192,
            "displayPartialFlushWouldFullRows64Count": 2,
            "displayPartialFlushWouldFullRows128Count": 1,
            "displayPartialFlushWouldFullRows256Count": 0,
        },
        {
            "displayPartialFlushLogicalWidthPeakPx": 160,
            "displayPartialFlushLogicalHeightPeakPx": 52,
            "displayPartialFlushRowCallsPeak": 160,
            "displayPartialFlushPixelsPerRowPeakPx": 52,
            "displayPartialFlushUsPeak": 4200,
            "displayPartialFlushWorstUsLogicalWidthPx": 160,
            "displayPartialFlushWorstUsLogicalHeightPx": 52,
            "displayPartialFlushWorstUsAreaPx": 8192,
            "displayPartialFlushWouldFullRows64Count": 2,
            "displayPartialFlushWouldFullRows128Count": 1,
            "displayPartialFlushWouldFullRows256Count": 0,
        },
    ]
    for row, metrics in zip(rows, partial_shape_metrics):
        row["data"].update(metrics)
    path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


def test_reduced_fixture_surfaces_unstable_connect_burst() -> None:
    parsed = parse_metrics(
        FIXTURE,
        "--ble-threshold",
        "25000",
        "--connect-burst-disp-threshold",
        "50000",
        "--connect-burst-consecutive-samples",
        "3",
    )
    assert_true(parsed["connect_burst_detected"] == "1", f"burst not detected: {parsed}")
    assert_true(parsed["connect_burst_stabilized"] == "0", f"fixture should remain unstable: {parsed}")
    assert_true(parsed["connect_burst_event_ble_state"] == "CONNECTED", f"wrong event state: {parsed}")
    assert_true(parsed["connect_burst_event_subscribe_step"] == "COMPLETE", f"wrong subscribe step: {parsed}")
    assert_true(parsed["connect_burst_event_proxy_advertising"] == "1", f"wrong proxy flag: {parsed}")
    assert_true(parsed["connect_burst_pre_ble_process_peak"] == "73755", f"wrong ble peak: {parsed}")
    assert_true(parsed["connect_burst_pre_disp_pipe_peak"] == "73276", f"wrong display peak: {parsed}")
    assert_true(parsed["event_publish_delta"] == "76", f"wrong event publish delta: {parsed}")
    assert_true(parsed["event_drop_delta"] == "0", f"wrong event drop delta: {parsed}")
    assert_true(parsed["event_size_peak"] == "1", f"wrong event size peak: {parsed}")
    assert_true(parsed["connect_burst_samples_to_stable"] == "", f"unexpected settle result: {parsed}")
    assert_true(parsed["connect_burst_time_to_stable_ms"] == "", f"unexpected settle time: {parsed}")


def test_synthetic_fixture_tracks_root_causes_and_settle_window() -> None:
    with tempfile.TemporaryDirectory() as tmp_dir:
        path = Path(tmp_dir) / "metrics.jsonl"
        write_metrics_jsonl(path)
        parsed = parse_metrics(
            path,
            "--ble-threshold",
            "25000",
            "--connect-burst-disp-threshold",
            "50000",
            "--connect-burst-consecutive-samples",
            "3",
        )

    assert_true(parsed["connect_burst_detected"] == "1", f"burst not detected: {parsed}")
    assert_true(parsed["connect_burst_stabilized"] == "1", f"burst should stabilize: {parsed}")
    assert_true(parsed["connect_burst_event_index"] == "1", f"wrong event index: {parsed}")
    assert_true(parsed["connect_burst_stable_index"] == "4", f"wrong stable index: {parsed}")
    assert_true(parsed["connect_burst_samples_to_stable"] == "4", f"wrong samples-to-stable: {parsed}")
    assert_true(parsed["connect_burst_time_to_stable_ms"] == "3000", f"wrong time-to-stable: {parsed}")
    assert_true(parsed["connect_burst_pre_ble_process_peak"] == "62000", f"wrong ble peak: {parsed}")
    assert_true(parsed["connect_burst_pre_disp_pipe_peak"] == "71000", f"wrong disp peak: {parsed}")
    assert_true(parsed["connect_burst_ble_followup_request_alert_peak"] == "8000", f"wrong alert peak: {parsed}")
    assert_true(parsed["connect_burst_ble_followup_request_version_peak"] == "17000", f"wrong version peak: {parsed}")
    assert_true(parsed["connect_burst_ble_connect_stable_callback_peak"] == "21000", f"wrong stable callback peak: {parsed}")
    assert_true(parsed["connect_burst_ble_proxy_start_peak"] == "58000", f"wrong proxy peak: {parsed}")
    assert_true(parsed["connect_burst_disp_render_peak"] == "44000", f"wrong render peak: {parsed}")
    assert_true(parsed["connect_burst_display_gap_recover_peak"] == "3000", f"wrong gap-recover peak: {parsed}")
    assert_true(parsed["connect_burst_display_base_frame_peak"] == "9000", f"wrong base-frame peak: {parsed}")
    assert_true(parsed["connect_burst_display_frequency_peak"] == "14000", f"wrong frequency peak: {parsed}")
    assert_true(parsed["connect_burst_display_flush_subphase_peak"] == "21000", f"wrong flush subphase peak: {parsed}")
    assert_true(parsed["event_publish_delta"] == "8", f"wrong event publish delta: {parsed}")
    assert_true(parsed["event_drop_delta"] == "1", f"wrong event drop delta: {parsed}")
    assert_true(parsed["event_size_peak"] == "5", f"wrong event size peak: {parsed}")
    assert_true(parsed["display_full_render_count_delta"] == "1", f"wrong full-render delta: {parsed}")
    assert_true(parsed["display_live_scenario_render_count_delta"] == "4", f"wrong live-scenario delta: {parsed}")
    assert_true(parsed["display_partial_flush_count_delta"] == "2", f"wrong partial-flush delta: {parsed}")
    assert_true(parsed["display_partial_flush_area_peak_px"] == "8192", f"wrong partial-flush area peak: {parsed}")
    assert_true(parsed["display_partial_flush_logical_width_peak_px"] == "160", f"wrong partial width peak: {parsed}")
    assert_true(parsed["display_partial_flush_logical_height_peak_px"] == "52", f"wrong partial height peak: {parsed}")
    assert_true(parsed["display_partial_flush_row_calls_peak"] == "160", f"wrong partial row-call peak: {parsed}")
    assert_true(parsed["display_partial_flush_pixels_per_row_peak_px"] == "52", f"wrong partial pixels-per-row peak: {parsed}")
    assert_true(parsed["display_partial_flush_us_peak_us"] == "4200", f"wrong partial us peak: {parsed}")
    assert_true(parsed["display_partial_flush_worst_us_logical_width_px"] == "160", f"wrong worst-width metric: {parsed}")
    assert_true(parsed["display_partial_flush_worst_us_logical_height_px"] == "52", f"wrong worst-height metric: {parsed}")
    assert_true(parsed["display_partial_flush_worst_us_area_px"] == "8192", f"wrong worst-area metric: {parsed}")
    assert_true(parsed["display_partial_flush_would_full_rows64_count_delta"] == "2", f"wrong rows64 shadow delta: {parsed}")
    assert_true(parsed["display_partial_flush_would_full_rows128_count_delta"] == "1", f"wrong rows128 shadow delta: {parsed}")
    assert_true(parsed["display_partial_flush_would_full_rows256_count_delta"] == "0", f"wrong rows256 shadow delta: {parsed}")
    assert_true(parsed["display_flush_max_area_px"] == "153664", f"wrong flush-max area: {parsed}")
    assert_true(parsed["display_preview_render_peak"] == "19000", f"wrong preview render peak: {parsed}")
    assert_true(parsed["display_restore_render_peak"] == "13000", f"wrong restore render peak: {parsed}")
    assert_true(parsed["display_preview_first_render_peak"] == "19000", f"wrong first preview peak: {parsed}")
    assert_true(parsed["display_preview_steady_render_peak"] == "11000", f"wrong steady preview peak: {parsed}")


def main() -> int:
    test_reduced_fixture_surfaces_unstable_connect_burst()
    test_synthetic_fixture_tracks_root_causes_and_settle_window()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
