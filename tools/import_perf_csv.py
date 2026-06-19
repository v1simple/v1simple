#!/usr/bin/env python3
"""Import a perf CSV capture into the canonical hardware scoring pipeline."""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

import import_drive_log  # type: ignore
import score_hardware_run  # type: ignore
import score_perf_csv  # type: ignore
from hardware_report_utils import write_comparison_text, write_comparison_tsv  # type: ignore
from metric_derivation import percentile  # type: ignore
from metric_schema import (  # type: ignore
    CANONICAL_METRIC_UNITS,
    CSV_CONNECT_BURST_PEAK_COLUMNS,
    CSV_DELTA_COLUMNS,
    CSV_PEAK_DIAGNOSTIC_COLUMNS,
    CSV_PEAK_ONLY_COLUMNS,
    DISPLAY_COUNTER_DELTA_MAPPINGS,
    coverage_status_for_unsupported_metrics,
    metric_unit,
    unsupported_metrics_for_perf_csv,
)


ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"
SLO_FILE = ROOT / "tools" / "perf_slo_thresholds.json"
DEFAULT_HEADER_COLUMNS = [
    line.strip()
    for line in (ROOT / "test" / "contracts" / "perf_csv_column_contract.txt").read_text(encoding="utf-8").splitlines()
    if line.strip() and not line.startswith("#")
]
TOP_ROW_FIELDS = (
    "disc",
    "reconn",
    "rx",
    "parseOK",
    "displayUpdates",
)
ATTRIBUTION_COLUMNS = (
    "bleState",
    "subscribeStep",
    "connectInProgress",
    "asyncConnectPending",
    "pendingDisconnectCleanup",
    "proxyAdvertising",
    "proxyAdvertisingLastTransitionReason",
    "wifiPriorityMode",
    "dispMax_us",
    "bleFollowupRequestAlertMax_us",
    "bleFollowupRequestVersionMax_us",
    "bleConnectStableCallbackMax_us",
    "bleProxyStartMax_us",
    "displayGapRecoverMax_us",
    "displayFullRenderCount",
    "displayRestingFullRenderCount",
    "displayRestingIncrementalRenderCount",
    "displayPersistedRenderCount",
    "displayPreviewRenderCount",
    "displayRestoreRenderCount",
    "displayLiveScenarioRenderCount",
    "displayRestingScenarioRenderCount",
    "displayPersistedScenarioRenderCount",
    "displayPreviewScenarioRenderCount",
    "displayRestoreScenarioRenderCount",
    "displayRestingFlushReasonFullRedrawCount",
    "displayRestingFlushReasonPendingExternalCount",
    "displayRestingFlushReasonPaintedCount",
    "displayRestingFlushReasonCacheHitCount",
    "displayPersistedFlushReasonFullRedrawCount",
    "displayPersistedFlushReasonPendingExternalCount",
    "displayPersistedFlushReasonPaintedCount",
    "displayPersistedFlushReasonCacheHitCount",
    "displayStatusVolumePaintCount",
    "displayStatusRssiPaintCount",
    "displayStatusProfilePaintCount",
    "displayStatusBatteryPaintCount",
    "displayStatusBleProxyPaintCount",
    "displayStatusWifiPaintCount",
    "displayStatusObdPaintCount",
    "displayStatusGpsPaintCount",
    "displayStatusAlpPaintCount",
    "displayRedrawReasonFirstRunCount",
    "displayRedrawReasonEnterLiveCount",
    "displayRedrawReasonLeaveLiveCount",
    "displayRedrawReasonLeavePersistedCount",
    "displayRedrawReasonForceRedrawCount",
    "displayRedrawReasonFrequencyChangeCount",
    "displayRedrawReasonBandSetChangeCount",
    "displayRedrawReasonArrowChangeCount",
    "displayRedrawReasonSignalBarChangeCount",
    "displayRedrawReasonVolumeChangeCount",
    "displayRedrawReasonBogeyCounterChangeCount",
    "displayRedrawReasonRssiRefreshCount",
    "displayRedrawReasonFlashTickCount",
    "displayFullFlushCount",
    "displayPartialFlushCount",
    "displayPartialFlushAreaPeakPx",
    "displayPartialFlushAreaTotalPx",
    "displayFlushEquivalentAreaTotalPx",
    "displayFlushMaxAreaPx",
    "displayPartialFlushLogicalWidthPeakPx",
    "displayPartialFlushLogicalHeightPeakPx",
    "displayPartialFlushRowCallsPeak",
    "displayPartialFlushPixelsPerRowPeakPx",
    "displayPartialFlushUsPeak_us",
    "displayPartialFlushWorstUsLogicalWidthPx",
    "displayPartialFlushWorstUsLogicalHeightPx",
    "displayPartialFlushWorstUsAreaPx",
    "displayPartialFlushWouldFullRows64Count",
    "displayPartialFlushWouldFullRows128Count",
    "displayPartialFlushWouldFullRows256Count",
    "displayUnionExceedsCapAreaPeakPx",
    "displayUnionExceedsCapRectCountPeak",
    "displayUnionExceedsCapAreaPeakSourceMask",
    "displayUnionExceedsCapWithFrequencyCount",
    "displayUnionExceedsCapWithBandsBarsCount",
    "displayUnionExceedsCapWithArrowsCount",
    "displayUnionExceedsCapWithStatusCount",
    "displayUnionExceedsCapWithIndicatorsCount",
    "displayUnionExceedsCapWithExternalCount",
    "displayUnionExceedsCapUnclassifiedCount",
    "displayBaseFrameMax_us",
    "displayStatusStripMax_us",
    "displayFrequencyMax_us",
    "displayBandsBarsMax_us",
    "displayArrowsIconsMax_us",
    "displayFlushSubphaseMax_us",
    "displayLiveRenderMax_us",
    "displayRestingRenderMax_us",
    "displayPersistedRenderMax_us",
    "displayPreviewRenderMax_us",
    "displayRestoreRenderMax_us",
    "displayPreviewFirstRenderMax_us",
    "displayPreviewSteadyRenderMax_us",
    "obdMax_us",
    "obdConnectCallMax_us",
    "obdSecurityStartCallMax_us",
    "obdDiscoveryCallMax_us",
    "obdSubscribeCallMax_us",
    "obdWriteCallMax_us",
    "obdRssiCallMax_us",
    "fsMax_us",
    "wifiHandleClientMax_us",
    "wifiMaintenanceMax_us",
    "wifiStatusCheckMax_us",
    "wifiTimeoutCheckMax_us",
    "wifiHeapGuardMax_us",
    "wifiApStaPollMax_us",
)
OBD_SYNC_CALL_COLUMNS = (
    ("obdConnectCallMax_us", "connect"),
    ("obdSecurityStartCallMax_us", "security start"),
    ("obdDiscoveryCallMax_us", "service discovery"),
    ("obdSubscribeCallMax_us", "notification subscribe"),
    ("obdWriteCallMax_us", "command write"),
    ("obdRssiCallMax_us", "RSSI read"),
)
WIFI_SUBPHASE_COLUMNS = (
    ("fsMax_us", "LittleFS static file serving"),
    ("wifiHandleClientMax_us", "HTTP client handling"),
    ("wifiMaintenanceMax_us", "WiFi maintenance"),
    ("wifiStatusCheckMax_us", "WiFi status check"),
    ("wifiTimeoutCheckMax_us", "WiFi timeout check"),
    ("wifiHeapGuardMax_us", "WiFi heap guard"),
    ("wifiApStaPollMax_us", "AP station poll"),
    ("wifiStopHttpServerMax_us", "HTTP server stop"),
    ("wifiStopStaDisconnectMax_us", "STA disconnect"),
    ("wifiStopApDisableMax_us", "AP disable"),
    ("wifiStopModeOffMax_us", "radio off"),
    ("wifiStartPreflightMax_us", "WiFi start preflight"),
    ("wifiStartApBringupMax_us", "AP bring-up"),
)
CONNECT_BURST_BLE_COLUMNS = (
    ("bleFollowupRequestAlertMax_us", "followup alert request"),
    ("bleFollowupRequestVersionMax_us", "followup version request"),
    ("bleConnectStableCallbackMax_us", "stable-connect callback"),
    ("bleProxyStartMax_us", "proxy advertising start"),
)
CONNECT_BURST_DISPLAY_COLUMNS = (
    ("dispMax_us", "display render"),
    ("displayGapRecoverMax_us", "display gap recovery"),
    ("displayBaseFrameMax_us", "display base frame"),
    ("displayStatusStripMax_us", "display status strip"),
    ("displayFrequencyMax_us", "display frequency"),
    ("displayBandsBarsMax_us", "display bands and bars"),
    ("displayArrowsIconsMax_us", "display arrows and icons"),
    ("displayFlushSubphaseMax_us", "display flush"),
)
BOUNDARY_EVENT_WINDOW_ROWS = 2
MIN_BOUNDARY_PADDING_ROWS = 3
SD_START_WINDOW_MS = 10_000
CONNECT_BURST_STABLE_CONSECUTIVE_SAMPLES = 3
BLE_STATE_NAMES = {
    0: "DISCONNECTED",
    1: "SCANNING",
    2: "SCAN_STOPPING",
    3: "CONNECTING",
    4: "CONNECTING_WAIT",
    5: "DISCOVERING",
    6: "SUBSCRIBING",
    7: "SUBSCRIBE_YIELD",
    8: "CONNECTED",
    9: "BACKOFF",
}
SUBSCRIBE_STEP_NAMES = {
    0: "GET_SERVICE",
    1: "GET_DISPLAY_CHAR",
    2: "GET_COMMAND_CHAR",
    3: "GET_COMMAND_LONG",
    4: "SUBSCRIBE_DISPLAY",
    5: "WRITE_DISPLAY_CCCD",
    6: "GET_DISPLAY_LONG",
    7: "SUBSCRIBE_LONG",
    8: "WRITE_LONG_CCCD",
    9: "REQUEST_ALERT_DATA",
    10: "REQUEST_VERSION",
    11: "COMPLETE",
}
CONNECT_PHASE_STATES = {"SCANNING", "SCAN_STOPPING", "CONNECTING", "CONNECTING_WAIT", "DISCOVERING", "SUBSCRIBING", "SUBSCRIBE_YIELD"}
DISCONNECT_PHASE_STATES = {"DISCONNECTED", "BACKOFF"}
DIRECT_SPEED_COLUMN_CANDIDATES = (
    ("speedSelectedMph_x10", "speedSourceValid"),
    ("obdSpeedMph_x10", None),
)


