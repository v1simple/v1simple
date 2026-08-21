#!/usr/bin/env python3
"""Canonical metric schema shared by offline observability tooling."""

from __future__ import annotations

from collections.abc import Iterable
import json
from pathlib import Path

CURRENT_PERF_CSV_SCHEMA = 48
MIN_DROP_COUNTER_SCHEMA = 13
MIN_NOTIFY_PIPELINE_COMPLETE_SCHEMA = 47

CATALOG_PATH = Path(__file__).with_name("hardware_metric_catalog.json")


def load_catalog_units(path: Path = CATALOG_PATH) -> dict[str, str]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    rows = payload.get("metrics") if isinstance(payload, dict) else None
    if not isinstance(rows, list):
        raise RuntimeError(f"Invalid hardware metric catalog: {path}")

    units: dict[str, str] = {}
    for index, row in enumerate(rows):
        if not isinstance(row, dict) or not isinstance(row.get("metric"), str) or not isinstance(row.get("unit"), str):
            raise RuntimeError(f"Invalid metric catalog entry at index {index}: {path}")
        metric = row["metric"]
        unit = row["unit"]
        if metric in units and units[metric] != unit:
            raise RuntimeError(f"Conflicting units for {metric}: {units[metric]} and {unit}")
        units[metric] = unit
    return units


CANONICAL_METRIC_UNITS = load_catalog_units()

CSV_DELTA_COLUMNS = {
    "rx_packets_delta": "rx",
    "parse_successes_delta": "parseOK",
    "parse_failures_delta": "parseFail",
    "parse_resyncs_delta": "parseResync",
    "queue_drops_delta": "qDrop",
    "perf_drop_delta": "perfDrop",
    "oversize_drops_delta": "oversizeDrops",
    "display_updates_delta": "displayUpdates",
    "display_skips_delta": "displaySkips",
    "reconnects_delta": "reconn",
    "disconnects_delta": "disc",
    "ble_mutex_timeout_delta": "bleMutexTimeout",
    "wifi_connect_deferred_delta": "wifiConnectDeferred",
    "display_partial_flush_would_full_rows64_count_delta": "displayPartialFlushWouldFullRows64Count",
    "display_partial_flush_would_full_rows128_count_delta": "displayPartialFlushWouldFullRows128Count",
    "display_partial_flush_would_full_rows256_count_delta": "displayPartialFlushWouldFullRows256Count",
    "display_union_exceeds_cap_with_frequency_count_delta": "displayUnionExceedsCapWithFrequencyCount",
    "display_union_exceeds_cap_with_bands_bars_count_delta": "displayUnionExceedsCapWithBandsBarsCount",
    "display_union_exceeds_cap_with_arrows_count_delta": "displayUnionExceedsCapWithArrowsCount",
    "display_union_exceeds_cap_with_status_count_delta": "displayUnionExceedsCapWithStatusCount",
    "display_union_exceeds_cap_with_indicators_count_delta": "displayUnionExceedsCapWithIndicatorsCount",
    "display_union_exceeds_cap_with_external_count_delta": "displayUnionExceedsCapWithExternalCount",
    "display_union_exceeds_cap_unclassified_count_delta": "displayUnionExceedsCapUnclassifiedCount",
    "display_resting_flush_reason_full_redraw_count_delta": "displayRestingFlushReasonFullRedrawCount",
    "display_resting_flush_reason_pending_external_count_delta": "displayRestingFlushReasonPendingExternalCount",
    "display_resting_flush_reason_painted_count_delta": "displayRestingFlushReasonPaintedCount",
    "display_resting_flush_reason_cache_hit_count_delta": "displayRestingFlushReasonCacheHitCount",
    "display_persisted_flush_reason_full_redraw_count_delta": "displayPersistedFlushReasonFullRedrawCount",
    "display_persisted_flush_reason_pending_external_count_delta": "displayPersistedFlushReasonPendingExternalCount",
    "display_persisted_flush_reason_painted_count_delta": "displayPersistedFlushReasonPaintedCount",
    "display_persisted_flush_reason_cache_hit_count_delta": "displayPersistedFlushReasonCacheHitCount",
    "display_status_volume_paint_count_delta": "displayStatusVolumePaintCount",
    "display_status_rssi_paint_count_delta": "displayStatusRssiPaintCount",
    "display_status_profile_paint_count_delta": "displayStatusProfilePaintCount",
    "display_status_battery_paint_count_delta": "displayStatusBatteryPaintCount",
    "display_status_ble_proxy_paint_count_delta": "displayStatusBleProxyPaintCount",
    "display_status_wifi_paint_count_delta": "displayStatusWifiPaintCount",
    "display_status_obd_paint_count_delta": "displayStatusObdPaintCount",
    "display_status_gps_paint_count_delta": "displayStatusGpsPaintCount",
    "display_status_alp_paint_count_delta": "displayStatusAlpPaintCount",
}