@dataclass(frozen=True)
class SessionSummary:
    session_index: int
    token: str
    schema: int
    row_count: int
    duration_ms: int
    duration_s: float
    rx_delta: int
    speed_active_rows: int
    speed_active_rows_supported: bool
    speed_active_column: Optional[str]
    connected: bool
    drive_like: bool
    has_marker: bool

    def to_dict(self) -> dict[str, Any]:
        return {
            "session_index": self.session_index,
            "token": self.token,
            "schema": self.schema,
            "row_count": self.row_count,
            "duration_ms": self.duration_ms,
            "duration_s": self.duration_s,
            "rx_delta": self.rx_delta,
            "speed_active_rows": self.speed_active_rows,
            "speed_active_rows_supported": self.speed_active_rows_supported,
            "speed_active_column": self.speed_active_column,
            "connected": self.connected,
            "drive_like": self.drive_like,
            "has_marker": self.has_marker,
        }


def _decode_ble_state(code: Any) -> str:
    try:
        return BLE_STATE_NAMES.get(int(code), f"UNKNOWN_{int(code)}")
    except (TypeError, ValueError):
        return "UNKNOWN"


def _decode_subscribe_step(code: Any) -> str:
    try:
        return SUBSCRIBE_STEP_NAMES.get(int(code), f"UNKNOWN_{int(code)}")
    except (TypeError, ValueError):
        return "UNKNOWN"


def _segment_position(row_index: int, row_count: int) -> str:
    if row_count <= 0:
        return "unknown"
    threshold = max(3, int(math.ceil(row_count * 0.1)))
    if row_index <= threshold:
        return "start"
    if row_index > max(0, row_count - threshold):
        return "end"
    return "mid-drive"


def peak_limit_map(profile: str) -> dict[str, float]:
    config = score_perf_csv.load_threshold_config(SLO_FILE)
    checks = config.hard_common + config.hard_profile.get(profile, [])
    limits: dict[str, float] = {}
    for metric, _source, op, limit in checks:
        if op == "<=":
            limits[metric] = float(limit)
    return limits


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Path to perf capture CSV")
    parser.add_argument("--out-dir", default="", help="Output directory for manifest/scoring artifacts")
    parser.add_argument(
        "--profile",
        default="drive_wifi_off",
        help="SLO profile: drive_wifi_off (default) or drive_wifi_ap",
    )
    parser.add_argument(
        "--segment",
        default="",
        help="Drive segment selector: auto (default), last, longest-connected, or 1-based index",
    )
    parser.add_argument(
        "--session",
        default="",
        help="Compatibility alias for --segment",
    )
    parser.add_argument("--list-segments", action="store_true", help="List discovered CSV segments and exit")
    parser.add_argument(
        "--compare-to",
        action="append",
        default=[],
        help="Optional baseline manifest.json (repeat for a baseline window)",
    )
    parser.add_argument("--board-id", default="", help="Override board_id")
    parser.add_argument("--git-sha", default="", help="Override git_sha")
    parser.add_argument("--git-ref", default="", help="Override git_ref")
    parser.add_argument("--stress-class", default="core", help="Stress class (default: core)")
    parser.add_argument("--lane", default="perf-csv-import", help="Lane tag")
    parser.add_argument(
        "--mode-coverage-json",
        default="",
        help="Optional mode coverage JSON from the SD/serial runner; used to keep lab no-activity runs from failing physical-coverage metrics.",
    )
    args = parser.parse_args()
    if not args.list_segments and not args.out_dir:
        parser.error("--out-dir is required unless --list-segments is used")
    return args


def _selector_arg(args: argparse.Namespace) -> str:
    return (args.segment or args.session or "auto").strip() or "auto"


def load_sessions(path: Path) -> list[tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]]]]:
    sessions: list[tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]]]] = []
    current_fields: Optional[list[str]] = None
    current_meta: Optional[score_perf_csv.SessionMeta] = None
    current_rows: list[dict[str, int]] = []
    pending_rows: list[str] = []

    def parse_rows(raw_rows: list[str], fields: list[str]) -> list[dict[str, int]]:
        parsed_rows: list[dict[str, int]] = []
        for raw_line in raw_rows:
            if not raw_line or raw_line.startswith("#"):
                continue
            values = raw_line.split(",")
            millis_val = values[0].strip() if values else ""
            if not millis_val or millis_val.startswith("#"):
                continue
            row: dict[str, int] = {}
            for index, key in enumerate(fields):
                raw_value = values[index].strip() if index < len(values) else "0"
                row[key] = score_perf_csv.parse_int(raw_value or "0")
            parsed_rows.append(row)
        return parsed_rows

    with path.open("r", newline="", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue

            if line.startswith("millis,"):
                fields = line.split(",")
                if current_fields is None and pending_rows:
                    leading_rows = parse_rows(pending_rows, fields)
                    if leading_rows:
                        sessions.append((None, leading_rows))
                    pending_rows = []
                elif current_fields is not None and current_rows:
                    sessions.append((current_meta, current_rows))
                current_fields = fields
                current_meta = None
                current_rows = []
                continue

            if line.startswith("#session_start"):
                if current_fields is None:
                    pending_rows.append(line)
                else:
                    current_meta = score_perf_csv._parse_session_meta(line)
                continue

            if current_fields is None:
                pending_rows.append(line)
                continue

            current_rows.extend(parse_rows([line], current_fields))

    if current_fields is None and pending_rows:
        header_fields = list(DEFAULT_HEADER_COLUMNS)
        trailing_rows = parse_rows(pending_rows, header_fields)
        if trailing_rows:
            sessions.append((None, trailing_rows))
    elif current_fields is not None and current_rows:
        sessions.append((current_meta, current_rows))

    return sessions


def _duration_ms(rows: list[dict[str, int]]) -> int:
    if not rows:
        return 0
    return max(0, int(rows[-1].get("millis", 0)) - int(rows[0].get("millis", 0)))


def _rx_delta(rows: list[dict[str, int]]) -> int:
    if not rows:
        return 0
    return int(rows[-1].get("rx", 0)) - int(rows[0].get("rx", 0))


def _direct_speed_column(rows: list[dict[str, int]]) -> tuple[Optional[str], Optional[str]]:
    if not rows:
        return None, None
    for column, valid_column in DIRECT_SPEED_COLUMN_CANDIDATES:
        if column in rows[0]:
            if valid_column is None or valid_column in rows[0]:
                return column, valid_column
    return None, None


def summarize_sessions(
    sessions: list[tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]]]]
) -> list[SessionSummary]:
    summaries: list[SessionSummary] = []
    for index, (meta, rows) in enumerate(sessions, start=1):
        rx_delta = _rx_delta(rows)
        speed_active_column, speed_valid_column = _direct_speed_column(rows)
        speed_active_rows_supported = speed_active_column is not None
        speed_active_rows = (
            sum(
                1
                for row in rows
                if int(row.get(speed_active_column, 0)) > 0
                and (speed_valid_column is None or int(row.get(speed_valid_column, 0)) == 1)
            )
            if speed_active_column is not None
            else 0
        )
        connected = bool(rows) and (rx_delta > 0 or int(rows[-1].get("rx", 0)) > 0)
        summaries.append(
            SessionSummary(
                session_index=index,
                token=meta.token if meta and meta.token else "",
                schema=int(meta.schema) if meta else 0,
                row_count=len(rows),
                duration_ms=_duration_ms(rows),
                duration_s=score_perf_csv.duration_s(rows) if rows else 0.0,
                rx_delta=rx_delta,
                speed_active_rows=speed_active_rows,
                speed_active_rows_supported=speed_active_rows_supported,
                speed_active_column=speed_active_column,
                connected=connected,
                drive_like=(speed_active_rows_supported and speed_active_rows > 0),
                has_marker=meta is not None,
            )
        )
    return summaries