CSV_PEAK_DIAGNOSTIC_COLUMNS = {
    "loop_max_peak_us": "loopMax_us",
    "ble_process_max_peak_us": "bleProcessMax_us",
    "wifi_max_peak_us": "wifiMax_us",
    "disp_pipe_max_peak_us": "dispPipeMax_us",
    "display_partial_flush_logical_width_peak_px": "displayPartialFlushLogicalWidthPeakPx",
    "display_partial_flush_logical_height_peak_px": "displayPartialFlushLogicalHeightPeakPx",
    "display_partial_flush_row_calls_peak": "displayPartialFlushRowCallsPeak",
    "display_partial_flush_pixels_per_row_peak_px": "displayPartialFlushPixelsPerRowPeakPx",
    "display_partial_flush_us_peak_us": "displayPartialFlushUsPeak_us",
    "display_partial_flush_worst_us_logical_width_px": "displayPartialFlushWorstUsLogicalWidthPx",
    "display_partial_flush_worst_us_logical_height_px": "displayPartialFlushWorstUsLogicalHeightPx",
    "display_partial_flush_worst_us_area_px": "displayPartialFlushWorstUsAreaPx",
    "display_union_exceeds_cap_area_peak_px": "displayUnionExceedsCapAreaPeakPx",
    "display_union_exceeds_cap_rect_count_peak": "displayUnionExceedsCapRectCountPeak",
    "display_union_exceeds_cap_area_peak_source_mask": "displayUnionExceedsCapAreaPeakSourceMask",
}

CSV_PEAK_ONLY_COLUMNS = {
    "loop_max_peak_us": "loopMax_us",
    "flush_max_peak_us": "flushMax_us",
    "wifi_max_peak_us": "wifiMax_us",
    "ble_drain_max_peak_us": "bleDrainMax_us",
    "sd_max_peak_us": "sdMax_us",
    "fs_max_peak_us": "fsMax_us",
    "queue_high_water_peak": "queueHighWater",
    "ble_process_max_peak_us": "bleProcessMax_us",
    "disp_pipe_max_peak_us": "dispPipeMax_us",
    "display_partial_flush_logical_width_peak_px": "displayPartialFlushLogicalWidthPeakPx",
    "display_partial_flush_logical_height_peak_px": "displayPartialFlushLogicalHeightPeakPx",
    "display_partial_flush_area_peak_px": "displayPartialFlushAreaPeakPx",
    "display_flush_max_area_px": "displayFlushMaxAreaPx",
    "display_partial_flush_row_calls_peak": "displayPartialFlushRowCallsPeak",
    "display_partial_flush_pixels_per_row_peak_px": "displayPartialFlushPixelsPerRowPeakPx",
    "display_partial_flush_us_peak_us": "displayPartialFlushUsPeak_us",
    "display_partial_flush_worst_us_logical_width_px": "displayPartialFlushWorstUsLogicalWidthPx",
    "display_partial_flush_worst_us_logical_height_px": "displayPartialFlushWorstUsLogicalHeightPx",
    "display_partial_flush_worst_us_area_px": "displayPartialFlushWorstUsAreaPx",
    "display_union_exceeds_cap_area_peak_px": "displayUnionExceedsCapAreaPeakPx",
    "display_union_exceeds_cap_rect_count_peak": "displayUnionExceedsCapRectCountPeak",
    "display_union_exceeds_cap_area_peak_source_mask": "displayUnionExceedsCapAreaPeakSourceMask",
    "display_base_frame_peak_us": "displayBaseFrameMax_us",
    "display_status_strip_peak_us": "displayStatusStripMax_us",
    "display_frequency_peak_us": "displayFrequencyMax_us",
    "display_bands_bars_peak_us": "displayBandsBarsMax_us",
    "display_arrows_icons_peak_us": "displayArrowsIconsMax_us",
    "display_flush_subphase_peak_us": "displayFlushSubphaseMax_us",
    "display_live_render_peak_us": "displayLiveRenderMax_us",
    "display_resting_render_peak_us": "displayRestingRenderMax_us",
    "display_persisted_render_peak_us": "displayPersistedRenderMax_us",
    "display_preview_render_peak_us": "displayPreviewRenderMax_us",
    "display_restore_render_peak_us": "displayRestoreRenderMax_us",
    "display_preview_first_render_peak_us": "displayPreviewFirstRenderMax_us",
    "display_preview_steady_render_peak_us": "displayPreviewSteadyRenderMax_us",
}

CSV_CONNECT_BURST_PEAK_COLUMNS = {
    "connect_burst_pre_ble_process_peak_us": "bleProcessMax_us",
    "connect_burst_pre_disp_pipe_peak_us": "dispPipeMax_us",
    "connect_burst_ble_followup_request_alert_peak_us": "bleFollowupRequestAlertMax_us",
    "connect_burst_ble_followup_request_version_peak_us": "bleFollowupRequestVersionMax_us",
    "connect_burst_ble_connect_stable_callback_peak_us": "bleConnectStableCallbackMax_us",
    "connect_burst_ble_proxy_start_peak_us": "bleProxyStartMax_us",
    "connect_burst_disp_render_peak_us": "dispMax_us",
    "connect_burst_display_base_frame_peak_us": "displayBaseFrameMax_us",
    "connect_burst_display_status_strip_peak_us": "displayStatusStripMax_us",
    "connect_burst_display_frequency_peak_us": "displayFrequencyMax_us",
    "connect_burst_display_bands_bars_peak_us": "displayBandsBarsMax_us",
    "connect_burst_display_arrows_icons_peak_us": "displayArrowsIconsMax_us",
    "connect_burst_display_flush_subphase_peak_us": "displayFlushSubphaseMax_us",
}

PERF_CSV_ALWAYS_UNSUPPORTED_METRICS = frozenset(
    {
        "samples_to_stable",
        "time_to_stable_ms",
        "connect_burst_samples_to_stable",
        "connect_burst_time_to_stable_ms",
    }
)
PERF_CSV_LEGACY_UNSUPPORTED_METRICS = frozenset({"perf_drop_delta"})
PERF_CSV_NOTIFY_PIPELINE_COMPLETE_METRICS = frozenset(
    {
        "notify_to_display_pipeline_complete_max_ms",
        "notify_to_display_pipeline_complete_sample_count",
    }
)
PERF_CSV_NOTIFY_PIPELINE_COMPLETE_COLUMNS = frozenset(
    {
        "notifyToDisplayPipelineCompleteMax_ms",
        "notifyToDisplayPipelineCompleteTotalCount",
    }
)