def select_segment(
    sessions: list[tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]]]],
    selector: str,
) -> tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]], SessionSummary, list[SessionSummary], str]:
    summaries = summarize_sessions(sessions)
    if not summaries:
        raise RuntimeError("No sessions found in CSV")

    effective_selector = selector
    if selector == "auto":
        drive_candidates = [summary for summary in summaries if summary.connected and summary.drive_like]
        if drive_candidates:
            chosen = max(
                drive_candidates,
                key=lambda item: (
                    item.speed_active_rows,
                    item.rx_delta,
                    item.duration_ms,
                    item.session_index,
                ),
            )
        else:
            try:
                _meta, _rows, idx = score_perf_csv.select_session(sessions, "longest-connected")
            except RuntimeError:
                _meta, _rows, idx = score_perf_csv.select_session(sessions, "longest")
            chosen = summaries[idx - 1]
        effective_selector = f"auto->{chosen.session_index}"
        return sessions[chosen.session_index - 1][0], sessions[chosen.session_index - 1][1], chosen, summaries, effective_selector

    meta, rows, session_index = score_perf_csv.select_session(sessions, selector)
    return meta, rows, summaries[session_index - 1], summaries, selector


def render_segment_listing(
    summaries: list[SessionSummary], selected_index: int, selector: str, csv_path: Path
) -> str:
    rows = [
        [
            "*" if summary.session_index == selected_index else "",
            str(summary.session_index),
            summary.token or "n/a",
            str(summary.schema or 0),
            str(summary.row_count),
            f"{summary.duration_s:.1f}",
            str(summary.rx_delta),
            str(summary.speed_active_rows) if summary.speed_active_rows_supported else "n/a",
            "yes" if summary.drive_like else ("no" if summary.speed_active_rows_supported else "n/a"),
        ]
        for summary in summaries
    ]
    widths = [len(label) for label in ["SEL", "SEG", "TOKEN", "SCHEMA", "ROWS", "DUR_S", "RX_DELTA", "SPEED_ROWS", "DRIVE"]]
    for row in rows:
        for idx, value in enumerate(row):
            widths[idx] = max(widths[idx], len(value))

    def fmt(values: list[str]) -> str:
        return "  ".join(value.ljust(widths[idx]) for idx, value in enumerate(values))

    lines = [
        f"source: {csv_path}",
        f"selector: {selector}",
        "",
        fmt(["SEL", "SEG", "TOKEN", "SCHEMA", "ROWS", "DUR_S", "RX_DELTA", "SPEED_ROWS", "DRIVE"]),
        fmt(["-" * widths[0], "-" * widths[1], "-" * widths[2], "-" * widths[3], "-" * widths[4], "-" * widths[5], "-" * widths[6], "-" * widths[7], "-" * widths[8]]),
    ]
    lines.extend(fmt(row) for row in rows)
    return "\n".join(lines) + "\n"


def _has_column(rows: list[dict[str, int]], column: str) -> bool:
    return bool(rows) and column in rows[0]


def _row_is_connect_burst_event(row: dict[str, int]) -> bool:
    return _decode_ble_state(row.get("bleState", 0)) == "CONNECTED" and _decode_subscribe_step(
        row.get("subscribeStep", 0)
    ) == "COMPLETE"


def _connect_burst_row_window(rows: list[dict[str, int]]) -> tuple[int, int] | None:
    """Return the first-connected burst window as [start, end).

    SD CSV imports do not have the richer JSONL threshold arguments used by
    ``soak_parse_metrics.py``. The firmware CSV already records the relevant
    per-window peak columns, so the importer anchors the window at the first
    CONNECTED/COMPLETE row and keeps the same default three-sample settle
    horizon used by the JSONL soak parser. That is enough to stop the hardware
    catalog from treating these available columns as missing while preserving
    the intended "first connected burst" scope.
    """
    for index, row in enumerate(rows):
        if _row_is_connect_burst_event(row):
            end = min(len(rows), index + CONNECT_BURST_STABLE_CONSECUTIVE_SAMPLES)
            return index, max(index + 1, end)
    return None


def _connect_burst_time_to_stable_ms(rows: list[dict[str, int]], start: int, end: int) -> float | None:
    if end <= start or not _has_column(rows, "millis"):
        return None
    return float(max(0, int(rows[end - 1].get("millis", 0)) - int(rows[start].get("millis", 0))))


def _delta_metric(rows: list[dict[str, int]], column: str) -> Optional[float]:
    if not _has_column(rows, column):
        return None
    return float(int(rows[-1].get(column, 0)) - int(rows[0].get(column, 0)))


def _peak_metric(rows: list[dict[str, int]], column: str) -> Optional[float]:
    if not _has_column(rows, column):
        return None
    return float(max(int(row.get(column, 0)) for row in rows))


def _floor_metric(rows: list[dict[str, int]], column: str) -> Optional[float]:
    if not _has_column(rows, column):
        return None
    return float(min(int(row.get(column, 0)) for row in rows))


def _peak_diagnostic(rows: list[dict[str, int]], column: str) -> Optional[dict[str, Any]]:
    if not _has_column(rows, column):
        return None
    peak_index, peak_row = max(enumerate(rows), key=lambda item: int(item[1].get(column, 0)))
    return {
        "column": column,
        "value": float(int(peak_row.get(column, 0))),
        "row_index": peak_index + 1,
        "millis": int(peak_row.get("millis", 0)),
    }


def _sd_window_peak(indexed_rows: list[tuple[int, dict[str, int]]], segment_first_millis: int) -> Optional[dict[str, Any]]:
    if not indexed_rows:
        return None
    original_index, peak_row = max(indexed_rows, key=lambda item: int(item[1].get("sdMax_us", 0)))
    millis = int(peak_row.get("millis", 0))
    return {
        "value": float(int(peak_row.get("sdMax_us", 0))),
        "row_index": original_index + 1,
        "millis": millis,
        "relative_ms": max(0, millis - segment_first_millis),
    }


def build_sd_latency_split(rows: list[dict[str, int]]) -> dict[str, Any]:
    """Split SD peaks into a fixed session-start window and steady runtime.

    sdMax_us records the largest SD writer cost observed in the previous
    snapshot window. A fixed window, relative to the selected CSV segment's
    first row, keeps bench runs comparable while preserving
    the real session-start cost instead of hiding it inside the steady runtime
    score.
    """
    if not rows or not _has_column(rows, "sdMax_us"):
        return {
            "available": False,
            "start_window_ms": SD_START_WINDOW_MS,
            "start_row_count": 0,
            "runtime_row_count": 0,
            "start_peak": None,
            "runtime_peak": None,
        }

    first_millis = int(rows[0].get("millis", 0))
    start_rows: list[tuple[int, dict[str, int]]] = []
    runtime_rows: list[tuple[int, dict[str, int]]] = []
    for index, row in enumerate(rows):
        relative_ms = max(0, int(row.get("millis", 0)) - first_millis)
        if relative_ms < SD_START_WINDOW_MS:
            start_rows.append((index, row))
        else:
            runtime_rows.append((index, row))

    return {
        "available": True,
        "start_window_ms": SD_START_WINDOW_MS,
        "runtime_starts_after_ms": SD_START_WINDOW_MS,
        "start_row_count": len(start_rows),
        "runtime_row_count": len(runtime_rows),
        "start_peak": _sd_window_peak(start_rows, first_millis),
        "runtime_peak": _sd_window_peak(runtime_rows, first_millis),
    }