DISPLAY_COUNTER_DELTA_MAPPINGS = (
    ("displayFullRenderCount", "display_full_render_count_delta"),
    ("displayRestingFullRenderCount", "display_resting_full_render_count_delta"),
    ("displayRestingIncrementalRenderCount", "display_resting_incremental_render_count_delta"),
    ("displayPersistedRenderCount", "display_persisted_render_count_delta"),
    ("displayPreviewRenderCount", "display_preview_render_count_delta"),
    ("displayRestoreRenderCount", "display_restore_render_count_delta"),
    ("displayLiveScenarioRenderCount", "display_live_scenario_render_count_delta"),
    ("displayRestingScenarioRenderCount", "display_resting_scenario_render_count_delta"),
    ("displayPersistedScenarioRenderCount", "display_persisted_scenario_render_count_delta"),
    ("displayPreviewScenarioRenderCount", "display_preview_scenario_render_count_delta"),
    ("displayRestoreScenarioRenderCount", "display_restore_scenario_render_count_delta"),
    ("displayRestingFlushReasonFullRedrawCount", "display_resting_flush_reason_full_redraw_count_delta"),
    ("displayRestingFlushReasonPendingExternalCount", "display_resting_flush_reason_pending_external_count_delta"),
    ("displayRestingFlushReasonPaintedCount", "display_resting_flush_reason_painted_count_delta"),
    ("displayRestingFlushReasonCacheHitCount", "display_resting_flush_reason_cache_hit_count_delta"),
    ("displayPersistedFlushReasonFullRedrawCount", "display_persisted_flush_reason_full_redraw_count_delta"),
    ("displayPersistedFlushReasonPendingExternalCount", "display_persisted_flush_reason_pending_external_count_delta"),
    ("displayPersistedFlushReasonPaintedCount", "display_persisted_flush_reason_painted_count_delta"),
    ("displayPersistedFlushReasonCacheHitCount", "display_persisted_flush_reason_cache_hit_count_delta"),
    ("displayStatusVolumePaintCount", "display_status_volume_paint_count_delta"),
    ("displayStatusRssiPaintCount", "display_status_rssi_paint_count_delta"),
    ("displayStatusProfilePaintCount", "display_status_profile_paint_count_delta"),
    ("displayStatusBatteryPaintCount", "display_status_battery_paint_count_delta"),
    ("displayStatusBleProxyPaintCount", "display_status_ble_proxy_paint_count_delta"),
    ("displayStatusWifiPaintCount", "display_status_wifi_paint_count_delta"),
    ("displayStatusObdPaintCount", "display_status_obd_paint_count_delta"),
    ("displayStatusGpsPaintCount", "display_status_gps_paint_count_delta"),
    ("displayStatusAlpPaintCount", "display_status_alp_paint_count_delta"),
    ("displayRedrawReasonFirstRunCount", "display_redraw_reason_first_run_count_delta"),
    ("displayRedrawReasonEnterLiveCount", "display_redraw_reason_enter_live_count_delta"),
    ("displayRedrawReasonLeaveLiveCount", "display_redraw_reason_leave_live_count_delta"),
    ("displayRedrawReasonLeavePersistedCount", "display_redraw_reason_leave_persisted_count_delta"),
    ("displayRedrawReasonForceRedrawCount", "display_redraw_reason_force_redraw_count_delta"),
    ("displayRedrawReasonFrequencyChangeCount", "display_redraw_reason_frequency_change_count_delta"),
    ("displayRedrawReasonBandSetChangeCount", "display_redraw_reason_band_set_change_count_delta"),
    ("displayRedrawReasonArrowChangeCount", "display_redraw_reason_arrow_change_count_delta"),
    ("displayRedrawReasonSignalBarChangeCount", "display_redraw_reason_signal_bar_change_count_delta"),
    ("displayRedrawReasonVolumeChangeCount", "display_redraw_reason_volume_change_count_delta"),
    ("displayRedrawReasonBogeyCounterChangeCount", "display_redraw_reason_bogey_counter_change_count_delta"),
    ("displayRedrawReasonRssiRefreshCount", "display_redraw_reason_rssi_refresh_count_delta"),
    ("displayRedrawReasonFlashTickCount", "display_redraw_reason_flash_tick_count_delta"),
    ("displayFullFlushCount", "display_full_flush_count_delta"),
    ("displayPartialFlushCount", "display_partial_flush_count_delta"),
    ("displayPartialFlushAreaTotalPx", "display_partial_flush_area_total_px_delta"),
    ("displayFlushEquivalentAreaTotalPx", "display_flush_equivalent_area_total_px_delta"),
    ("displayPartialFlushWouldFullRows64Count", "display_partial_flush_would_full_rows64_count_delta"),
    ("displayPartialFlushWouldFullRows128Count", "display_partial_flush_would_full_rows128_count_delta"),
    ("displayPartialFlushWouldFullRows256Count", "display_partial_flush_would_full_rows256_count_delta"),
    ("displayUnionExceedsCapWithFrequencyCount", "display_union_exceeds_cap_with_frequency_count_delta"),
    ("displayUnionExceedsCapWithBandsBarsCount", "display_union_exceeds_cap_with_bands_bars_count_delta"),
    ("displayUnionExceedsCapWithArrowsCount", "display_union_exceeds_cap_with_arrows_count_delta"),
    ("displayUnionExceedsCapWithStatusCount", "display_union_exceeds_cap_with_status_count_delta"),
    ("displayUnionExceedsCapWithIndicatorsCount", "display_union_exceeds_cap_with_indicators_count_delta"),
    ("displayUnionExceedsCapWithExternalCount", "display_union_exceeds_cap_with_external_count_delta"),
    ("displayUnionExceedsCapUnclassifiedCount", "display_union_exceeds_cap_unclassified_count_delta"),
)