def _build_row_summary(row: dict[str, int], row_index: int, column: str) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "row_index": row_index,
        "millis": int(row.get("millis", 0)),
        "value": float(int(row.get(column, 0))),
    }
    for field in TOP_ROW_FIELDS:
        if field in row:
            summary[field] = int(row.get(field, 0))
    for field in ("loopMax_us", "obdMax_us", "wifiMax_us", "fsMax_us", "bleProcessMax_us", "dispPipeMax_us"):
        if field in row:
            summary[field] = int(row.get(field, 0))
    for field, _label in CONNECT_BURST_BLE_COLUMNS:
        if field in row:
            summary[field] = int(row.get(field, 0))
    for field, _label in CONNECT_BURST_DISPLAY_COLUMNS:
        if field in row:
            summary[field] = int(row.get(field, 0))
    if "bleState" in row:
        summary["bleState"] = _decode_ble_state(row.get("bleState", 0))
        summary["bleStateCode"] = int(row.get("bleState", 0))
    if "subscribeStep" in row:
        summary["subscribeStep"] = _decode_subscribe_step(row.get("subscribeStep", 0))
        summary["subscribeStepCode"] = int(row.get("subscribeStep", 0))
    if "proxyAdvertisingLastTransitionReason" in row:
        summary["proxyAdvertisingLastTransitionReasonCode"] = int(row.get("proxyAdvertisingLastTransitionReason", 0))
    for field in ("connectInProgress", "asyncConnectPending", "pendingDisconnectCleanup", "proxyAdvertising", "wifiPriorityMode"):
        if field in row:
            summary[field] = bool(int(row.get(field, 0)))
    for field, _label in OBD_SYNC_CALL_COLUMNS:
        if field in row:
            summary[field] = int(row.get(field, 0))
    for field, _label in WIFI_SUBPHASE_COLUMNS:
        if field in row:
            summary[field] = int(row.get(field, 0))
    return summary


def _wrapped_metric(value: float, wrapped_value: float, *, min_slack: float = 5000.0) -> bool:
    if value <= 0 or wrapped_value <= 0:
        return False
    return abs(value - wrapped_value) <= max(min_slack, wrapped_value * 0.2)


def _dominant_named_metric(summary: dict[str, Any], columns: tuple[tuple[str, str], ...]) -> Optional[dict[str, Any]]:
    best: Optional[tuple[str, str, int]] = None
    for column, label in columns:
        try:
            value = int(summary.get(column, 0))
        except (TypeError, ValueError):
            value = 0
        if value <= 0:
            continue
        if best is None or value > best[2]:
            best = (column, label, value)
    if best is None:
        return None
    return {
        "column": best[0],
        "label": best[1],
        "value": best[2],
    }


def _is_obd_wrapped_loop(summary: dict[str, Any]) -> bool:
    try:
        return _wrapped_metric(float(summary.get("value", 0)), float(summary.get("obdMax_us", 0)), min_slack=50000.0)
    except (TypeError, ValueError):
        return False


def _is_fs_wrapped_wifi(summary: dict[str, Any]) -> bool:
    try:
        return _wrapped_metric(float(summary.get("value", 0)), float(summary.get("fsMax_us", 0)))
    except (TypeError, ValueError):
        return False


def _longest_exceed_run(rows: list[dict[str, int]], column: str, limit: float) -> int:
    longest = 0
    current = 0
    for row in rows:
        if int(row.get(column, 0)) > limit:
            current += 1
            longest = max(longest, current)
        else:
            current = 0
    return longest


def _boundary_padding_rows(row_count: int) -> int:
    if row_count <= 0:
        return 0
    return min(row_count, max(MIN_BOUNDARY_PADDING_ROWS, int(math.ceil(row_count * 0.05))))


def _counter_delta_indexes(rows: list[dict[str, int]], column: str) -> set[int]:
    indexes: set[int] = set()
    if not rows or not _has_column(rows, column):
        return indexes
    previous = int(rows[0].get(column, 0))
    for index in range(1, len(rows)):
        current = int(rows[index].get(column, 0))
        if current > previous:
            start = max(0, index - BOUNDARY_EVENT_WINDOW_ROWS)
            end = min(len(rows), index + BOUNDARY_EVENT_WINDOW_ROWS + 1)
            indexes.update(range(start, end))
        previous = current
    return indexes


def _row_is_explicit_boundary(row: dict[str, int]) -> bool:
    ble_state = _decode_ble_state(row.get("bleState", 0)) if "bleState" in row else ""
    if ble_state in CONNECT_PHASE_STATES or ble_state in DISCONNECT_PHASE_STATES:
        return True
    if int(row.get("connectInProgress", 0)) == 1:
        return True
    if int(row.get("asyncConnectPending", 0)) == 1:
        return True
    if int(row.get("pendingDisconnectCleanup", 0)) == 1:
        return True
    return False


def _boundary_row_indexes(rows: list[dict[str, int]]) -> set[int]:
    indexes: set[int] = set()
    if not rows:
        return indexes
    padding = _boundary_padding_rows(len(rows))
    indexes.update(range(min(padding, len(rows))))
    indexes.update(range(max(0, len(rows) - padding), len(rows)))
    for index, row in enumerate(rows):
        if _row_is_explicit_boundary(row):
            indexes.add(index)
    indexes.update(_counter_delta_indexes(rows, "disc"))
    indexes.update(_counter_delta_indexes(rows, "reconn"))
    return indexes


def _rows_from_indexes(rows: list[dict[str, int]], indexes: set[int], *, include: bool) -> list[dict[str, int]]:
    if include:
        return [row for index, row in enumerate(rows) if index in indexes]
    return [row for index, row in enumerate(rows) if index not in indexes]


def _subset_peak_diagnostic(rows: list[dict[str, int]], column: str, limit: float) -> Optional[dict[str, Any]]:
    if not rows or not _has_column(rows, column):
        return None
    peak_index, peak_row = max(enumerate(rows), key=lambda item: int(item[1].get(column, 0)))
    exceed_count = sum(1 for row in rows if int(row.get(column, 0)) > limit)
    longest_exceed_run = _longest_exceed_run(rows, column, limit)
    classification = (
        "sustained"
        if longest_exceed_run > 3 or (len(rows) > 0 and exceed_count > (len(rows) * 0.05))
        else ("spike" if exceed_count > 0 else "clean")
    )
    return {
        "value": float(int(peak_row.get(column, 0))),
        "row_index": peak_index + 1,
        "millis": int(peak_row.get("millis", 0)),
        "exceed_count": exceed_count,
        "longest_exceed_run": longest_exceed_run,
        "classification": classification,
    }


def build_peak_partition_analysis(
    rows: list[dict[str, int]],
    peak_diagnostics: dict[str, dict[str, Any]],
    profile: str,
) -> dict[str, Any]:
    boundary_indexes = _boundary_row_indexes(rows)
    boundary_rows = _rows_from_indexes(rows, boundary_indexes, include=True)
    steady_rows = _rows_from_indexes(rows, boundary_indexes, include=False)
    limits = peak_limit_map(profile)

    metrics: dict[str, Any] = {}
    for metric_name, column in CSV_PEAK_DIAGNOSTIC_COLUMNS.items():
        if metric_name not in peak_diagnostics:
            continue
        limit = limits.get(column)
        if limit is None:
            continue
        boundary_peak = _subset_peak_diagnostic(boundary_rows, column, limit)
        steady_peak = _subset_peak_diagnostic(steady_rows, column, limit)
        diagnosis = "clean"
        if steady_peak and steady_peak["exceed_count"] > 0:
            diagnosis = "steady_state"
        elif boundary_peak and boundary_peak["exceed_count"] > 0:
            diagnosis = "boundary_only"
        metrics[metric_name] = {
            "limit": limit,
            "diagnosis": diagnosis,
            "boundary_peak": boundary_peak,
            "steady_state_peak": steady_peak,
        }

    return {
        "boundary_row_count": len(boundary_rows),
        "steady_state_row_count": len(steady_rows),
        "boundary_row_indexes": [index + 1 for index in sorted(boundary_indexes)],
        "metrics": metrics,
    }