DISPLAY_SAMPLE_FIELD_MAPPINGS = (
    ("display_partial_flush_area_peak_px", "displayPartialFlushAreaPeakPx"),
    ("display_flush_max_area_px", "displayFlushMaxAreaPx"),
    ("display_partial_flush_logical_width_peak_px", "displayPartialFlushLogicalWidthPeakPx"),
    ("display_partial_flush_logical_height_peak_px", "displayPartialFlushLogicalHeightPeakPx"),
    ("display_partial_flush_row_calls_peak", "displayPartialFlushRowCallsPeak"),
    ("display_partial_flush_pixels_per_row_peak_px", "displayPartialFlushPixelsPerRowPeakPx"),
    ("display_partial_flush_us_peak_us", "displayPartialFlushUsPeak"),
    ("display_partial_flush_worst_us_logical_width_px", "displayPartialFlushWorstUsLogicalWidthPx"),
    ("display_partial_flush_worst_us_logical_height_px", "displayPartialFlushWorstUsLogicalHeightPx"),
    ("display_partial_flush_worst_us_area_px", "displayPartialFlushWorstUsAreaPx"),
    ("display_union_exceeds_cap_area_peak_px", "displayUnionExceedsCapAreaPeakPx"),
    ("display_union_exceeds_cap_rect_count_peak", "displayUnionExceedsCapRectCountPeak"),
    ("display_union_exceeds_cap_area_peak_source_mask", "displayUnionExceedsCapAreaPeakSourceMask"),
    ("display_base_frame", "displayBaseFrameMaxUs"),
    ("display_status_strip", "displayStatusStripMaxUs"),
    ("display_frequency", "displayFrequencyMaxUs"),
    ("display_bands_bars", "displayBandsBarsMaxUs"),
    ("display_arrows_icons", "displayArrowsIconsMaxUs"),
    ("display_flush_subphase", "displayFlushSubphaseMaxUs"),
    ("display_live_render", "displayLiveRenderMaxUs"),
    ("display_resting_render", "displayRestingRenderMaxUs"),
    ("display_persisted_render", "displayPersistedRenderMaxUs"),
    ("display_preview_render", "displayPreviewRenderMaxUs"),
    ("display_restore_render", "displayRestoreRenderMaxUs"),
    ("display_preview_first_render", "displayPreviewFirstRenderMaxUs"),
    ("display_preview_steady_render", "displayPreviewSteadyRenderMaxUs"),
)