def _peak_phase_bucket(summary: dict[str, Any], segment_position: str, classification: str) -> str:
    if _is_obd_wrapped_loop(summary):
        return "steady-state OBD runtime stall"
    if _is_fs_wrapped_wifi(summary):
        return "steady-state WiFi/AP file serving stall"
    ble_state = summary.get("bleState", "")
    subscribe_step = summary.get("subscribeStep", "")
    connect_burst_ble = _dominant_named_metric(summary, CONNECT_BURST_BLE_COLUMNS)
    connect_burst_display = _dominant_named_metric(summary, CONNECT_BURST_DISPLAY_COLUMNS)
    if summary.get("pendingDisconnectCleanup"):
        return "boundary spike during disconnect/cleanup/proxy transition"
    if ble_state in CONNECT_PHASE_STATES:
        return "boundary spike during connect/discovery/subscribe"
    if (
        segment_position == "start"
        and ble_state == "CONNECTED"
        and (
            subscribe_step == "COMPLETE"
            or summary.get("proxyAdvertising")
            or connect_burst_ble is not None
            or connect_burst_display is not None
        )
    ):
        return "boundary spike during first-connected burst"
    if ble_state in DISCONNECT_PHASE_STATES:
        return "boundary spike during disconnect/cleanup/proxy transition"
    if summary.get("connectInProgress") or summary.get("asyncConnectPending"):
        return "boundary spike during connect/discovery/subscribe"
    if summary.get("proxyAdvertising") or summary.get("wifiPriorityMode"):
        return "boundary spike during disconnect/cleanup/proxy transition"
    if segment_position == "start":
        return "boundary spike during connect/discovery/subscribe"
    if segment_position == "end":
        return "boundary spike during disconnect/cleanup/proxy transition"
    if classification == "sustained":
        return "sustained steady-state BLE runtime issue"
    return "unknown, requiring deeper instrumentation"


def _augment_peak_diagnostics(
    rows: list[dict[str, int]],
    peak_diagnostics: dict[str, dict[str, Any]],
    profile: str,
) -> dict[str, dict[str, Any]]:
    limits = peak_limit_map(profile)
    enriched: dict[str, dict[str, Any]] = {}
    for metric_name, diagnostic in peak_diagnostics.items():
        column = diagnostic["column"]
        limit = limits.get(column)
        enriched_metric = dict(diagnostic)
        if limit is None:
            enriched_metric["limit"] = None
            enriched_metric["exceed_count"] = 0
            enriched_metric["longest_exceed_run"] = 0
            enriched_metric["classification"] = "unknown"
            enriched_metric["segment_position"] = _segment_position(int(diagnostic["row_index"]), len(rows))
            enriched_metric["top_5_rows"] = [
                _build_row_summary(rows[int(diagnostic["row_index"]) - 1], int(diagnostic["row_index"]), column)
            ]
            enriched_metric["likely_phase_bucket"] = _peak_phase_bucket(
                enriched_metric["top_5_rows"][0],
                enriched_metric["segment_position"],
                enriched_metric["classification"],
            )
            enriched[metric_name] = enriched_metric
            continue

        exceed_rows = [
            (index + 1, row)
            for index, row in enumerate(rows)
            if int(row.get(column, 0)) > limit
        ]
        top_rows = sorted(
            [(_build_row_summary(row, row_index, column)) for row_index, row in exceed_rows],
            key=lambda item: (item["value"], item["row_index"]),
            reverse=True,
        )[:5]
        exceed_count = len(exceed_rows)
        longest_exceed_run = _longest_exceed_run(rows, column, limit)
        classification = (
            "sustained"
            if longest_exceed_run > 3 or (len(rows) > 0 and exceed_count > (len(rows) * 0.05))
            else "spike"
        )
        segment_position = _segment_position(int(diagnostic["row_index"]), len(rows))
        wrapper_symptom_of = None
        if not top_rows:
            top_rows = [
                _build_row_summary(rows[int(diagnostic["row_index"]) - 1], int(diagnostic["row_index"]), column)
            ]
        root_cause_hint = None
        top_row = top_rows[0]
        dominant_connect_burst_ble = _dominant_named_metric(top_row, CONNECT_BURST_BLE_COLUMNS)
        dominant_connect_burst_display = _dominant_named_metric(top_row, CONNECT_BURST_DISPLAY_COLUMNS)
        if metric_name == "loop_max_peak_us":
            if _is_obd_wrapped_loop(top_row):
                wrapper_symptom_of = "obdMax_us"
                dominant_obd_call = _dominant_named_metric(top_row, OBD_SYNC_CALL_COLUMNS)
                if dominant_obd_call is not None:
                    root_cause_hint = f"inline OBD {dominant_obd_call['label']} stall"
                else:
                    root_cause_hint = "inline OBD runtime stall"
            elif "ble_process_max_peak_us" in peak_diagnostics:
                ble_diag = peak_diagnostics["ble_process_max_peak_us"]
                if (int(ble_diag["row_index"]) == int(diagnostic["row_index"]) and
                        int(ble_diag["millis"]) == int(diagnostic["millis"])):
                    loop_value = float(diagnostic["value"])
                    ble_value = float(ble_diag["value"])
                    if ble_value > 0 and (loop_value - ble_value) <= max(50000.0, ble_value * 0.2):
                        wrapper_symptom_of = "ble_process_max_peak_us"
            if root_cause_hint is None:
                if dominant_connect_burst_ble is not None:
                    root_cause_hint = f"connect-burst BLE subphase: {dominant_connect_burst_ble['label']}"
                elif dominant_connect_burst_display is not None:
                    root_cause_hint = f"connect-burst display subphase: {dominant_connect_burst_display['label']}"
        elif metric_name == "ble_process_max_peak_us":
            if dominant_connect_burst_ble is not None:
                root_cause_hint = f"connect-burst BLE subphase: {dominant_connect_burst_ble['label']}"
        elif metric_name == "wifi_max_peak_us":
            if _is_fs_wrapped_wifi(top_row):
                wrapper_symptom_of = "fsMax_us"
                root_cause_hint = "LittleFS static file serving during AP"
            dominant_wifi_phase = _dominant_named_metric(top_row, WIFI_SUBPHASE_COLUMNS)
            if dominant_wifi_phase is not None and root_cause_hint is None:
                root_cause_hint = f"WiFi subphase stall: {dominant_wifi_phase['label']}"
        elif metric_name == "disp_pipe_max_peak_us":
            if dominant_connect_burst_display is not None:
                root_cause_hint = f"connect-burst display subphase: {dominant_connect_burst_display['label']}"
        enriched_metric.update(
            {
                "limit": limit,
                "exceed_count": exceed_count,
                "longest_exceed_run": longest_exceed_run,
                "classification": classification,
                "segment_position": segment_position,
                "top_5_rows": top_rows,
                "likely_phase_bucket": _peak_phase_bucket(top_rows[0], segment_position, classification),
            }
        )
        if wrapper_symptom_of:
            enriched_metric["wrapper_symptom_of"] = wrapper_symptom_of
        dominant_obd_call = _dominant_named_metric(top_row, OBD_SYNC_CALL_COLUMNS)
        if dominant_obd_call is not None:
            enriched_metric["obd_dominant_sync_call_column"] = dominant_obd_call["column"]
            enriched_metric["obd_dominant_sync_call_label"] = dominant_obd_call["label"]
        dominant_wifi_phase = _dominant_named_metric(top_row, WIFI_SUBPHASE_COLUMNS)
        if dominant_wifi_phase is not None:
            enriched_metric["wifi_dominant_subphase_column"] = dominant_wifi_phase["column"]
            enriched_metric["wifi_dominant_subphase_label"] = dominant_wifi_phase["label"]
        if dominant_connect_burst_ble is not None:
            enriched_metric["connect_burst_ble_subphase_column"] = dominant_connect_burst_ble["column"]
            enriched_metric["connect_burst_ble_subphase_label"] = dominant_connect_burst_ble["label"]
        if dominant_connect_burst_display is not None:
            enriched_metric["connect_burst_display_subphase_column"] = dominant_connect_burst_display["column"]
            enriched_metric["connect_burst_display_subphase_label"] = dominant_connect_burst_display["label"]
        if root_cause_hint:
            enriched_metric["root_cause_hint"] = root_cause_hint
        enriched[metric_name] = enriched_metric
    return enriched