SOAK_TREND_METRIC_NAMES = (
    "metrics_ok_samples",
    "rx_packets_delta",
    "parse_successes_delta",
    "parse_failures_delta",
    "parse_resyncs_delta",
    "queue_drops_delta",
    "perf_drop_delta",
    "oversize_drops_delta",
    "display_drive_activity_delta",
    "display_updates_delta",
    "display_skips_delta",
    "reconnects_delta",
    "disconnects_delta",
    "flush_max_peak_us",
    "loop_max_peak_us",
    "wifi_max_peak_us",
    "ble_drain_max_peak_us",
    "sd_max_peak_us",
    "fs_max_peak_us",
    "queue_high_water_peak",
    "wifi_connect_deferred_delta",
    "dma_free_min_bytes",
    "dma_largest_min_bytes",
    "ble_process_max_peak_us",
    "disp_pipe_max_peak_us",
    "ble_mutex_timeout_delta",
    "wifi_p95_us",
    "disp_pipe_p95_us",
    "dma_fragmentation_pct_p95",
    "samples_to_stable",
    "time_to_stable_ms",
    "connect_burst_samples_to_stable",
    "connect_burst_time_to_stable_ms",
    "connect_burst_pre_ble_process_peak_us",
    "connect_burst_pre_disp_pipe_peak_us",
    "connect_burst_ble_followup_request_alert_peak_us",
    "connect_burst_ble_followup_request_version_peak_us",
    "connect_burst_ble_connect_stable_callback_peak_us",
    "connect_burst_ble_proxy_start_peak_us",
    "connect_burst_disp_render_peak_us",
    "connect_burst_display_base_frame_peak_us",
    "connect_burst_display_status_strip_peak_us",
    "connect_burst_display_frequency_peak_us",
    "connect_burst_display_bands_bars_peak_us",
    "connect_burst_display_arrows_icons_peak_us",
    "connect_burst_display_flush_subphase_peak_us",
    "display_full_flush_count_delta",
    "display_partial_flush_count_delta",
    "display_partial_flush_area_peak_px",
    "display_flush_max_area_px",
    "display_partial_flush_logical_width_peak_px",
    "display_partial_flush_logical_height_peak_px",
    "display_partial_flush_row_calls_peak",
    "display_partial_flush_pixels_per_row_peak_px",
    "display_partial_flush_us_peak_us",
    "display_partial_flush_worst_us_logical_width_px",
    "display_partial_flush_worst_us_logical_height_px",
    "display_partial_flush_worst_us_area_px",
    "display_partial_flush_would_full_rows64_count_delta",
    "display_partial_flush_would_full_rows128_count_delta",
    "display_partial_flush_would_full_rows256_count_delta",
    "display_resting_flush_reason_full_redraw_count_delta",
    "display_resting_flush_reason_pending_external_count_delta",
    "display_resting_flush_reason_painted_count_delta",
    "display_resting_flush_reason_cache_hit_count_delta",
    "display_persisted_flush_reason_full_redraw_count_delta",
    "display_persisted_flush_reason_pending_external_count_delta",
    "display_persisted_flush_reason_painted_count_delta",
    "display_persisted_flush_reason_cache_hit_count_delta",
    "display_status_volume_paint_count_delta",
    "display_status_rssi_paint_count_delta",
    "display_status_profile_paint_count_delta",
    "display_status_battery_paint_count_delta",
    "display_status_ble_proxy_paint_count_delta",
    "display_status_wifi_paint_count_delta",
    "display_status_obd_paint_count_delta",
    "display_status_gps_paint_count_delta",
    "display_status_alp_paint_count_delta",
    "display_base_frame_peak_us",
    "display_status_strip_peak_us",
    "display_frequency_peak_us",
    "display_bands_bars_peak_us",
    "display_arrows_icons_peak_us",
    "display_flush_subphase_peak_us",
    "display_live_render_peak_us",
    "display_resting_render_peak_us",
    "display_persisted_render_peak_us",
    "display_preview_render_peak_us",
    "display_restore_render_peak_us",
    "display_preview_first_render_peak_us",
    "display_preview_steady_render_peak_us",
    "notify_to_display_pipeline_complete_max_ms",
    "notify_to_display_pipeline_complete_sample_count",
)

SOAK_TREND_METRIC_UNITS = {name: CANONICAL_METRIC_UNITS[name] for name in SOAK_TREND_METRIC_NAMES}