def extract_metrics(
    rows: list[dict[str, int]],
    source_schema: int,
) -> tuple[dict[str, tuple[float, str]], dict[str, dict[str, Any]], list[str]]:
    if not rows:
        return {}, {}, []

    metrics: dict[str, tuple[float, str]] = {
        "metrics_ok_samples": (float(len(rows)), metric_unit("metrics_ok_samples"))
    }
    columns = set(rows[0].keys())
    unsupported_metrics = unsupported_metrics_for_perf_csv(source_schema, columns)

    for metric_name, column in CSV_DELTA_COLUMNS.items():
        if metric_name in unsupported_metrics:
            continue
        value = _delta_metric(rows, column)
        if value is not None:
            metrics[metric_name] = (value, metric_unit(metric_name))

    for column, metric_name in DISPLAY_COUNTER_DELTA_MAPPINGS:
        if metric_name not in CANONICAL_METRIC_UNITS or metric_name in unsupported_metrics:
            continue
        value = _delta_metric(rows, column)
        if value is not None:
            metrics[metric_name] = (value, metric_unit(metric_name))

    display_drive_activity: float | None = None
    display_updates_metric = metrics.get("display_updates_delta")
    if display_updates_metric is not None:
        display_drive_activity = display_updates_metric[0]

    preview_delta = _delta_metric(rows, "displayPreviewRenderCount") if _has_column(rows, "displayPreviewRenderCount") else None
    restore_delta = _delta_metric(rows, "displayRestoreRenderCount") if _has_column(rows, "displayRestoreRenderCount") else None
    if preview_delta is not None or restore_delta is not None:
        preview_restore_activity = float((preview_delta or 0.0) + (restore_delta or 0.0))
        if display_drive_activity is None or preview_restore_activity > display_drive_activity:
            display_drive_activity = preview_restore_activity
    if display_drive_activity is not None:
        metrics["display_drive_activity_delta"] = (
            display_drive_activity,
            metric_unit("display_drive_activity_delta"),
        )

    peak_diagnostics: dict[str, dict[str, Any]] = {}
    for metric_name, column in CSV_PEAK_ONLY_COLUMNS.items():
        value = _peak_metric(rows, column)
        if value is None:
            continue
        metrics[metric_name] = (value, metric_unit(metric_name))
        if metric_name in CSV_PEAK_DIAGNOSTIC_COLUMNS:
            diagnostic = _peak_diagnostic(rows, column)
            if diagnostic is not None:
                peak_diagnostics[metric_name] = diagnostic

    connect_burst_window = _connect_burst_row_window(rows)
    if connect_burst_window is not None:
        start, end = connect_burst_window
        burst_rows = rows[start:end]
        metrics["connect_burst_samples_to_stable"] = (
            float(len(burst_rows)),
            metric_unit("connect_burst_samples_to_stable"),
        )
        time_to_stable = _connect_burst_time_to_stable_ms(rows, start, end)
        if time_to_stable is not None:
            metrics["connect_burst_time_to_stable_ms"] = (
                time_to_stable,
                metric_unit("connect_burst_time_to_stable_ms"),
            )
        for metric_name, column in CSV_CONNECT_BURST_PEAK_COLUMNS.items():
            value = _peak_metric(burst_rows, column)
            if value is not None:
                metrics[metric_name] = (value, metric_unit(metric_name))

    sd_latency_split = build_sd_latency_split(rows)
    if sd_latency_split.get("available"):
        start_peak = sd_latency_split.get("start_peak")
        runtime_peak = sd_latency_split.get("runtime_peak")
        if start_peak is not None:
            metrics["sd_start_max_peak_us"] = (
                float(start_peak["value"]),
                metric_unit("sd_start_max_peak_us"),
            )
        if runtime_peak is not None:
            metrics["sd_runtime_max_peak_us"] = (
                float(runtime_peak["value"]),
                metric_unit("sd_runtime_max_peak_us"),
            )

    free_dma_floor_column = "freeDmaMin" if _has_column(rows, "freeDmaMin") else "freeDma"
    largest_dma_floor_column = "largestDmaMin" if _has_column(rows, "largestDmaMin") else "largestDma"
    dma_free_floor = _floor_metric(rows, free_dma_floor_column)
    dma_largest_floor = _floor_metric(rows, largest_dma_floor_column)
    if dma_free_floor is not None:
        metrics["dma_free_min_bytes"] = (dma_free_floor, metric_unit("dma_free_min_bytes"))
    if dma_largest_floor is not None:
        metrics["dma_largest_min_bytes"] = (dma_largest_floor, metric_unit("dma_largest_min_bytes"))

    if _has_column(rows, "wifiMax_us"):
        wifi_samples = [float(int(row.get("wifiMax_us", 0))) for row in rows]
        wifi_p95 = percentile(wifi_samples, 95)
        if wifi_p95 is not None:
            metrics["wifi_p95_us"] = (wifi_p95, metric_unit("wifi_p95_us"))

    if _has_column(rows, "dispPipeMax_us"):
        disp_samples = [float(int(row.get("dispPipeMax_us", 0))) for row in rows]
        disp_p95 = percentile(disp_samples, 95)
        if disp_p95 is not None:
            metrics["disp_pipe_p95_us"] = (disp_p95, metric_unit("disp_pipe_p95_us"))

    if _has_column(rows, "freeDma") and _has_column(rows, "largestDma"):
        fragmentation_samples: list[float] = []
        for row in rows:
            free_dma = float(int(row.get("freeDma", 0)))
            largest_dma = float(int(row.get("largestDma", 0)))
            if free_dma > 0:
                fragmentation_samples.append((1.0 - (largest_dma / free_dma)) * 100.0)
        fragmentation_p95 = percentile(fragmentation_samples, 95)
        if fragmentation_p95 is not None:
            metrics["dma_fragmentation_pct_p95"] = (
                fragmentation_p95,
                metric_unit("dma_fragmentation_pct_p95"),
            )

    if _has_column(rows, "notifyToDisplayMax_ms"):
        notify_max = _peak_metric(rows, "notifyToDisplayMax_ms")
        if notify_max is not None:
            metrics["notify_to_display_max_ms"] = (
                notify_max,
                metric_unit("notify_to_display_max_ms"),
            )
    if _has_column(rows, "notifyToDisplayTotalCount"):
        # Per-window sample count is reset each reporting window, so sum
        # rather than delta to get total samples observed across the run.
        notify_samples = float(sum(int(row.get("notifyToDisplayTotalCount", 0)) for row in rows))
        metrics["notify_to_display_sample_count"] = (
            notify_samples,
            metric_unit("notify_to_display_sample_count"),
        )

    return metrics, peak_diagnostics, sorted(unsupported_metrics)


def write_metrics_ndjson(
    path: Path,
    run_id: str,
    git_sha: str,
    suite_or_profile: str,
    metrics: dict[str, tuple[float, str]],
) -> int:
    count = 0
    with path.open("w", encoding="utf-8") as handle:
        for metric_name, (value, unit) in sorted(metrics.items()):
            handle.write(
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": run_id,
                        "git_sha": git_sha,
                        "run_kind": "real_fw_soak",
                        "suite_or_profile": suite_or_profile,
                        "metric": metric_name,
                        "sample": "value",
                        "value": value,
                        "unit": unit,
                        "tags": {},
                    },
                    sort_keys=True,
                )
            )
            handle.write("\n")
            count += 1
    return count


def run_csv_scorecard(
    profile: str,
    session_selector: str,
    rows: list[dict[str, int]],
    session_idx: int,
    total_sessions: int,
) -> dict[str, Any]:
    config = score_perf_csv.load_threshold_config(SLO_FILE)
    checks = score_perf_csv.evaluate(rows, profile, config)
    hard_fail = [check for check in checks if check.level == "hard" and not check.passed]
    advisory_fail = [check for check in checks if check.level == "advisory" and not check.passed]
    return {
        "profile": profile,
        "segment_selector": session_selector,
        "session_index": session_idx,
        "total_sessions": total_sessions,
        "rows": len(rows),
        "duration_s": score_perf_csv.duration_s(rows),
        "hard_failures": len(hard_fail),
        "advisory_warnings": len(advisory_fail),
        "result": "FAIL" if hard_fail else ("PASS_WITH_WARNINGS" if advisory_fail else "PASS"),
        "checks": [
            {
                "metric": check.metric,
                "level": check.level,
                "source": check.source,
                "value": check.value,
                "op": check.op,
                "limit": check.limit,
                "passed": check.passed,
            }
            for check in checks
        ],
    }


def _panic_path_for_csv(csv_path: Path) -> Path | None:
    candidate = csv_path.with_suffix(".panic.jsonl")
    return candidate if candidate.exists() else None


def _panic_summary(panic_path: Path | None) -> tuple[dict[str, Any], str]:
    if panic_path is None:
        return {
            "present": False,
            "runtime_crash_detected": False,
            "preexisting_crash_state": False,
            "state_change_count": None,
        }, "PASS"

    panic_kv, raw_panic_kv = import_drive_log.run_kv_parser(import_drive_log.SOAK_PARSE_PANIC, panic_path)
    runtime_crash = import_drive_log.panic_runtime_crash_detected(panic_kv)
    preexisting_crash = import_drive_log.panic_preexisting_crash_state(panic_kv)
    if runtime_crash:
        base_result = "FAIL"
    elif preexisting_crash:
        base_result = "PASS_WITH_WARNINGS"
    else:
        base_result = "PASS"
    return {
        "present": True,
        "runtime_crash_detected": runtime_crash,
        "preexisting_crash_state": preexisting_crash,
        "state_change_count": import_drive_log.integer(panic_kv.get("state_change_count", "")),
        "first_was_crash": import_drive_log.integer(panic_kv.get("first_was_crash", "")),
        "last_was_crash": import_drive_log.integer(panic_kv.get("last_was_crash", "")),
        "raw_kv": panic_kv,
        "path": str(panic_path),
        "parsed_text": raw_panic_kv,
    }, base_result