SOAK_TREND_METRIC_KV_ALIASES = {
    "metrics_ok_samples": "ok_samples",
    "loop_max_peak_us": "loop_max_peak",
    "flush_max_peak_us": "flush_max_peak",
    "wifi_max_peak_us": "wifi_max_peak",
    "ble_drain_max_peak_us": "ble_drain_max_peak",
    "sd_max_peak_us": "sd_max_peak",
    "fs_max_peak_us": "fs_max_peak",
    "dma_free_min_bytes": "dma_free_min",
    "dma_largest_min_bytes": "dma_largest_min",
    "ble_process_max_peak_us": "ble_process_max_peak",
    "disp_pipe_max_peak_us": "disp_pipe_max_peak",
    "disp_pipe_p95_us": "disp_pipe_p95",
    "connect_burst_pre_ble_process_peak_us": "connect_burst_pre_ble_process_peak",
    "connect_burst_pre_disp_pipe_peak_us": "connect_burst_pre_disp_pipe_peak",
    "connect_burst_ble_followup_request_alert_peak_us": "connect_burst_ble_followup_request_alert_peak",
    "connect_burst_ble_followup_request_version_peak_us": "connect_burst_ble_followup_request_version_peak",
    "connect_burst_ble_connect_stable_callback_peak_us": "connect_burst_ble_connect_stable_callback_peak",
    "connect_burst_ble_proxy_start_peak_us": "connect_burst_ble_proxy_start_peak",
    "connect_burst_disp_render_peak_us": "connect_burst_disp_render_peak",
    "connect_burst_display_base_frame_peak_us": "connect_burst_display_base_frame_peak",
    "connect_burst_display_status_strip_peak_us": "connect_burst_display_status_strip_peak",
    "connect_burst_display_frequency_peak_us": "connect_burst_display_frequency_peak",
    "connect_burst_display_bands_bars_peak_us": "connect_burst_display_bands_bars_peak",
    "connect_burst_display_arrows_icons_peak_us": "connect_burst_display_arrows_icons_peak",
    "connect_burst_display_flush_subphase_peak_us": "connect_burst_display_flush_subphase_peak",
    "display_base_frame_peak_us": "display_base_frame_peak",
    "display_status_strip_peak_us": "display_status_strip_peak",
    "display_frequency_peak_us": "display_frequency_peak",
    "display_bands_bars_peak_us": "display_bands_bars_peak",
    "display_arrows_icons_peak_us": "display_arrows_icons_peak",
    "display_flush_subphase_peak_us": "display_flush_subphase_peak",
    "display_live_render_peak_us": "display_live_render_peak",
    "display_resting_render_peak_us": "display_resting_render_peak",
    "display_persisted_render_peak_us": "display_persisted_render_peak",
    "display_preview_render_peak_us": "display_preview_render_peak",
    "display_restore_render_peak_us": "display_restore_render_peak",
    "display_preview_first_render_peak_us": "display_preview_first_render_peak",
    "display_preview_steady_render_peak_us": "display_preview_steady_render_peak",
}


def metric_unit(metric_name: str) -> str:
    return CANONICAL_METRIC_UNITS[metric_name]


def unsupported_metrics_for_perf_csv(source_schema: int, columns: Iterable[str]) -> set[str]:
    column_set = set(columns)
    unsupported = set(PERF_CSV_ALWAYS_UNSUPPORTED_METRICS)
    schema_is_legacy = source_schema != 0 and source_schema < MIN_DROP_COUNTER_SCHEMA
    if schema_is_legacy or "perfDrop" not in column_set:
        unsupported.update(PERF_CSV_LEGACY_UNSUPPORTED_METRICS)
    notify_schema_is_legacy = source_schema != 0 and source_schema < MIN_NOTIFY_PIPELINE_COMPLETE_SCHEMA
    if notify_schema_is_legacy or not PERF_CSV_NOTIFY_PIPELINE_COMPLETE_COLUMNS <= column_set:
        unsupported.update(PERF_CSV_NOTIFY_PIPELINE_COMPLETE_METRICS)
    return unsupported


def coverage_status_for_unsupported_metrics(unsupported_metrics: Iterable[str]) -> str:
    unsupported = set(unsupported_metrics)
    if PERF_CSV_LEGACY_UNSUPPORTED_METRICS & unsupported:
        return "partial_legacy_import"
    if unsupported:
        return "full_runtime_gates"
    return "full"


def kv_source_key(metric_name: str) -> str:
    return SOAK_TREND_METRIC_KV_ALIASES.get(metric_name, metric_name)