def append_import_sections(
    text_path: Path,
    *,
    csv_path: Path,
    source_schema: int,
    coverage_status: str,
    unsupported_metrics: list[str],
    selected_segment: dict[str, Any],
    peak_diagnostics: dict[str, dict[str, Any]],
    partition_analysis: dict[str, Any],
    sd_latency_split: dict[str, Any],
    csv_scorecard: dict[str, Any],
    panic_summary: dict[str, Any],
) -> None:
    if selected_segment.get("speed_active_rows_supported"):
        drive_evidence = (
            f"speed_rows={selected_segment['speed_active_rows']}"
            + (
                f" via {selected_segment['speed_active_column']}"
                if selected_segment.get("speed_active_column")
                else ""
            )
        )
    else:
        drive_evidence = "speed_rows=n/a (schema lacks direct speed column)"

    lines = [
        "",
        "## Imported CSV",
        "",
        f"- Source CSV: `{csv_path}`",
        f"- Source schema: `{source_schema}`",
        f"- Coverage status: `{coverage_status}`",
        f"- Selected segment: `{selected_segment['session_index']}`"
        + (f" token={selected_segment['token']}" if selected_segment.get("token") else ""),
        f"- Segment rows/duration: {selected_segment['row_count']} rows / {selected_segment['duration_s']:.1f}s",
        f"- Segment drive evidence: {drive_evidence}, rx_delta={selected_segment['rx_delta']}",
        f"- Unsupported metrics: {', '.join(unsupported_metrics) if unsupported_metrics else 'none'}",
        f"- Panic log: {'present' if panic_summary.get('present') else 'missing'} `{panic_summary.get('path', 'n/a')}`",
        f"- Panic runtime crash detected: {'yes' if panic_summary.get('runtime_crash_detected') else 'no'}",
        f"- Panic preexisting crash state: {'yes' if panic_summary.get('preexisting_crash_state') else 'no'}",
        "",
        "## Peak Diagnostics",
        "",
    ]
    if peak_diagnostics:
        for metric_name in sorted(peak_diagnostics):
            peak = peak_diagnostics[metric_name]
            rendered_value = int(round(peak["value"])) if abs(peak["value"] - round(peak["value"])) < 1e-9 else peak["value"]
            lines.append(
                f"- `{metric_name}`: value={rendered_value}, row={peak['row_index']}, millis={peak['millis']}, "
                f"classification={peak.get('classification', 'unknown')}, exceed_count={peak.get('exceed_count', 0)}, "
                f"longest_exceed_run={peak.get('longest_exceed_run', 0)}, segment_position={peak.get('segment_position', 'unknown')}"
            )
            if peak.get("wrapper_symptom_of"):
                lines.append(f"  wrapper_symptom_of={peak['wrapper_symptom_of']}")
            if peak.get("likely_phase_bucket"):
                lines.append(f"  likely_phase_bucket={peak['likely_phase_bucket']}")
            if peak.get("root_cause_hint"):
                lines.append(f"  root_cause_hint={peak['root_cause_hint']}")
            if peak.get("obd_dominant_sync_call_label"):
                lines.append(
                    f"  obd_dominant_sync_call={peak['obd_dominant_sync_call_label']} ({peak['obd_dominant_sync_call_column']})"
                )
            if peak.get("wifi_dominant_subphase_label"):
                lines.append(
                    f"  wifi_dominant_subphase={peak['wifi_dominant_subphase_label']} ({peak['wifi_dominant_subphase_column']})"
                )
            if peak.get("connect_burst_ble_subphase_label"):
                lines.append(
                    f"  connect_burst_ble_subphase={peak['connect_burst_ble_subphase_label']} ({peak['connect_burst_ble_subphase_column']})"
                )
            if peak.get("connect_burst_display_subphase_label"):
                lines.append(
                    f"  connect_burst_display_subphase={peak['connect_burst_display_subphase_label']} ({peak['connect_burst_display_subphase_column']})"
                )
            for top_row in peak.get("top_5_rows", [])[:3]:
                detail_fields = [
                    f"row={top_row['row_index']}",
                    f"millis={top_row['millis']}",
                    f"value={int(round(top_row['value'])) if abs(top_row['value'] - round(top_row['value'])) < 1e-9 else top_row['value']}",
                ]
                for field in TOP_ROW_FIELDS:
                    if field in top_row:
                        detail_fields.append(f"{field}={top_row[field]}")
                for field in (
                    "loopMax_us",
                    "dispMax_us",
                    "obdMax_us",
                    "wifiMax_us",
                    "fsMax_us",
                    "bleProcessMax_us",
                    "dispPipeMax_us",
                ):
                    if field in top_row:
                        detail_fields.append(f"{field}={top_row[field]}")
                for field in (
                    "bleFollowupRequestAlertMax_us",
                    "bleFollowupRequestVersionMax_us",
                    "bleConnectStableCallbackMax_us",
                    "bleProxyStartMax_us",
                                    "displayGapRecoverMax_us",
                ):
                    if field in top_row:
                        detail_fields.append(f"{field}={top_row[field]}")
                if "bleState" in top_row:
                    detail_fields.append(f"bleState={top_row['bleState']}")
                if "subscribeStep" in top_row:
                    detail_fields.append(f"subscribeStep={top_row['subscribeStep']}")
                for field in (
                    "connectInProgress",
                    "asyncConnectPending",
                    "pendingDisconnectCleanup",
                    "proxyAdvertising",
                    "wifiPriorityMode",
                ):
                    if field in top_row:
                        detail_fields.append(f"{field}={'yes' if top_row[field] else 'no'}")
                lines.append(f"  top_row: {', '.join(detail_fields)}")
    else:
        lines.append("- none")
    lines.extend(
        [
            "",
            "## SD Start vs Runtime",
            "",
            f"- Start window: first {sd_latency_split.get('start_window_ms', SD_START_WINDOW_MS)} ms of the selected segment",
            f"- Start rows: {sd_latency_split.get('start_row_count', 0)}",
            f"- Runtime rows: {sd_latency_split.get('runtime_row_count', 0)}",
        ]
    )
    for label, payload in (
        ("start_peak", sd_latency_split.get("start_peak")),
        ("runtime_peak", sd_latency_split.get("runtime_peak")),
    ):
        if not payload:
            lines.append(f"- `{label}`: none")
            continue
        rendered_value = int(round(payload["value"])) if abs(payload["value"] - round(payload["value"])) < 1e-9 else payload["value"]
        lines.append(
            f"- `{label}`: value={rendered_value}, row={payload['row_index']}, "
            f"millis={payload['millis']}, relative_ms={payload['relative_ms']}"
        )
    lines.extend(
        [
            "",
            "## Boundary vs Steady-State",
            "",
            f"- Boundary rows: {partition_analysis.get('boundary_row_count', 0)}",
            f"- Steady-state rows: {partition_analysis.get('steady_state_row_count', 0)}",
        ]
    )
    partition_metrics = partition_analysis.get("metrics", {})
    if partition_metrics:
        for metric_name in sorted(partition_metrics):
            partition = partition_metrics[metric_name]
            lines.append(
                f"- `{metric_name}`: diagnosis={partition.get('diagnosis', 'unknown')}, limit={int(partition['limit']) if abs(partition['limit'] - round(partition['limit'])) < 1e-9 else partition['limit']}"
            )
            for label, payload in (("boundary_peak", partition.get("boundary_peak")), ("steady_state_peak", partition.get("steady_state_peak"))):
                if not payload:
                    lines.append(f"  {label}: none")
                    continue
                rendered_value = int(round(payload["value"])) if abs(payload["value"] - round(payload["value"])) < 1e-9 else payload["value"]
                lines.append(
                    f"  {label}: value={rendered_value}, row={payload['row_index']}, millis={payload['millis']}, "
                    f"classification={payload['classification']}, exceed_count={payload['exceed_count']}, "
                    f"longest_exceed_run={payload['longest_exceed_run']}"
                )
    else:
        lines.append("- none")
    lines.extend(
        [
            "",
            "## Complementary CSV Scorecard",
            "",
            f"- Profile: `{csv_scorecard['profile']}`",
            f"- Segment selector: `{csv_scorecard['segment_selector']}`",
            f"- Session: {csv_scorecard['session_index']}/{csv_scorecard['total_sessions']}",
            f"- Rows: {csv_scorecard['rows']}, Duration: {csv_scorecard['duration_s']:.1f}s",
            f"- CSV SLO result: **{csv_scorecard['result']}** (hard={csv_scorecard['hard_failures']}, advisory={csv_scorecard['advisory_warnings']})",
        ]
    )
    with text_path.open("a", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    csv_path = Path(args.input).resolve()
    if not csv_path.exists():
        print(f"ERROR: file not found: {csv_path}", file=sys.stderr)
        return 3

    selector = _selector_arg(args)
    try:
        sessions = load_sessions(csv_path)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3
    if not sessions:
        print("ERROR: no sessions found in CSV", file=sys.stderr)
        return 3

    try:
        session_meta, rows, selected_summary, summaries, effective_selector = select_segment(sessions, selector)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    if args.list_segments:
        print(render_segment_listing(summaries, selected_summary.session_index, effective_selector, csv_path), end="")
        return 0

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    source_schema = selected_summary.schema
    profile = args.profile
    suite_or_profile = profile
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    git_sha = args.git_sha or "unknown"
    git_ref = args.git_ref or "unknown"
    board_id = args.board_id or "unknown"
    stress_class = args.stress_class
    run_id = f"perf_csv_import_{timestamp}_{selected_summary.token or 'unknown'}"

    segments_payload = {
        "source": str(csv_path),
        "segment_selector": selector,
        "effective_segment_selector": effective_selector,
        "selected_segment": selected_summary.to_dict(),
        "sessions": [summary.to_dict() for summary in summaries],
    }
    segments_path = out_dir / "segments.json"
    segments_path.write_text(json.dumps(segments_payload, indent=2) + "\n", encoding="utf-8")

    metrics, peak_diagnostics, unsupported_metrics = extract_metrics(rows, source_schema)
    if not metrics:
        print("ERROR: no metrics extracted from CSV", file=sys.stderr)
        return 3
    if stress_class != "display_preview":
        metrics.pop("display_drive_activity_delta", None)
        metrics.pop("display_preview_render_peak_us", None)
        metrics.pop("display_preview_first_render_peak_us", None)
        metrics.pop("display_preview_steady_render_peak_us", None)
    peak_diagnostics = _augment_peak_diagnostics(rows, peak_diagnostics, profile)
    partition_analysis = build_peak_partition_analysis(rows, peak_diagnostics, profile)
    sd_latency_split = build_sd_latency_split(rows)

    metrics_ndjson = out_dir / "metrics.ndjson"
    write_metrics_ndjson(metrics_ndjson, run_id, git_sha, suite_or_profile, metrics)

    csv_scorecard = run_csv_scorecard(profile, effective_selector, rows, selected_summary.session_index, len(summaries))
    csv_scorecard_path = out_dir / "csv_scorecard.json"
    csv_scorecard_path.write_text(json.dumps(csv_scorecard, indent=2) + "\n", encoding="utf-8")

    panic_path = _panic_path_for_csv(csv_path)
    try:
        panic_summary, base_result = _panic_summary(panic_path)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3
    if panic_summary.get("present") and panic_summary.get("parsed_text"):
        (out_dir / "parsed_panic_kv.txt").write_text(str(panic_summary["parsed_text"]), encoding="utf-8")

    coverage_status = coverage_status_for_unsupported_metrics(unsupported_metrics)
    selected_segment_payload = {
        **selected_summary.to_dict(),
        "selector": selector,
        "effective_selector": effective_selector,
    }
    diagnostics = {
        "source_files": {
            "csv": str(csv_path),
            "panic_jsonl": panic_summary.get("path", ""),
        },
        "source_schema": source_schema,
        "coverage_status": coverage_status,
        "unsupported_metrics": unsupported_metrics,
        "selected_segment": selected_segment_payload,
        "peaks": peak_diagnostics,
        "latency_partitions": partition_analysis,
        "sd_latency_split": sd_latency_split,
        "panic": {
            key: value
            for key, value in panic_summary.items()
            if key not in {"parsed_text", "raw_kv"}
        },
    }
    diagnostics_path = out_dir / "import_diagnostics.json"
    diagnostics_path.write_text(json.dumps(diagnostics, indent=2) + "\n", encoding="utf-8")

    manifest_path = out_dir / "manifest.json"
    manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "timestamp_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "git_sha": git_sha,
        "git_ref": git_ref,
        "run_kind": "real_fw_soak",
        "board_id": board_id,
        "env": "perf-csv-import",
        "lane": args.lane,
        "suite_or_profile": suite_or_profile,
        "stress_class": stress_class,
        "result": base_result,
        "base_result": base_result,
        "metrics_file": "metrics.ndjson",
        "scoring_file": "scoring.json",
        "tracks": [suite_or_profile],
        "source_input": str(csv_path),
        "source_type": "perf_csv",
        "source_schema": source_schema,
        "selected_segment": selected_segment_payload,
        "unsupported_metrics": unsupported_metrics,
        "coverage_status": coverage_status,
        "segments_file": "segments.json",
        "csv_scorecard_file": "csv_scorecard.json",
        "import_diagnostics_file": "import_diagnostics.json",
        "rows": len(rows),
        "duration_s": selected_summary.duration_s,
    }
    if args.mode_coverage_json:
        mode_coverage_path = Path(args.mode_coverage_json).resolve()
        try:
            mode_coverage = json.loads(mode_coverage_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            print(f"ERROR: failed to read --mode-coverage-json {mode_coverage_path}: {exc}", file=sys.stderr)
            return 3
        if not isinstance(mode_coverage, dict):
            print(f"ERROR: --mode-coverage-json {mode_coverage_path} did not contain a JSON object", file=sys.stderr)
            return 3
        manifest["mode_coverage"] = mode_coverage
    if panic_summary.get("present"):
        manifest["source_panic_jsonl"] = str(panic_path)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    baseline_paths = [Path(path).resolve() for path in args.compare_to if path]
    try:
        scored = score_hardware_run.score_run(manifest_path, CATALOG_PATH, baseline_paths)
    except Exception as exc:
        print(f"ERROR: scoring failed: {exc}", file=sys.stderr)
        return 3

    scoring_path = out_dir / "scoring.json"
    comparison_txt = out_dir / "comparison.txt"
    comparison_tsv = out_dir / "comparison.tsv"
    scoring_path.write_text(json.dumps(scored, indent=2) + "\n", encoding="utf-8")
    write_comparison_text(scored, comparison_txt)
    write_comparison_tsv(scored, comparison_tsv)
    append_import_sections(
        comparison_txt,
        csv_path=csv_path,
        source_schema=source_schema,
        coverage_status=coverage_status,
        unsupported_metrics=unsupported_metrics,
        selected_segment=selected_segment_payload,
        peak_diagnostics=peak_diagnostics,
        partition_analysis=partition_analysis,
        sd_latency_split=sd_latency_split,
        csv_scorecard=csv_scorecard,
        panic_summary=panic_summary,
    )

    manifest["result"] = scored["result"]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    summary = scored["summary"]
    print(
        f"Source: {csv_path.name} segment {selected_summary.session_index}/{len(summaries)} "
        f"({len(rows)} rows, {selected_summary.duration_s:.1f}s)"
    )
    print(f"Segment selector: {effective_selector}")
    print(f"Coverage status: {coverage_status}")
    print(
        f"Hardware catalog score: {scored['result']} "
        f"(hard={summary['hard_failures']}, advisory={summary['advisory_failures']}, unsupported={summary.get('unsupported_metrics', 0)})"
    )
    print(
        f"CSV SLO scorecard:      {csv_scorecard['result']} "
        f"(hard={csv_scorecard['hard_failures']}, advisory={csv_scorecard['advisory_warnings']})"
    )
    boundary_only = sorted(
        metric_name
        for metric_name, payload in partition_analysis.get("metrics", {}).items()
        if payload.get("diagnosis") == "boundary_only"
    )
    steady_state = sorted(
        metric_name
        for metric_name, payload in partition_analysis.get("metrics", {}).items()
        if payload.get("diagnosis") == "steady_state"
    )
    if boundary_only or steady_state:
        print(
            "Latency split: "
            f"boundary_only={','.join(boundary_only) if boundary_only else 'none'} "
            f"steady_state={','.join(steady_state) if steady_state else 'none'}"
        )
    print(f"Artifacts: {out_dir}")

    result = str(scored["result"])
    if result == "FAIL":
        return 2
    if result == "PASS_WITH_WARNINGS":
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
