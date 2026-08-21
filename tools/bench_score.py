#!/usr/bin/env python3
"""Score a unified bench run made of core/display/replay SD CSV windows.

The bench score intentionally has no baseline concept. A window passes when
collection completed, imported metrics are present, hard catalog failures are
zero, and advisory catalog failures are zero. Regression/no-baseline language
is not part of this result.
"""

from __future__ import annotations

import argparse
import csv
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

ENCOUNTER_COLUMNS = (
    "millis",
    "encounter_id",
    "sample_seq",
    "event",
    "v1_index",
    "alert_count",
    "band",
    "frequency_mhz",
    "direction",
    "front_raw",
    "rear_raw",
    "front_bars",
    "rear_bars",
    "priority",
    "junk",
    "photo_type",
    "dropped_snapshots",
)
ENCOUNTER_INTEGER_COLUMNS = tuple(
    column for column in ENCOUNTER_COLUMNS if column not in {"event", "band", "direction"}
)

# These are authored public-bench semantics, not values rebuilt with the
# emulator's protocol helpers. Alert-table row order is part of the contract.
EXPECTED_REPLAY_CHECKPOINTS = (
    (
        "two-row handoff",
        (
            (1, "K", 24_150, "SIDE", 4, 0, 0),
            (2, "Ka", 34_700, "FRONT", 5, 0, 1),
        ),
    ),
    (
        "three-row table",
        (
            (1, "K", 24_150, "SIDE", 4, 0, 0),
            (2, "Ka", 34_700, "FRONT", 6, 0, 1),
            (3, "Ka", 35_500, "REAR", 0, 4, 0),
        ),
    ),
    (
        "two-row clear-down",
        (
            (1, "K", 24_150, "SIDE", 4, 0, 0),
            (2, "Ka", 34_700, "FRONT", 5, 0, 1),
        ),
    ),
    (
        "one-row clear-down",
        (
            (1, "K", 24_150, "SIDE", 4, 0, 1),
        ),
    ),
)

HANDSHAKE_LEDGER_KIND = "v1replay_handshake_ledger"
HANDSHAKE_LEDGER_LEGACY_SCHEMA = 1
HANDSHAKE_LEDGER_SCHEMA = 2
HANDSHAKE_LEDGER_TIMEBASE = "epoch_monotonic_ms"
MAX_HANDSHAKE_LEDGER_BYTES = 8 * 1024
MAX_HANDSHAKE_EPOCHS = 4
MAX_HANDSHAKE_EVENTS_PER_EPOCH = 12
MAX_HANDSHAKE_EVENTS = MAX_HANDSHAKE_EPOCHS * MAX_HANDSHAKE_EVENTS_PER_EPOCH
MAX_HANDSHAKE_ELAPSED_MS = 0xFFFFFFFF
# Six non-start events plus five accepted starts use 11 slots, leaving the
# twelfth slot to expose a violating sixth start before the writer's hard cap.
MAX_HANDSHAKE_START_REQUESTS_PER_EPOCH = 5
MIN_HANDSHAKE_START_RETRY_MS = 1000
HEX_DIGEST_LENGTH = 64
HANDSHAKE_REQUEST_CHANNELS = {"B6D4", "BAD4"}
MAX_RECONNECT_PREFLIGHT_LOG_BYTES = 256 * 1024
MAX_BENCH_SERIAL_LOG_BYTES = 8 * 1024 * 1024
RECONNECT_PROCESS_EXIT_BOUNDARY = "HOST_BOUNDARY reconnect_preflight_process_exited"
RECONNECT_PREFLIGHT_START_BOUNDARY = "HOST_BOUNDARY reconnect_preflight_start"
RECONNECT_FENCE_BEGIN = "HOST_BOUNDARY reconnect_preflight_fence_begin"
RECONNECT_FENCE_COMPLETE = "HOST_BOUNDARY reconnect_preflight_fence_complete"
RECONNECT_POST_CLEANUP_FENCE_BEGIN = "HOST_BOUNDARY reconnect_post_cleanup_fence_begin"
RECONNECT_POST_CLEANUP_FENCE_COMPLETE = "HOST_BOUNDARY reconnect_post_cleanup_fence_complete"
RECONNECT_PRE_QSTART_FENCE_BEGIN = "HOST_BOUNDARY reconnect_pre_qstart_fence_begin"
RECONNECT_PRE_QSTART_FENCE_COMPLETE = "HOST_BOUNDARY reconnect_pre_qstart_fence_complete"
V1_DISCONNECT_CLEANUP_PREFIX = "[BLE] V1 disconnected; cleared LCD BLE state at "
BOOT_PREFIX = "BOOT bootId="

START_ALERT_REQUEST = (0xDA, 0xE6, 0x41, ())
VERSION_REQUEST = (0xDA, 0xE6, 0x01, ())
VERSION_RESPONSE = (0xD6, 0xEA, 0x02, tuple(b"v4.1038"))
ALL_VOLUME_REQUEST = (0xDA, 0xE6, 0x3C, ())
ALL_VOLUME_RESPONSE = (0xD6, 0xEA, 0x3D, (0x04, 0x00, 0x04, 0x00))
RECONNECT_PREFLIGHT_CLEAR_FRAME = (
    0xAA,
    0xD8,
    0xEA,
    0x43,
    0x08,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0xB7,
    0xAB,
)
RECONNECT_VERSION_REPLY_FRAME = (
    0xAA, 0xD6, 0xEA, 0x02, 0x08,
    0x76, 0x34, 0x2E, 0x31, 0x30, 0x33, 0x38,
    0x18, 0xAB,
)
RECONNECT_ALL_VOLUME_REPLY_FRAME = (
    0xAA, 0xD6, 0xEA, 0x3D, 0x05,
    0x04, 0x00, 0x04, 0x00,
    0xB4, 0xAB,
)
REPLAY_ALL_VOLUME_COUNTER = "v1AllVolumeParsed"
REPLAY_ALL_VOLUME_MIN_SCHEMA = 46


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


def valid_digest(value: Any) -> bool:
    text = str(value or "")
    return len(text) == HEX_DIGEST_LENGTH and all(
        character in "0123456789abcdef" for character in text
    )


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


def same_window_artifact(window_dir: Path, raw: Any, label: str) -> tuple[Path | None, str]:
    text = str(raw or "").strip()
    if not text:
        return None, f"replay window is missing required {label} path"

    candidate = Path(text)
    if not candidate.is_absolute():
        candidate = window_dir / candidate
    try:
        window_root = window_dir.resolve()
        resolved = candidate.resolve()
        resolved.relative_to(window_root)
    except (OSError, RuntimeError, ValueError):
        return None, f"replay {label} must resolve inside its replay window"
    if not resolved.is_file():
        return None, f"replay {label} is missing or is not a file: {resolved.name}"
    return resolved, ""


def handshake_collection_failure(message: str) -> dict[str, Any]:
    return {"result": "COLLECTION_FAILED", "evidence": [message]}


def decode_handshake_frame(raw: list[int]) -> tuple[tuple[int, int, int, tuple[int, ...]] | None, str]:
    """Decode a ledger frame without using the emulator's protocol helpers."""
    if len(raw) < 7:
        return None, "is shorter than a complete frame"
    if raw[0] != 0xAA or raw[-1] != 0xAB:
        return None, "has invalid frame boundaries"
    declared_length = raw[4]
    if declared_length < 1 or len(raw) != 6 + declared_length:
        return None, "has an invalid declared length"
    checksum_index = len(raw) - 2
    if raw[checksum_index] != sum(raw[:checksum_index]) & 0xFF:
        return None, "has an invalid checksum"
    return (raw[1], raw[2], raw[3], tuple(raw[5:checksum_index])), ""


def score_replay_handshake_ledger(
    path: Path,
    *,
    expected_stream_frame: tuple[int, ...] | None = None,
) -> dict[str, Any]:
    try:
        if path.stat().st_size > MAX_HANDSHAKE_LEDGER_BYTES:
            return handshake_collection_failure("replay handshake ledger exceeds its bounded size")
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        return handshake_collection_failure(f"replay handshake ledger could not be read: {exc}")

    lines = text.splitlines()
    if not lines:
        return handshake_collection_failure("replay handshake ledger is empty")
    if any(not line.strip() for line in lines):
        return handshake_collection_failure("replay handshake ledger contains an empty record")

    records: list[dict[str, Any]] = []
    for line_number, line in enumerate(lines, start=1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            return handshake_collection_failure(
                f"replay handshake ledger line {line_number} is not valid JSON"
            )
        if not isinstance(record, dict):
            return handshake_collection_failure(
                f"replay handshake ledger line {line_number} is not an object"
            )
        records.append(record)

    legacy_header = {
        "schema_version": HANDSHAKE_LEDGER_LEGACY_SCHEMA,
        "kind": HANDSHAKE_LEDGER_KIND,
    }
    current_header = {
        "schema_version": HANDSHAKE_LEDGER_SCHEMA,
        "kind": HANDSHAKE_LEDGER_KIND,
        "timebase": HANDSHAKE_LEDGER_TIMEBASE,
    }
    if records[0] == legacy_header:
        ledger_schema = HANDSHAKE_LEDGER_LEGACY_SCHEMA
    elif records[0] == current_header:
        ledger_schema = HANDSHAKE_LEDGER_SCHEMA
    else:
        return handshake_collection_failure("replay handshake ledger has an invalid header")

    raw_events = records[1:]
    if len(raw_events) > MAX_HANDSHAKE_EVENTS:
        return handshake_collection_failure("replay handshake ledger has too many events")

    events: list[dict[str, Any]] = []
    epoch_ids: set[int] = set()
    event_keys: dict[str, set[str]] = {
        "subscribe": {"event", "epoch", "channel"},
        "request": {"event", "epoch", "channel", "bytes"},
        "response": {"event", "epoch", "channel", "bytes", "delivery"},
        "stream_started": {"event", "epoch", "channel", "bytes", "delivery"},
    }
    if ledger_schema == HANDSHAKE_LEDGER_SCHEMA:
        event_keys = {
            event_name: {*keys, "elapsed_ms"}
            for event_name, keys in event_keys.items()
        }
    previous_elapsed_by_epoch: dict[int, int] = {}
    for line_number, raw_event in enumerate(raw_events, start=2):
        event_name = raw_event.get("event")
        if (
            not isinstance(event_name, str)
            or event_name not in event_keys
            or set(raw_event) != event_keys[event_name]
        ):
            return handshake_collection_failure(
                f"replay handshake ledger line {line_number} has an invalid event schema"
            )
        epoch = raw_event.get("epoch")
        if not isinstance(epoch, int) or isinstance(epoch, bool) or not 1 <= epoch <= MAX_HANDSHAKE_EPOCHS:
            return handshake_collection_failure(
                f"replay handshake ledger line {line_number} has an invalid anonymous epoch"
            )
        channel = raw_event.get("channel")
        if not isinstance(channel, str):
            return handshake_collection_failure(
                f"replay handshake ledger line {line_number} has an invalid channel field"
            )
        event = {"event": event_name, "epoch": epoch, "channel": channel}
        if ledger_schema == HANDSHAKE_LEDGER_SCHEMA:
            elapsed_ms = raw_event.get("elapsed_ms")
            if (
                not isinstance(elapsed_ms, int)
                or isinstance(elapsed_ms, bool)
                or not 0 <= elapsed_ms <= MAX_HANDSHAKE_ELAPSED_MS
            ):
                return handshake_collection_failure(
                    f"replay handshake ledger line {line_number} has an invalid elapsed_ms"
                )
            previous_elapsed = previous_elapsed_by_epoch.get(epoch)
            if previous_elapsed is not None and elapsed_ms < previous_elapsed:
                return handshake_collection_failure(
                    f"replay handshake ledger epoch {epoch} elapsed_ms values decrease"
                )
            if event_name == "subscribe" and elapsed_ms != 0:
                return handshake_collection_failure(
                    f"replay handshake ledger epoch {epoch} subscription elapsed_ms is not zero"
                )
            previous_elapsed_by_epoch[epoch] = elapsed_ms
            event["elapsed_ms"] = elapsed_ms
        if event_name != "subscribe":
            raw_bytes = raw_event.get("bytes")
            if (
                not isinstance(raw_bytes, list)
                or len(raw_bytes) > 64
                or any(
                    not isinstance(value, int)
                    or isinstance(value, bool)
                    or not 0 <= value <= 0xFF
                    for value in raw_bytes
                )
            ):
                return handshake_collection_failure(
                    f"replay handshake ledger line {line_number} has an invalid byte array"
                )
            event["bytes"] = raw_bytes
        if event_name in {"response", "stream_started"}:
            delivery = raw_event.get("delivery")
            if not isinstance(delivery, str):
                return handshake_collection_failure(
                    f"replay handshake ledger line {line_number} has an invalid delivery field"
                )
            event["delivery"] = delivery
        events.append(event)
        epoch_ids.add(epoch)

    if len(epoch_ids) > MAX_HANDSHAKE_EPOCHS:
        return handshake_collection_failure("replay handshake ledger has too many anonymous epochs")
    events_per_epoch = {
        epoch: sum(event["epoch"] == epoch for event in events) for epoch in epoch_ids
    }
    if any(count > MAX_HANDSHAKE_EVENTS_PER_EPOCH for count in events_per_epoch.values()):
        return handshake_collection_failure("replay handshake epoch has too many events")

    semantic_failures: list[str] = []
    expected_epoch = 1
    current_epoch = 0
    for event in events:
        epoch = event["epoch"]
        if epoch != current_epoch:
            if epoch != expected_epoch:
                semantic_failures.append(
                    "replay handshake epochs are not contiguous and ordered"
                )
                break
            current_epoch = epoch
            expected_epoch += 1

    decoded_events: list[dict[str, Any]] = []
    for event in events:
        event_name = event["event"]
        channel = event["channel"]
        if event_name == "subscribe":
            if channel != "B2CE":
                semantic_failures.append(
                    "replay handshake subscribed on the wrong short-response channel"
                )
            decoded_events.append({**event, "kind": "subscribe"})
            continue

        decoded, error = decode_handshake_frame(event["bytes"])
        if decoded is None:
            semantic_failures.append(f"replay handshake {event_name} frame {error}")
            continue
        destination, origin, packet_id, payload = decoded

        if event_name == "request":
            if channel not in HANDSHAKE_REQUEST_CHANNELS:
                semantic_failures.append(
                    "replay handshake request used an unsupported command channel"
                )
            frame = (destination, origin, packet_id, payload)
            request_kind = {
                START_ALERT_REQUEST: "start_request",
                VERSION_REQUEST: "version_request",
                ALL_VOLUME_REQUEST: "all_volume_request",
            }.get(frame)
            if request_kind is None:
                semantic_failures.append(
                    "replay handshake request has the wrong header, ID, length, or payload"
                )
                continue
            decoded_events.append({**event, "kind": request_kind})
            continue

        if event.get("delivery") != "delivered":
            semantic_failures.append(
                f"replay handshake {event_name} was not delivered"
            )
        if channel != "B2CE":
            semantic_failures.append(
                f"replay handshake {event_name} used the wrong response channel"
            )

        frame = (destination, origin, packet_id, payload)
        if event_name == "response":
            response_kind = {
                VERSION_RESPONSE: "version_response",
                ALL_VOLUME_RESPONSE: "all_volume_response",
            }.get(frame)
            if response_kind is None:
                semantic_failures.append(
                    "replay handshake response has the wrong header, ID, length, or payload"
                )
                continue
            decoded_events.append({**event, "kind": response_kind})
            continue

        if expected_stream_frame is not None and tuple(event["bytes"]) != expected_stream_frame:
            semantic_failures.append(
                "replay reconnect preflight stream frame is not the canonical clear row"
            )

        descriptor = payload[0] if len(payload) == 7 else -1
        row_index = descriptor >> 4 if descriptor >= 0 else -1
        row_count = descriptor & 0x0F if descriptor >= 0 else -1
        valid_descriptor = (row_index == 0 and row_count == 0) or (
            1 <= row_index <= row_count
        )
        if (
            destination != 0xD8
            or origin != 0xEA
            or packet_id != 0x43
            or len(payload) != 7
            or not valid_descriptor
        ):
            semantic_failures.append(
                "replay handshake first stream frame is not a valid broadcast alert row"
            )
            continue
        decoded_events.append({**event, "kind": "stream_started"})

    events_by_epoch: dict[int, list[dict[str, Any]]] = {}
    for event in decoded_events:
        events_by_epoch.setdefault(event["epoch"], []).append(event)

    complete_epoch: int | None = None
    start_request_counts: list[dict[str, int]] = []
    for epoch in sorted(epoch_ids):
        request_channel: str | None = None
        start_elapsed_ms: list[int] = []
        state = {
            "subscribe": False,
            "start_request": False,
            "stream_started": False,
            "version_request": False,
            "version_response": False,
            "all_volume_request": False,
            "all_volume_response": False,
        }
        for event in events_by_epoch.get(epoch, []):
            kind = event["kind"]
            if kind in {"start_request", "version_request", "all_volume_request"}:
                if request_channel is None:
                    request_channel = event["channel"]
                elif event["channel"] != request_channel:
                    semantic_failures.append(
                        "replay handshake epoch switches its selected command channel"
                    )
            if kind == "start_request":
                if not state["subscribe"]:
                    semantic_failures.append(
                        "replay handshake start request occurred before B2CE subscription"
                    )
                if ledger_schema == HANDSHAKE_LEDGER_LEGACY_SCHEMA:
                    if state["start_request"]:
                        semantic_failures.append(
                            "replay handshake epoch repeats start request"
                        )
                else:
                    elapsed_ms = int(event["elapsed_ms"])
                    if state["stream_started"]:
                        semantic_failures.append(
                            "replay handshake start retry occurred after stream delivery"
                        )
                    if len(start_elapsed_ms) >= MAX_HANDSHAKE_START_REQUESTS_PER_EPOCH:
                        semantic_failures.append(
                            "replay handshake epoch exceeds its bounded start request count"
                        )
                    if (
                        start_elapsed_ms
                        and elapsed_ms - start_elapsed_ms[-1]
                        < MIN_HANDSHAKE_START_RETRY_MS
                    ):
                        semantic_failures.append(
                            "replay handshake start retries are less than 1000 ms apart"
                        )
                    start_elapsed_ms.append(elapsed_ms)
                state["start_request"] = True
                continue
            if state[kind]:
                semantic_failures.append(
                    f"replay handshake epoch repeats {kind.replace('_', ' ')}"
                )
                continue
            if kind == "subscribe":
                state[kind] = True
            elif kind == "stream_started":
                if not state["start_request"]:
                    semantic_failures.append(
                        "replay handshake stream began before the accepted start request"
                    )
                state[kind] = True
            elif kind == "version_request":
                if not state["start_request"]:
                    semantic_failures.append(
                        "replay handshake version request occurred before the accepted start request"
                    )
                state[kind] = True
            elif kind == "version_response":
                if not state["version_request"]:
                    semantic_failures.append(
                        "replay handshake version response occurred before its request"
                    )
                state[kind] = True
            elif kind == "all_volume_request":
                if not state["version_request"]:
                    semantic_failures.append(
                        "replay handshake all-volume request occurred before version request"
                    )
                state[kind] = True
            elif kind == "all_volume_response":
                if not state["all_volume_request"]:
                    semantic_failures.append(
                        "replay handshake all-volume response occurred before its request"
                    )
                state[kind] = True

        start_request_counts.append(
            {
                "epoch": epoch,
                "count": (
                    len(start_elapsed_ms)
                    if ledger_schema == HANDSHAKE_LEDGER_SCHEMA
                    else int(state["start_request"])
                ),
            }
        )
        if all(state.values()):
            complete_epoch = epoch

    incomplete_failures: list[str] = []
    if complete_epoch is None:
        incomplete_failures.append(
            "replay handshake has no single epoch containing subscribe, start, stream, version, and all-volume delivery"
        )

    semantic_failures = list(dict.fromkeys(semantic_failures))
    failures = list(dict.fromkeys([*semantic_failures, *incomplete_failures]))
    handshake_state = (
        "invalid"
        if semantic_failures
        else "incomplete"
        if complete_epoch is None
        else "complete"
    )
    return {
        "result": "FAIL" if failures else "PASS",
        "schema_version": ledger_schema,
        "event_count": len(events),
        "epoch_count": len(epoch_ids),
        "complete_epoch": complete_epoch,
        "handshake_state": handshake_state,
        "start_request_counts": start_request_counts,
        "evidence": failures,
    }


def score_reconnect_epoch(label: str, checks: dict[str, Any]) -> dict[str, Any]:
    """Apply the strict one-epoch reconnect shape without changing the base scorer."""
    if checks.get("result") == "COLLECTION_FAILED":
        return dict(checks)

    failures = [str(item) for item in checks.get("evidence") or []]
    expected = {
        "epoch_count": 1,
        "complete_epoch": 1,
    }
    for field, value in expected.items():
        if checks.get(field) != value:
            failures.append(
                f"replay reconnect {label} {field}={checks.get(field)!r} expected={value}"
            )
    ledger_schema = checks.get("schema_version")
    maximum_event_count = (
        7
        if ledger_schema == HANDSHAKE_LEDGER_LEGACY_SCHEMA
        else 6 + MAX_HANDSHAKE_START_REQUESTS_PER_EPOCH
    )
    event_count = checks.get("event_count")
    if (
        not isinstance(event_count, int)
        or isinstance(event_count, bool)
        or not 7 <= event_count <= maximum_event_count
    ):
        expected_count = "7" if maximum_event_count == 7 else f"7..{maximum_event_count}"
        failures.append(
            f"replay reconnect {label} event_count={event_count!r} expected={expected_count}"
        )
    strict_invalid = (
        checks.get("epoch_count") not in {0, 1}
        or checks.get("complete_epoch") not in {None, 1}
        or (
            isinstance(event_count, int)
            and not isinstance(event_count, bool)
            and event_count > maximum_event_count
        )
    )
    handshake_state = str(checks.get("handshake_state") or "invalid")
    if strict_invalid:
        handshake_state = "invalid"
    failures = list(dict.fromkeys(failures))
    return {
        **checks,
        "result": "FAIL" if failures else "PASS",
        "handshake_state": handshake_state,
        "evidence": failures,
    }


def score_reconnect_lifecycle(
    raw: Any,
    *,
    failure_kind: str = "",
    legacy: bool = False,
) -> dict[str, Any]:
    """Score bounded disappearance evidence recorded by the live runner."""
    if raw is None:
        return handshake_collection_failure(
            "replay reconnect preflight terminal result is missing"
        )
    if not isinstance(raw, dict):
        return handshake_collection_failure(
            "replay reconnect preflight terminal result is not an object"
        )

    early_handshake_failure = failure_kind in {"handshake_timeout", "handshake_invalid"}
    boolean_expectations = {
        "handshake_ready_while_alive": not early_handshake_failure,
        "serial_fence_observed": True,
        "managed_stop": True,
        "confirmed_exit": True,
        "serial_session_continuous": True,
        "boot_observed_before_second_complete": False,
    }
    if not legacy:
        boolean_expectations["graceful_stop_confirmed"] = True
    missing = [field for field in boolean_expectations if field not in raw]
    if not legacy and "returncode" not in raw:
        missing.append("returncode")
    if "cleanup_marker_count" not in raw:
        missing.append("cleanup_marker_count")
    if missing:
        return handshake_collection_failure(
            "replay reconnect preflight terminal result is missing fields: "
            + ", ".join(sorted(missing))
        )

    malformed = [
        field
        for field in boolean_expectations
        if field in raw and not isinstance(raw[field], bool)
    ]
    cleanup_count = raw.get("cleanup_marker_count")
    if "cleanup_marker_count" in raw and (
        not isinstance(cleanup_count, int)
        or isinstance(cleanup_count, bool)
        or cleanup_count < 0
    ):
        malformed.append("cleanup_marker_count")
    returncode = raw.get("returncode")
    if not legacy and (
        not isinstance(returncode, int) or isinstance(returncode, bool)
    ):
        malformed.append("returncode")
    if malformed:
        return handshake_collection_failure(
            "replay reconnect preflight terminal result has invalid fields: "
            + ", ".join(sorted(malformed))
        )

    failures: list[str] = []
    for field, expected in boolean_expectations.items():
        if raw[field] is not expected:
            failures.append(
                f"replay reconnect preflight {field}={raw[field]!r} expected={expected!r}"
            )
    if not legacy and returncode != 0:
        failures.append(
            f"replay reconnect preflight returncode={returncode!r} expected=0"
        )
    diagnostics: list[str] = []
    if early_handshake_failure and cleanup_count == 0:
        diagnostics.append(
            "replay reconnect cleanup was not collected after the early handshake terminal"
        )
    elif cleanup_count != 1:
        failures.append(
            f"replay reconnect preflight cleanup_marker_count={cleanup_count!r} expected=1"
        )

    return {
        "result": "FAIL" if failures else "PASS",
        "evidence": failures,
        "diagnostics": diagnostics,
    }


def read_bounded_reconnect_log(
    path: Path,
    label: str,
    maximum_bytes: int,
    *,
    require_final_newline: bool = True,
) -> tuple[str | None, dict[str, Any] | None]:
    try:
        if path.stat().st_size > maximum_bytes:
            return None, handshake_collection_failure(
                f"replay reconnect {label} exceeds its bounded size"
            )
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        return None, handshake_collection_failure(
            f"replay reconnect {label} could not be read: {exc}"
        )
    if not text or (require_final_newline and not text.endswith("\n")):
        return None, handshake_collection_failure(
            f"replay reconnect {label} is empty or has an incomplete final line"
        )
    return text, None


def parse_reconnect_machine_events(text: str) -> tuple[list[tuple[int, dict[str, Any]]], str]:
    events: list[tuple[int, dict[str, Any]]] = []
    decoder = json.JSONDecoder()
    marker = "V1REPLAY_EVENT "
    for line_number, line in enumerate(text.splitlines(), start=1):
        offset = line.find(marker)
        if offset < 0:
            continue
        try:
            raw_event = line[offset + len(marker) :]
            event, end = decoder.raw_decode(raw_event)
        except (json.JSONDecodeError, TypeError):
            return [], f"preflight log line {line_number} has an invalid machine event"
        if raw_event[end:].strip():
            return [], f"preflight log line {line_number} has trailing machine-event data"
        if not isinstance(event, dict) or not isinstance(event.get("state"), str):
            return [], f"preflight log line {line_number} has an invalid machine event"
        events.append((line_number, event))
    return events, ""


def score_reconnect_raw_evidence(
    preflight_log_path: Path | None,
    preflight_log_error: str,
    serial_log_path: Path | None,
    serial_log_error: str,
    *,
    failure_kind: str = "",
    expected_duration_seconds: int | None = None,
    require_graceful_shutdown: bool = False,
) -> dict[str, Any]:
    """Independently reconstruct the managed-disappearance lifecycle from logs."""
    if preflight_log_path is None:
        return handshake_collection_failure(preflight_log_error)
    if serial_log_path is None:
        return handshake_collection_failure(serial_log_error)
    if preflight_log_path.name != "v1replay_reconnect_preflight.log":
        return handshake_collection_failure(
            "replay reconnect preflight log does not use its owned artifact name"
        )
    if serial_log_path.name != "bench_serial.log":
        return handshake_collection_failure(
            "replay reconnect serial log does not use its owned artifact name"
        )
    try:
        if preflight_log_path.samefile(serial_log_path):
            return handshake_collection_failure(
                "replay reconnect preflight and serial logs are not distinct artifacts"
            )
    except OSError as exc:
        return handshake_collection_failure(
            f"replay reconnect raw artifact identity could not be verified: {exc}"
        )
    preflight_text, read_error = read_bounded_reconnect_log(
        preflight_log_path,
        "preflight log",
        MAX_RECONNECT_PREFLIGHT_LOG_BYTES,
        require_final_newline=False,
    )
    if read_error is not None:
        return read_error
    serial_text, read_error = read_bounded_reconnect_log(
        serial_log_path,
        "serial log",
        MAX_BENCH_SERIAL_LOG_BYTES,
    )
    if read_error is not None:
        return read_error
    assert preflight_text is not None and serial_text is not None

    if not preflight_text.endswith("\n"):
        trailing_line = preflight_text.rsplit("\n", 1)[-1]
        if "V1REPLAY_EVENT " in trailing_line:
            return handshake_collection_failure(
                "replay reconnect preflight log has a truncated machine event"
            )
    machine_events, machine_error = parse_reconnect_machine_events(preflight_text)
    if machine_error:
        return handshake_collection_failure(f"replay reconnect {machine_error}")
    configured = [item for item in machine_events if item[1].get("state") == "configured"]
    transport = [item for item in machine_events if item[1].get("state") == "handshake_transport"]
    ready = [item for item in machine_events if item[1].get("state") == "handshake_ready"]
    console_diagnostics: list[str] = []
    if len(configured) != 1:
        console_diagnostics.append(
            "replay reconnect preflight log does not contain one configured machine event"
        )
    elif any(
        line_number < configured[0][0]
        for line_number, event in machine_events
        if event.get("state") in {"handshake_transport", "handshake_ready"}
    ):
        console_diagnostics.append(
            "replay reconnect preflight configured event follows readiness evidence"
        )
    if any(not isinstance(item[1].get("active"), bool) for item in transport):
        console_diagnostics.append(
            "replay reconnect preflight log has a malformed transport event"
        )
    if len(ready) > 1:
        console_diagnostics.append(
            "replay reconnect preflight log repeats its ready event"
        )
    if any(item[1].get("state") in {"replay_started", "complete"} for item in machine_events):
        return {
            "result": "FAIL",
            "evidence": ["replay reconnect preflight entered the scored scenario"],
        }

    # Readiness is owned by the strict, delivery-confirmed preflight ledger.
    # The console is published from multiple host threads, so positive ready
    # and transport events are diagnostics and their relative order is never
    # causal evidence. The terminal boolean transport state remains the sole
    # temporal witness of a live session lost before managed removal.
    expected_ready = failure_kind not in {
        "handshake_timeout",
        "handshake_invalid",
    }
    if expected_ready and not ready:
        console_diagnostics.append(
            "replay reconnect preflight log is missing its ready event"
        )
    if not transport:
        console_diagnostics.append(
            "replay reconnect preflight log is missing transport events"
        )
    elif ready:
        ready_line = ready[0][0]
        active_before_ready = any(
            line_number <= ready_line and event.get("active") is True
            for line_number, event in transport
        )
        if not active_before_ready:
            console_diagnostics.append(
                "replay reconnect positive transport evidence was published after readiness"
            )
    boolean_transport = [
        item for item in transport if isinstance(item[1].get("active"), bool)
    ]
    # Current managed children publish a serialized shutdown boundary before
    # removing their services:
    #
    #   stopping(sessionTransportActive=true) -> transport=false -> stopped
    #
    # The false event after that boundary is required teardown evidence, not a
    # live-session loss.  Legacy logs have no stopping snapshot, so retain the
    # historical terminal-state interpretation for them. Current artifacts
    # independently validate the complete raw stopping/stopped sequence below.
    stopping = [item for item in machine_events if item[1].get("state") == "stopping"]
    stopped = [item for item in machine_events if item[1].get("state") == "stopped"]
    transport_before_removal = boolean_transport
    graceful_shutdown = False
    if len(stopping) == 1 and len(stopped) == 1:
        stopping_line = stopping[0][0]
        stopped_line = stopped[0][0]
        teardown_transport = [
            item
            for item in machine_events
            if stopping_line < item[0] < stopped_line
            and item[1].get("state") == "session_transport"
        ]
        graceful_shutdown = (
            stopping[0][1].get("sessionTransportActive") is True
            and stopped_line == machine_events[-1][0]
            and len(teardown_transport) == 1
            and teardown_transport[0][1].get("active") is False
        )
        if graceful_shutdown:
            transport_before_removal = [
                item for item in boolean_transport if item[0] < stopping_line
            ]
    transport_lost = (
        bool(transport_before_removal)
        and transport_before_removal[-1][1].get("active") is False
    )
    malformed_graceful_shutdown = bool(stopping or stopped) and not graceful_shutdown
    if require_graceful_shutdown and not graceful_shutdown:
        return handshake_collection_failure(
            "replay reconnect preflight graceful shutdown evidence is missing or malformed"
        )

    transmission_failures: list[str] = []
    if failure_kind != "handshake_timeout":
        transmitted: list[tuple[str, tuple[int, ...]]] = []
        for line_number, line in enumerate(preflight_text.splitlines(), start=1):
            marker = line.find("TX ")
            if marker < 0:
                continue
            fields = line[marker + 3 :].strip().split()
            if len(fields) < 2:
                return handshake_collection_failure(
                    f"replay reconnect preflight log line {line_number} has malformed TX evidence"
                )
            channel = fields[0]
            try:
                raw = tuple(int(field, 16) for field in fields[1:])
            except ValueError:
                return handshake_collection_failure(
                    f"replay reconnect preflight log line {line_number} has malformed TX bytes"
                )
            if any(len(field) != 2 for field in fields[1:]):
                return handshake_collection_failure(
                    f"replay reconnect preflight log line {line_number} has malformed TX bytes"
                )
            transmitted.append((channel, raw))
        expected_transmissions = [
            ("B2CE", RECONNECT_VERSION_REPLY_FRAME),
            ("B2CE", RECONNECT_ALL_VOLUME_REPLY_FRAME),
            ("B2CE", RECONNECT_PREFLIGHT_CLEAR_FRAME),
        ]
        unmatched_transmissions = list(transmitted)
        missing_transmissions: list[tuple[str, tuple[int, ...]]] = []
        for expected in expected_transmissions:
            try:
                unmatched_transmissions.remove(expected)
            except ValueError:
                missing_transmissions.append(expected)
        if unmatched_transmissions:
            transmission_failures.append(
                "replay reconnect preflight did not stay quiet after its three bounded transmissions"
            )
        if missing_transmissions:
            console_diagnostics.append(
                "replay reconnect preflight console is missing ledger-confirmed TX telemetry"
            )

    lines = serial_text.splitlines()
    starts = [
        index for index, line in enumerate(lines)
        if line == RECONNECT_PREFLIGHT_START_BOUNDARY
    ]
    fence_begins = [index for index, line in enumerate(lines) if line == RECONNECT_FENCE_BEGIN]
    fence_completes = [
        index for index, line in enumerate(lines) if line == RECONNECT_FENCE_COMPLETE
    ]
    boundaries = [
        index for index, line in enumerate(lines) if line == RECONNECT_PROCESS_EXIT_BOUNDARY
    ]
    if len(starts) != 1 or len(boundaries) != 1:
        return handshake_collection_failure(
            "replay reconnect serial log is missing one preflight-start or process-exited boundary"
        )
    preflight_start = starts[0]
    boundary = boundaries[0]
    if boundary <= preflight_start:
        return handshake_collection_failure(
            "replay reconnect process-exited boundary precedes preflight start"
        )
    cleanup_indexes = [
        index for index, line in enumerate(lines) if line.startswith(V1_DISCONNECT_CLEANUP_PREFIX)
    ]
    qstart_indexes = [
        index for index, line in enumerate(lines)
        if index > preflight_start and line.startswith(">>> QSTART ")
    ]
    done_indexes: list[int] = []
    for index, line in enumerate(lines):
        if index <= preflight_start:
            continue
        if not line.startswith("QEVENT "):
            continue
        try:
            payload = json.loads(line[len("QEVENT ") :])
        except json.JSONDecodeError:
            return handshake_collection_failure(
                "replay reconnect serial log contains malformed QEVENT evidence"
            )
        if not isinstance(payload, dict):
            return handshake_collection_failure(
                "replay reconnect serial log contains malformed QEVENT evidence"
            )
        if payload.get("ok") is not True or payload.get("state") == "error":
            return handshake_collection_failure(
                "replay reconnect serial log contains a failed replacement QEVENT"
            )
        if payload.get("state") == "done":
            if not (
                payload.get("ok") is True
                and payload.get("suite") == "core"
                and payload.get("finalized") is True
            ):
                return handshake_collection_failure(
                    "replay reconnect serial log contains a mismatched replacement completion"
                )
            done_indexes.append(index)

    end = done_indexes[-1] if done_indexes else len(lines)
    def status_fence_result(start: int, stop: int) -> tuple[bool, str]:
        command_indexes = [
            index for index in range(start, stop) if lines[index] == ">>> QSTATUS"
        ]
        protocol_indexes = [
            index
            for index in range(start, stop)
            if lines[index].startswith(("QRESP ", "QERR "))
        ]
        if len(command_indexes) != 1 or len(protocol_indexes) != 1:
            return False, "replay reconnect serial fence requires one QSTATUS and one response"
        command_index = command_indexes[0]
        response_index = protocol_indexes[0]
        if response_index <= command_index:
            return False, "replay reconnect serial fence response precedes QSTATUS"
        if any(lines[index].startswith(">>> ") for index in range(start, stop) if index != command_index):
            return False, "replay reconnect serial fence contains an unexpected host command"
        line = lines[response_index]
        if line.startswith("QERR "):
            try:
                error = json.loads(line[len("QERR ") :])
            except json.JSONDecodeError:
                return False, "replay reconnect serial log contains malformed QERR evidence"
            if not isinstance(error, dict):
                return False, "replay reconnect serial log contains malformed QERR evidence"
            return False, "replay reconnect serial fence returned QERR"
        try:
            response = json.loads(line[len("QRESP ") :])
        except json.JSONDecodeError:
            return False, "replay reconnect serial log contains malformed QRESP evidence"
        if not isinstance(response, dict):
            return False, "replay reconnect serial log contains malformed QRESP evidence"
        if not (
            response.get("ok") is True
            and response.get("state") in {"idle", "done"}
            and response.get("suite") in {"core", "display"}
            and response.get("mode") in {"current", "proxy", "obd", "v1"}
        ):
            return False, "replay reconnect serial fence returned a non-ready QRESP"
        return True, ""

    barrier_commands = [
        index
        for index in range(preflight_start)
        if lines[index].startswith(">>> QBSC08")
    ]
    if len(barrier_commands) != 1:
        return handshake_collection_failure(
            "replay reconnect readiness requires exactly one QBSC08 nonce command"
        )
    barrier_command = barrier_commands[0]
    barrier_fields = lines[barrier_command].split()
    if (
        len(barrier_fields) != 3
        or barrier_fields[:2] != [">>>", "QBSC08"]
        or len(barrier_fields[2]) != 32
        or any(character not in "0123456789abcdef" for character in barrier_fields[2])
    ):
        return handshake_collection_failure(
            "replay reconnect readiness QBSC08 command has an invalid nonce"
        )
    barrier_nonce = barrier_fields[2]
    barrier_responses = [
        index
        for index in range(preflight_start)
        if lines[index].startswith("QBSC08 ")
    ]
    if len(barrier_responses) != 1:
        return handshake_collection_failure(
            "replay reconnect readiness requires exactly one QBSC08 nonce response"
        )
    barrier_response = barrier_responses[0]
    if barrier_response <= barrier_command:
        return handshake_collection_failure(
            "replay reconnect readiness QBSC08 response precedes its command"
        )
    try:
        barrier_payload = json.loads(lines[barrier_response][len("QBSC08 ") :])
    except json.JSONDecodeError:
        return handshake_collection_failure(
            "replay reconnect readiness QBSC08 response is malformed"
        )
    if not isinstance(barrier_payload, dict):
        return handshake_collection_failure(
            "replay reconnect readiness QBSC08 response is malformed"
        )
    barrier_schema = barrier_payload.get("schema")
    if (
        not isinstance(barrier_schema, int)
        or isinstance(barrier_schema, bool)
        or barrier_schema != 1
        or barrier_payload.get("nonce") != barrier_nonce
        or barrier_payload.get("status") not in {"ready", "busy"}
    ):
        return handshake_collection_failure(
            "replay reconnect readiness QBSC08 response does not match its command"
        )
    if any(
        lines[index].startswith(">>> ")
        for index in range(barrier_command + 1, barrier_response)
    ):
        return handshake_collection_failure(
            "replay reconnect readiness QBSC08 transaction contains an unexpected command"
        )
    for index in range(barrier_command + 1, barrier_response):
        line = lines[index]
        if line.startswith("QERR "):
            return handshake_collection_failure(
                "replay reconnect readiness QBSC08 transaction contains QERR"
            )
        if not line.startswith("QRESP "):
            continue
        try:
            delayed_response = json.loads(line[len("QRESP ") :])
        except json.JSONDecodeError:
            return handshake_collection_failure(
                "replay reconnect readiness contains malformed delayed QRESP evidence"
            )
        if not isinstance(delayed_response, dict):
            return handshake_collection_failure(
                "replay reconnect readiness contains malformed delayed QRESP evidence"
            )

    readiness_fence, readiness_fence_error = status_fence_result(
        barrier_response + 1,
        preflight_start,
    )
    if readiness_fence_error:
        return handshake_collection_failure(
            f"replay reconnect readiness {readiness_fence_error}"
        )
    if not readiness_fence:
        return handshake_collection_failure(
            "replay reconnect readiness is missing its final QSTATUS/QRESP fence"
        )

    if failure_kind not in {"handshake_timeout", "handshake_invalid"}:
        if len(fence_begins) != 1 or len(fence_completes) != 1:
            return handshake_collection_failure(
                "replay reconnect serial log is missing its pre-stop fence boundaries"
            )
        fence_begin = fence_begins[0]
        fence_complete = fence_completes[0]
        if not preflight_start < fence_begin < fence_complete < boundary:
            return handshake_collection_failure(
                "replay reconnect pre-stop fence does not precede managed process exit"
            )
        pre_stop_fence, fence_error = status_fence_result(fence_begin + 1, fence_complete)
        if fence_error:
            return handshake_collection_failure(fence_error)
        if not pre_stop_fence:
            return handshake_collection_failure(
                "replay reconnect serial log is missing its pre-stop QSTATUS/QRESP fence"
            )

    expected_pre_stop_commands: list[int] = []
    expected_pre_stop_responses: list[int] = []
    if (
        len(fence_begins) == 1
        and len(fence_completes) == 1
        and preflight_start < fence_begins[0] < fence_completes[0] < boundary
    ):
        expected_pre_stop_commands = [
            index
            for index in range(fence_begins[0] + 1, fence_completes[0])
            if lines[index] == ">>> QSTATUS"
        ]
        expected_pre_stop_responses = [
            index
            for index in range(fence_begins[0] + 1, fence_completes[0])
            if lines[index].startswith(("QRESP ", "QERR "))
        ]
    scoped_pre_stop_commands = [
        index
        for index in range(preflight_start + 1, boundary)
        if lines[index].startswith(">>> ")
    ]
    scoped_pre_stop_responses = [
        index
        for index in range(preflight_start + 1, boundary)
        if lines[index].startswith(("QRESP ", "QERR "))
    ]
    if (
        scoped_pre_stop_commands != expected_pre_stop_commands
        or scoped_pre_stop_responses != expected_pre_stop_responses
    ):
        return handshake_collection_failure(
            "replay reconnect serial log contains an unbounded pre-stop protocol exchange"
        )

    if any(line.startswith(BOOT_PREFIX) for line in lines[preflight_start : end + 1]):
        return handshake_collection_failure(
            "replay reconnect observed a board boot before replacement completion"
        )

    before_boundary = [
        index for index in cleanup_indexes if preflight_start < index < boundary
    ]
    after_boundary = [index for index in cleanup_indexes if index > boundary]

    if failure_kind:
        if qstart_indexes:
            return handshake_collection_failure(
                "replacement emulator window started after a failed reconnect preflight"
            )
        post_exit_fence, fence_error = status_fence_result(boundary + 1, len(lines))
        if fence_error:
            return handshake_collection_failure(fence_error)
        if not post_exit_fence:
            return handshake_collection_failure(
                "replay reconnect failure lacks a post-exit serial health fence"
            )
        observed_failure = {
            "handshake_timeout": True,
            "handshake_invalid": True,
            "active_session_lost": transport_lost,
            "cleanup_before_stop": bool(before_boundary),
            "cleanup_missing": not after_boundary,
            "cleanup_count": len(after_boundary) != 1,
        }.get(failure_kind, False)
        if not observed_failure:
            return handshake_collection_failure(
                "replay reconnect failure terminal disagrees with its raw lifecycle logs"
            )
        return {
            "result": "FAIL",
            "evidence": [
                f"replay reconnect raw lifecycle confirms {failure_kind}",
                *transmission_failures,
            ],
            "diagnostics": console_diagnostics,
        }

    if before_boundary:
        return {
            "result": "FAIL",
            "evidence": ["replay reconnect cleanup marker occurred before managed process exit"],
        }

    if not qstart_indexes or len(done_indexes) != 1:
        return handshake_collection_failure(
            "replay reconnect serial log requires QSTART and exactly one replacement completion"
        )
    done = done_indexes[0]
    if any(index >= done for index in qstart_indexes):
        return handshake_collection_failure(
            "replay reconnect serial log has QSTART outside its replacement window"
        )
    unexpected_commands = [
        lines[index]
        for index in range(qstart_indexes[0], done)
        if lines[index].startswith(">>> ") and not lines[index].startswith(">>> QSTART ")
    ]
    if unexpected_commands:
        return handshake_collection_failure(
            "replay reconnect replacement window contains an unexpected host command"
        )
    if expected_duration_seconds is None:
        return handshake_collection_failure(
            "replay reconnect window is missing its expected duration"
        )
    for qstart in qstart_indexes:
        parts = lines[qstart].split()
        if (
            len(parts) != 4
            or parts[:3] != [">>>", "QSTART", "core"]
            or not parts[3].isdigit()
            or int(parts[3]) != expected_duration_seconds
        ):
            return handshake_collection_failure(
                "replay reconnect serial log has the wrong replacement QSTART command"
            )

    running_ack_indexes: list[int] = []
    retry_error_indexes: list[int] = []
    unexpected_error_indexes: list[int] = []
    for index in range(qstart_indexes[0] + 1, done):
        prefix = ""
        if lines[index].startswith("QRESP "):
            prefix = "QRESP "
        elif lines[index].startswith("QERR "):
            prefix = "QERR "
        if not prefix:
            continue
        try:
            response = json.loads(lines[index][len(prefix) :])
        except json.JSONDecodeError:
            return handshake_collection_failure(
                "replay reconnect serial log contains malformed QSTART response evidence"
            )
        if not isinstance(response, dict):
            return handshake_collection_failure(
                "replay reconnect serial log contains malformed QSTART response evidence"
            )
        if prefix == "QRESP " and (
            response.get("ok") is True
            and response.get("state") == "running"
            and response.get("suite") == "core"
        ):
            running_ack_indexes.append(index)
        if prefix == "QERR ":
            if str(response.get("error") or response.get("message") or "") == "perf_sd_busy_retry":
                retry_error_indexes.append(index)
            else:
                unexpected_error_indexes.append(index)

    if unexpected_error_indexes:
        return handshake_collection_failure(
            "replay reconnect QSTART sequence contains an unexpected QERR"
        )

    if len(running_ack_indexes) != 1:
        return handshake_collection_failure(
            "replay reconnect serial log requires exactly one replacement running acknowledgement"
        )
    running_ack = running_ack_indexes[0]
    final_qstart = qstart_indexes[-1]
    if not final_qstart < running_ack < done:
        return handshake_collection_failure(
            "replay reconnect running acknowledgement does not follow the final QSTART"
        )
    for attempt, next_attempt in zip(qstart_indexes, qstart_indexes[1:]):
        retries = [index for index in retry_error_indexes if attempt < index < next_attempt]
        successes = [index for index in running_ack_indexes if attempt < index < next_attempt]
        if len(retries) != 1 or successes:
            return handshake_collection_failure(
                "replay reconnect repeated QSTART without one perf_sd_busy_retry response"
            )
    if any(index > final_qstart for index in retry_error_indexes):
        return handshake_collection_failure(
            "replay reconnect final QSTART retained a retry error"
        )
    qstart = qstart_indexes[0]
    cleanups_between = [index for index in after_boundary if index < qstart]
    cleanups_during = [index for index in after_boundary if qstart <= index <= done]
    failures: list[str] = list(transmission_failures)
    if malformed_graceful_shutdown and not failure_kind:
        failures.append(
            "replay reconnect preflight graceful shutdown evidence is malformed"
        )
    if transport_lost:
        failures.append(
            "replay reconnect preflight lost active transport before removal"
        )
    if len(cleanups_between) != 1:
        failures.append(
            "replay reconnect requires exactly one cleanup marker between process exit and QSTART"
        )
    if cleanups_during:
        failures.append("replacement V1 session disconnected before completion")
    post_cleanup_begins = [
        index for index, line in enumerate(lines) if line == RECONNECT_POST_CLEANUP_FENCE_BEGIN
    ]
    post_cleanup_completes = [
        index
        for index, line in enumerate(lines)
        if line == RECONNECT_POST_CLEANUP_FENCE_COMPLETE
    ]
    pre_qstart_begins = [
        index for index, line in enumerate(lines) if line == RECONNECT_PRE_QSTART_FENCE_BEGIN
    ]
    pre_qstart_completes = [
        index
        for index, line in enumerate(lines)
        if line == RECONNECT_PRE_QSTART_FENCE_COMPLETE
    ]
    if not all(
        len(items) == 1
        for items in (
            post_cleanup_begins,
            post_cleanup_completes,
            pre_qstart_begins,
            pre_qstart_completes,
        )
    ):
        return handshake_collection_failure(
            "replay reconnect serial log is missing its two bounded post-cleanup fences"
        )
    post_cleanup_begin = post_cleanup_begins[0]
    post_cleanup_complete = post_cleanup_completes[0]
    pre_qstart_begin = pre_qstart_begins[0]
    pre_qstart_complete = pre_qstart_completes[0]
    if not (
        boundary
        < post_cleanup_begin
        < post_cleanup_complete
        < pre_qstart_begin
        < pre_qstart_complete
        < qstart
    ):
        return handshake_collection_failure(
            "replay reconnect post-cleanup fences do not bracket camera admission and QSTART"
        )
    if len(cleanups_between) == 1 and cleanups_between[0] >= post_cleanup_begin:
        failures.append(
            "replay reconnect cleanup marker did not precede its post-cleanup fence"
        )
    for fence_begin, fence_complete in (
        (post_cleanup_begin, post_cleanup_complete),
        (pre_qstart_begin, pre_qstart_complete),
    ):
        fence_ok, fence_error = status_fence_result(fence_begin + 1, fence_complete)
        if fence_error:
            return handshake_collection_failure(fence_error)
        if not fence_ok:
            return handshake_collection_failure(
                "replay reconnect serial log is missing a bounded post-cleanup QSTATUS/QRESP fence"
            )
    pre_window_status, pre_window_status_error = status_fence_result(
        pre_qstart_complete + 1,
        qstart,
    )
    if pre_window_status_error:
        return handshake_collection_failure(pre_window_status_error)
    if not pre_window_status:
        return handshake_collection_failure(
            "replay reconnect serial log is missing its bounded pre-window QSTATUS/QRESP exchange"
        )
    scoped_commands = [
        index
        for index in range(boundary + 1, qstart)
        if lines[index].startswith(">>> ")
    ]
    scoped_responses = [
        index
        for index in range(boundary + 1, qstart)
        if lines[index].startswith(("QRESP ", "QERR "))
    ]
    expected_commands = [
        index
        for begin, complete in (
            (post_cleanup_begin, post_cleanup_complete),
            (pre_qstart_begin, pre_qstart_complete),
            (pre_qstart_complete, qstart),
        )
        for index in range(begin + 1, complete)
        if lines[index] == ">>> QSTATUS"
    ]
    expected_responses = [
        index
        for begin, complete in (
            (post_cleanup_begin, post_cleanup_complete),
            (pre_qstart_begin, pre_qstart_complete),
            (pre_qstart_complete, qstart),
        )
        for index in range(begin + 1, complete)
        if lines[index].startswith("QRESP ")
    ]
    if scoped_commands != expected_commands or scoped_responses != expected_responses:
        return handshake_collection_failure(
            "replay reconnect serial log contains an unbounded pre-QSTART protocol exchange"
        )
    return {
        "result": "FAIL" if failures else "PASS",
        "preflight_ready": True,
        "console_ready_observed": bool(ready),
        "cleanup_marker_count": len(after_boundary),
        "evidence": failures,
        "diagnostics": console_diagnostics,
    }


def score_replay_reconnect(
    primary_path: Path | None,
    primary_path_error: str,
    preflight_path: Path | None,
    preflight_path_error: str,
    raw_lifecycle: Any,
    primary_handshake_checks: dict[str, Any],
    raw_evidence_checks: dict[str, Any],
    *,
    legacy_lifecycle: bool = False,
) -> dict[str, Any]:
    """Score two handshake ledgers independently; never combine their events."""
    if preflight_path is None:
        preflight_checks = handshake_collection_failure(preflight_path_error)
    else:
        preflight_checks = score_replay_handshake_ledger(
            preflight_path,
            expected_stream_frame=RECONNECT_PREFLIGHT_CLEAR_FRAME,
        )

    if primary_path is None:
        primary_checks = handshake_collection_failure(primary_path_error)
    else:
        primary_checks = primary_handshake_checks

    path_checks: dict[str, Any] = {"result": "PASS", "evidence": []}
    if primary_path is not None and preflight_path is not None:
        try:
            shared_artifact = primary_path == preflight_path or primary_path.samefile(
                preflight_path
            )
        except OSError:
            path_checks = handshake_collection_failure(
                "replay reconnect ledger identity could not be verified"
            )
        else:
            if shared_artifact:
                path_checks = handshake_collection_failure(
                    "replay reconnect primary and preflight handshake ledgers are not distinct"
                )

    preflight_epoch_checks = score_reconnect_epoch("preflight", preflight_checks)
    primary_epoch_checks = score_reconnect_epoch("primary", primary_checks)
    lifecycle_checks = score_reconnect_lifecycle(
        raw_lifecycle,
        legacy=legacy_lifecycle,
    )
    result = "PASS"
    evidence: list[str] = []
    for checks in (
        path_checks,
        preflight_epoch_checks,
        primary_epoch_checks,
        lifecycle_checks,
        raw_evidence_checks,
    ):
        result = worse(result, str(checks["result"]))
        evidence.extend(str(item) for item in checks.get("evidence") or [])

    return {
        "result": result,
        "artifact_checks": path_checks,
        "preflight_handshake_checks": preflight_epoch_checks,
        "primary_handshake_checks": primary_epoch_checks,
        "lifecycle_checks": lifecycle_checks,
        "raw_evidence_checks": raw_evidence_checks,
        "evidence": list(dict.fromkeys(evidence)),
    }


def classify_reconnect_failure(window_dir: Path, suite: str, window: dict[str, Any]) -> dict[str, Any]:
    """Grade a bounded pre-QSTART reconnect failure without requiring metrics."""
    failure_kind = window.get("reconnect_failure_kind")
    allowed_failure_kinds = {
        "handshake_timeout",
        "handshake_invalid",
        "active_session_lost",
        "cleanup_before_stop",
        "cleanup_missing",
        "cleanup_count",
    }
    if suite != "replay" or failure_kind not in allowed_failure_kinds:
        return {
            "suite": suite,
            "result": "COLLECTION_FAILED",
            "artifact_dir": str(window_dir),
            "evidence": ["reconnect failure terminal has an invalid behavioral classification"],
        }

    raw_path = str(window.get("reconnect_preflight_handshake_ledger_path") or "").strip()
    preflight_checks: dict[str, Any]
    if not raw_path:
        preflight_checks = handshake_collection_failure(
            "reconnect failure terminal is missing its preflight handshake ledger path"
        )
    else:
        candidate = Path(raw_path)
        if not candidate.is_absolute():
            candidate = window_dir / candidate
        try:
            resolved = candidate.resolve()
            resolved.relative_to(window_dir.resolve())
        except (OSError, RuntimeError, ValueError):
            preflight_checks = handshake_collection_failure(
                "reconnect failure preflight handshake ledger must resolve inside its replay window"
            )
        else:
            if resolved.is_file():
                preflight_checks = score_reconnect_epoch(
                    "preflight",
                    score_replay_handshake_ledger(
                        resolved,
                        expected_stream_frame=RECONNECT_PREFLIGHT_CLEAR_FRAME,
                    ),
                )
            else:
                preflight_checks = handshake_collection_failure(
                    "reconnect failure preflight handshake ledger is missing"
                )

    if preflight_checks.get("result") != "COLLECTION_FAILED":
        if failure_kind == "handshake_timeout":
            structurally_incomplete = (
                preflight_checks.get("handshake_state") == "incomplete"
                and preflight_checks.get("epoch_count") in {0, 1}
            )
            if not structurally_incomplete:
                preflight_checks = handshake_collection_failure(
                    "reconnect handshake-timeout terminal contradicts its preflight ledger"
                )
        elif failure_kind == "handshake_invalid":
            if preflight_checks.get("handshake_state") != "invalid":
                preflight_checks = handshake_collection_failure(
                    "reconnect invalid-handshake terminal contradicts its preflight ledger"
                )
        elif preflight_checks.get("result") != "PASS":
            preflight_checks = handshake_collection_failure(
                "reconnect failure terminal requires a complete preflight handshake ledger"
            )

    lifecycle_checks = score_reconnect_lifecycle(
        window.get("reconnect_preflight"),
        failure_kind=str(failure_kind),
        legacy=window.get("schema_version") == 1,
    )
    preflight_log_path, preflight_log_error = same_window_artifact(
        window_dir,
        window.get("reconnect_preflight_log_path"),
        "reconnect preflight log",
    )
    serial_log_path, serial_log_error = same_window_artifact(
        window_dir,
        window.get("bench_serial_log_path"),
        "bench serial log",
    )
    raw_evidence_checks = score_reconnect_raw_evidence(
        preflight_log_path,
        preflight_log_error,
        serial_log_path,
        serial_log_error,
        failure_kind=str(failure_kind),
        require_graceful_shutdown=window.get("schema_version") != 1,
        expected_duration_seconds=(
            int(window["duration_seconds"])
            if isinstance(window.get("duration_seconds"), int)
            and not isinstance(window.get("duration_seconds"), bool)
            else None
        ),
    )
    terminal_checks = {
        "result": "FAIL",
        "failure_kind": failure_kind,
        "evidence": [str(window.get("error") or f"reconnect failed: {failure_kind}")],
    }
    reconnect_result = "FAIL"
    for checks in (preflight_checks, lifecycle_checks, raw_evidence_checks):
        reconnect_result = worse(reconnect_result, str(checks["result"]))
    reconnect_checks = {
        "result": reconnect_result,
        "preflight_handshake_checks": preflight_checks,
        "primary_handshake_checks": {
            "result": "NOT_RUN",
            "evidence": ["replacement emulator was not launched after reconnect preflight failure"],
        },
        "lifecycle_checks": lifecycle_checks,
        "raw_evidence_checks": raw_evidence_checks,
        "terminal_checks": terminal_checks,
        "evidence": list(
            dict.fromkeys(
                [
                    *(str(item) for item in preflight_checks.get("evidence") or []),
                    *(str(item) for item in lifecycle_checks.get("evidence") or []),
                    *(str(item) for item in raw_evidence_checks.get("evidence") or []),
                    *(str(item) for item in terminal_checks.get("evidence") or []),
                ]
            )
        ),
    }
    return {
        "suite": suite,
        "result": reconnect_result,
        "window_schema_version": window.get("schema_version"),
        "git_sha": window.get("git_sha", ""),
        "git_ref": window.get("git_ref", ""),
        "product_fingerprint": window.get("product_fingerprint", ""),
        "grader_fingerprint": window.get("grader_fingerprint", ""),
        "hardware_scoring_fingerprint": (
            window.get("hardware_scoring_fingerprint", "")
            if window.get("schema_version") == 3
            else ""
        ),
        "scenario_fingerprint": window.get("scenario_fingerprint", ""),
        "git_worktree_clean": window.get("git_worktree_clean") is True,
        "artifact_dir": str(window_dir),
        "replay_checks": {
            "result": reconnect_result,
            "reconnect_checks": reconnect_checks,
            "evidence": reconnect_checks["evidence"],
        },
        "camera": {},
        "budget_pressure": [],
        "evidence": reconnect_checks["evidence"],
    }


def encounter_collection_failure(message: str) -> dict[str, Any]:
    return {"result": "COLLECTION_FAILED", "evidence": [message]}


def encounter_semantic_failure(message: str) -> dict[str, Any]:
    return {"result": "FAIL", "evidence": [message]}


def encounter_row_signature(row: dict[str, Any]) -> tuple[Any, ...]:
    return (
        row["v1_index"],
        row["band"],
        row["frequency_mhz"],
        row["direction"],
        row["front_bars"],
        row["rear_bars"],
        row["priority"],
    )


def score_replay_encounter_csv(csv_path: Path) -> dict[str, Any]:
    try:
        with csv_path.open("r", encoding="utf-8", newline="") as handle:
            lines = [line for line in handle if line.strip() and not line.lstrip().startswith("#")]
        reader = csv.DictReader(lines)
        fieldnames = list(reader.fieldnames or [])
        if not fieldnames:
            return encounter_collection_failure("replay encounter CSV has no header")
        if len(fieldnames) != len(set(fieldnames)):
            return encounter_collection_failure("replay encounter CSV has duplicate columns")
        missing = sorted(set(ENCOUNTER_COLUMNS) - set(fieldnames))
        if missing:
            return encounter_collection_failure(
                "replay encounter CSV is missing required columns: " + ", ".join(missing)
            )

        parsed_rows: list[dict[str, Any]] = []
        for row_number, raw_row in enumerate(reader, start=2):
            if None in raw_row or any(
                raw_row.get(column) is None or not str(raw_row[column]).strip()
                for column in ENCOUNTER_COLUMNS
            ):
                return encounter_collection_failure(
                    f"replay encounter CSV row {row_number} is truncated or has empty required fields"
                )
            row: dict[str, Any] = {
                "event": str(raw_row["event"]).strip().upper(),
                "band": str(raw_row["band"]).strip(),
                "direction": str(raw_row["direction"]).strip().upper(),
            }
            try:
                for column in ENCOUNTER_INTEGER_COLUMNS:
                    row[column] = int(str(raw_row[column]).strip())
            except ValueError:
                return encounter_collection_failure(
                    f"replay encounter CSV row {row_number} has a non-integer numeric field"
                )
            if row["event"] not in {"START", "SAMPLE", "END"}:
                return encounter_collection_failure(
                    f"replay encounter CSV row {row_number} has an unknown event"
                )
            if row["encounter_id"] < 1 or row["sample_seq"] < 1:
                return encounter_collection_failure(
                    f"replay encounter CSV row {row_number} has an invalid encounter identity"
                )
            if row["alert_count"] < 0:
                return encounter_semantic_failure(
                    f"replay encounter CSV row {row_number} has a negative alert_count"
                )
            if row["priority"] not in {0, 1}:
                return encounter_semantic_failure(
                    f"replay encounter CSV row {row_number} has an invalid priority flag"
                )
            if row["dropped_snapshots"] < 0:
                return encounter_collection_failure(
                    f"replay encounter CSV row {row_number} has an invalid dropped-snapshot counter"
                )
            parsed_rows.append(row)
    except (OSError, UnicodeError, csv.Error) as exc:
        return encounter_collection_failure(f"replay encounter CSV could not be read: {exc}")

    if not parsed_rows:
        return encounter_collection_failure("replay encounter CSV contains no snapshot rows")

    groups: list[dict[str, Any]] = []
    seen_keys: set[tuple[int, int]] = set()
    last_sequence: dict[int, int] = {}
    current_key: tuple[int, int] | None = None
    for row in parsed_rows:
        key = (row["encounter_id"], row["sample_seq"])
        if key != current_key:
            if key in seen_keys:
                return encounter_collection_failure(
                    "replay encounter CSV has a non-contiguous snapshot group"
                )
            prior_sequence = last_sequence.get(row["encounter_id"], 0)
            if row["sample_seq"] <= prior_sequence:
                return encounter_collection_failure(
                    "replay encounter CSV sample_seq is not strictly increasing"
                )
            seen_keys.add(key)
            last_sequence[row["encounter_id"]] = row["sample_seq"]
            current_key = key
            groups.append(
                {
                    "encounter_id": row["encounter_id"],
                    "sample_seq": row["sample_seq"],
                    "rows": [],
                }
            )
        groups[-1]["rows"].append(row)

    for group in groups:
        rows = group["rows"]
        events = {row["event"] for row in rows}
        counts = {row["alert_count"] for row in rows}
        if len(events) != 1 or len(counts) != 1:
            return encounter_semantic_failure(
                "replay encounter snapshot rows disagree on event or alert_count"
            )
        event = next(iter(events))
        count = next(iter(counts))
        indices = [row["v1_index"] for row in rows]
        # The qualification logger persists an empty published table as one
        # END sentinel row. Non-empty tables remain one row per alert; their
        # own alert_count and ordered indices prove completeness without
        # coupling the evidence reader to a particular replay scenario size.
        if count == 0:
            if event != "END" or len(rows) != 1 or indices != [0] or rows[0]["priority"] != 0:
                return encounter_semantic_failure(
                    "replay encounter zero-alert snapshot requires one non-priority END sentinel with v1_index 0"
                )
        else:
            if len(rows) != count:
                return encounter_semantic_failure(
                    "replay encounter snapshot cardinality does not match alert_count"
                )
            if indices != list(range(1, count + 1)):
                return encounter_semantic_failure(
                    "replay encounter snapshot requires ordered unique one-based v1_index values"
                )
        if any(row["dropped_snapshots"] != 0 for row in rows):
            return encounter_collection_failure(
                "replay encounter snapshot reports dropped snapshots"
            )
        if event in {"START", "SAMPLE"} and sum(row["priority"] for row in rows) != 1:
            return encounter_semantic_failure(
                "replay encounter active snapshot requires exactly one priority row"
            )
        group["event"] = event
        group["signature"] = (
            () if count == 0 else tuple(encounter_row_signature(row) for row in rows)
        )

    groups_by_encounter: dict[int, list[dict[str, Any]]] = {}
    for group in groups:
        groups_by_encounter.setdefault(group["encounter_id"], []).append(group)

    complete_lifecycle: dict[str, Any] | None = None
    candidate_failures: list[tuple[int, str, int]] = []
    for encounter_id, encounter_groups in groups_by_encounter.items():
        for start_index, start_group in enumerate(encounter_groups):
            if start_group["event"] != "START":
                continue

            expected_index = 0
            matched_group_indices: list[int] = []
            candidate_failure = ""
            for group_index in range(start_index, len(encounter_groups)):
                group = encounter_groups[group_index]
                event = group["event"]
                signature = group["signature"]

                if event == "END":
                    if expected_index == len(EXPECTED_REPLAY_CHECKPOINTS):
                        final_signature = EXPECTED_REPLAY_CHECKPOINTS[-1][1]
                        # Schema-2 qualification evidence closes a lifecycle
                        # with the parser-published empty table. Retain the
                        # legacy repeated-final-table form for older artifacts.
                        if signature not in {(), final_signature}:
                            candidate_failure = (
                                "replay encounter END does not close the final authored one-row state"
                            )
                        else:
                            complete_lifecycle = {
                                "encounter_id": encounter_id,
                                "start_group_index": start_index,
                                "checkpoint_group_indices": matched_group_indices,
                                "end_group_index": group_index,
                            }
                    elif expected_index > 0:
                        missing_label = EXPECTED_REPLAY_CHECKPOINTS[expected_index][0]
                        candidate_failure = (
                            "replay encounter ended before authored checkpoint: " + missing_label
                        )
                    break

                if event == "START" and group_index != start_index:
                    candidate_failure = "replay encounter has a second START before END"
                    break

                if expected_index == 0:
                    if signature == EXPECTED_REPLAY_CHECKPOINTS[0][1]:
                        matched_group_indices.append(group_index)
                        expected_index = 1
                    elif len(signature) == len(EXPECTED_REPLAY_CHECKPOINTS[0][1]):
                        candidate_failure = (
                            "replay encounter first two-row state does not match the authored handoff"
                        )
                        break
                    continue

                current_signature = EXPECTED_REPLAY_CHECKPOINTS[expected_index - 1][1]
                if signature == current_signature:
                    continue
                if (
                    expected_index < len(EXPECTED_REPLAY_CHECKPOINTS)
                    and signature == EXPECTED_REPLAY_CHECKPOINTS[expected_index][1]
                ):
                    matched_group_indices.append(group_index)
                    expected_index += 1
                    continue

                candidate_failure = (
                    "replay encounter has an unexpected or regressed active state after authored checkpoint: "
                    + EXPECTED_REPLAY_CHECKPOINTS[expected_index - 1][0]
                )
                break

            if complete_lifecycle is not None:
                break
            if expected_index > 0 or candidate_failure:
                if not candidate_failure:
                    if expected_index == len(EXPECTED_REPLAY_CHECKPOINTS):
                        candidate_failure = (
                            "replay encounter CSV has no END after the authored active sequence"
                        )
                    else:
                        missing_label = EXPECTED_REPLAY_CHECKPOINTS[expected_index][0]
                        candidate_failure = (
                            "replay encounter CSV is missing authored checkpoint: " + missing_label
                        )
                candidate_failures.append((expected_index, candidate_failure, encounter_id))
        if complete_lifecycle is not None:
            break

    failures: list[str] = []
    matched_checkpoints = 0
    if complete_lifecycle is not None:
        matched_checkpoints = len(EXPECTED_REPLAY_CHECKPOINTS)
    elif candidate_failures:
        matched_checkpoints, failure, _encounter_id = max(
            candidate_failures,
            key=lambda item: item[0],
        )
        failures.append(failure)
    else:
        failures.append(
            "replay encounter CSV has no START-led encounter containing the authored checkpoint sequence"
        )

    return {
        "result": "FAIL" if failures else "PASS",
        "encounter_count": len({group["encounter_id"] for group in groups}),
        "snapshot_group_count": len(groups),
        "matched_checkpoints": matched_checkpoints,
        "expected_checkpoints": len(EXPECTED_REPLAY_CHECKPOINTS),
        "lifecycle_encounter_id": (
            complete_lifecycle["encounter_id"] if complete_lifecycle is not None else None
        ),
        "closure_found": complete_lifecycle is not None,
        "evidence": failures,
    }


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

    # The reconnect replay has two owned metric sessions after preflight: QSTART
    # captures a baseline, then the replacement V1 connection opens the final
    # session. Boot/preflight sessions contain the deliberately induced removal
    # and must not contribute to replacement-session deltas.
    if len(nonempty_sessions) < 2:
        return {
            "result": "COLLECTION_FAILED",
            "evidence": [
                "replay CSV requires nonempty QSTART and replacement-connection sessions"
            ],
        }
    selected_sessions = nonempty_sessions[-2:]
    selected_meta = [meta for _index, meta, _rows in selected_sessions]
    if any(meta is None for meta in selected_meta):
        return {
            "result": "COLLECTION_FAILED",
            "evidence": ["replay CSV replacement window is missing session markers"],
        }
    qstart_meta, replacement_meta = selected_meta
    assert qstart_meta is not None and replacement_meta is not None
    if (
        qstart_meta.bootId <= 0
        or replacement_meta.bootId != qstart_meta.bootId
        or replacement_meta.schema != qstart_meta.schema
        or qstart_meta.seq <= 0
        or replacement_meta.seq != qstart_meta.seq + 1
        or replacement_meta.uptime_ms <= qstart_meta.uptime_ms
        or not qstart_meta.token
        or not replacement_meta.token
        or replacement_meta.token == qstart_meta.token
    ):
        return {
            "result": "COLLECTION_FAILED",
            "evidence": [
                "replay CSV QSTART and replacement sessions have invalid or discontinuous metadata"
            ],
        }
    if qstart_meta.schema < REPLAY_ALL_VOLUME_MIN_SCHEMA:
        message = (
            "replay CSV all-volume consumption evidence requires session schema "
            f">={REPLAY_ALL_VOLUME_MIN_SCHEMA}"
        )
        return {
            "result": "COLLECTION_FAILED",
            "all_volume_consumption": {
                "result": "COLLECTION_FAILED",
                "counter": REPLAY_ALL_VOLUME_COUNTER,
                "minimum_schema": REPLAY_ALL_VOLUME_MIN_SCHEMA,
                "selected_schema": qstart_meta.schema,
                "evidence": [message],
            },
            "evidence": [message],
        }
    if any("millis" not in row for _index, _meta, session_rows in selected_sessions for row in session_rows):
        return {
            "result": "COLLECTION_FAILED",
            "evidence": ["replay CSV replacement window is missing a millis anchor"],
        }
    for (_index, meta, session_rows) in selected_sessions:
        assert meta is not None
        millis_values = [int(row["millis"]) for row in session_rows]
        if any(value < meta.uptime_ms for value in millis_values) or any(
            later < earlier for earlier, later in zip(millis_values, millis_values[1:])
        ):
            return {
                "result": "COLLECTION_FAILED",
                "evidence": [
                    "replay CSV replacement window has invalid or regressed row timing"
                ],
            }
    qstart_rows = selected_sessions[0][2]
    if max(int(row["millis"]) for row in qstart_rows) >= replacement_meta.uptime_ms:
        return {
            "result": "COLLECTION_FAILED",
            "evidence": [
                "replay CSV replacement marker does not follow the QSTART baseline session"
            ],
        }
    try:
        csv_lines = csv_path.read_text(encoding="utf-8").splitlines()
        marker_indices = [
            index
            for index, line in enumerate(csv_lines)
            if line.startswith("#session_start")
        ]
        selected_marker_indices = marker_indices[-2:]
        if len(selected_marker_indices) != 2:
            raise ValueError("selected session markers are missing")
        selected_marker_seqs = []
        for marker_index in selected_marker_indices:
            if marker_index == 0 or not csv_lines[marker_index - 1].startswith("millis,"):
                raise ValueError("session marker does not follow its CSV header")
            marker_values = {
                key.strip(): value.strip()
                for field in csv_lines[marker_index].split(",")
                if "=" in field
                for key, value in [field.split("=", 1)]
            }
            selected_marker_seqs.append(int(marker_values.get("seq", "0")))
        if selected_marker_seqs != [qstart_meta.seq, replacement_meta.seq]:
            raise ValueError("selected session markers do not match parsed sessions")
        final_marker_index = selected_marker_indices[-1]
        final_marker = csv_lines[final_marker_index]
        final_marker_fields = {
            key.strip(): value.strip()
            for field in final_marker.split(",")
            if "=" in field
            for key, value in [field.split("=", 1)]
        }
        final_marker_seq = int(final_marker_fields.get("seq", "0"))
    except (OSError, StopIteration, ValueError):
        return {
            "result": "COLLECTION_FAILED",
            "evidence": ["replay CSV final replacement session marker is missing or invalid"],
        }
    try:
        selected_raw_session_ends = [selected_marker_indices[1] - 1, len(csv_lines)]
        for marker_index, session_end in zip(
            selected_marker_indices, selected_raw_session_ends
        ):
            header_fields = csv_lines[marker_index - 1].split(",")
            if header_fields.count(REPLAY_ALL_VOLUME_COUNTER) > 1:
                message = (
                    "replay CSV selected session header repeats required column: "
                    + REPLAY_ALL_VOLUME_COUNTER
                )
                return {
                    "result": "COLLECTION_FAILED",
                    "all_volume_consumption": {
                        "result": "COLLECTION_FAILED",
                        "counter": REPLAY_ALL_VOLUME_COUNTER,
                        "minimum_schema": REPLAY_ALL_VOLUME_MIN_SCHEMA,
                        "selected_schema": qstart_meta.schema,
                        "evidence": [message],
                    },
                    "evidence": [message],
                }
            if REPLAY_ALL_VOLUME_COUNTER not in header_fields:
                continue
            counter_index = header_fields.index(REPLAY_ALL_VOLUME_COUNTER)
            for line in csv_lines[marker_index + 1 : session_end]:
                if not line.strip() or line.startswith("#") or line.startswith("millis,"):
                    continue
                values = line.split(",")
                raw_value = values[counter_index].strip()
                if (
                    not raw_value.isascii()
                    or not raw_value.isdigit()
                    or int(raw_value) > 0xFFFFFFFF
                ):
                    raise ValueError
    except (IndexError, ValueError):
        message = "replay CSV all-volume consumption counter is not an unsigned integer"
        return {
            "result": "COLLECTION_FAILED",
            "all_volume_consumption": {
                "result": "COLLECTION_FAILED",
                "counter": REPLAY_ALL_VOLUME_COUNTER,
                "minimum_schema": REPLAY_ALL_VOLUME_MIN_SCHEMA,
                "selected_schema": qstart_meta.schema,
                "evidence": [message],
            },
            "evidence": [message],
        }
    final_marker_has_rows = any(
        line.strip()
        and not line.startswith("#")
        and not line.startswith("millis,")
        for line in csv_lines[final_marker_index + 1 :]
    )
    if final_marker_seq != replacement_meta.seq or not final_marker_has_rows:
        return {
            "result": "COLLECTION_FAILED",
            "evidence": ["replay CSV final replacement session is missing or empty"],
        }
    rows = [row for _index, _meta, session_rows in selected_sessions for row in session_rows]
    session_indices = [index for index, _meta, _rows in selected_sessions]

    required = set(exact) | set(zero) | {REPLAY_ALL_VOLUME_COUNTER}
    missing = sorted(
        {
            column
            for row in rows
            for column in required
            if column not in row
        }
    )
    if missing:
        message = "replay CSV is missing required columns: " + ", ".join(missing)
        result: dict[str, Any] = {
            "result": "COLLECTION_FAILED",
            "evidence": [message],
        }
        if REPLAY_ALL_VOLUME_COUNTER in missing:
            result["all_volume_consumption"] = {
                "result": "COLLECTION_FAILED",
                "counter": REPLAY_ALL_VOLUME_COUNTER,
                "minimum_schema": REPLAY_ALL_VOLUME_MIN_SCHEMA,
                "selected_schema": qstart_meta.schema,
                "evidence": [message],
            }
        return result

    if any(int(row[REPLAY_ALL_VOLUME_COUNTER]) < 0 for row in rows):
        message = "replay CSV all-volume consumption counter is not an unsigned integer"
        return {
            "result": "COLLECTION_FAILED",
            "all_volume_consumption": {
                "result": "COLLECTION_FAILED",
                "counter": REPLAY_ALL_VOLUME_COUNTER,
                "minimum_schema": REPLAY_ALL_VOLUME_MIN_SCHEMA,
                "selected_schema": qstart_meta.schema,
                "evidence": [message],
            },
            "evidence": [message],
        }

    regressed = sorted(
        column
        for column in required
        if any(
            int(later[column]) < int(earlier[column])
            for earlier, later in zip(rows, rows[1:])
        )
    )
    if regressed:
        message = (
            "replay CSV cumulative counters regress inside the replacement window: "
            + ", ".join(regressed)
        )
        result = {
            "result": "COLLECTION_FAILED",
            "evidence": [message],
        }
        if REPLAY_ALL_VOLUME_COUNTER in regressed:
            result["all_volume_consumption"] = {
                "result": "COLLECTION_FAILED",
                "counter": REPLAY_ALL_VOLUME_COUNTER,
                "minimum_schema": REPLAY_ALL_VOLUME_MIN_SCHEMA,
                "selected_schema": qstart_meta.schema,
                "evidence": [message],
            }
        return result

    observed = {column: counter_delta(rows, column) for column in required}
    failures: list[str] = []
    for column, expected in exact.items():
        if observed[column] != expected:
            failures.append(f"{column} delta={observed[column]} expected={expected}")
    for column in zero:
        if observed[column] != 0:
            failures.append(f"{column} delta={observed[column]} expected=0")
    all_volume_failures: list[str] = []
    if observed[REPLAY_ALL_VOLUME_COUNTER] != 1:
        all_volume_failures.append(
            f"{REPLAY_ALL_VOLUME_COUNTER} delta={observed[REPLAY_ALL_VOLUME_COUNTER]} expected=1"
        )
    failures.extend(all_volume_failures)
    return {
        "result": "FAIL" if failures else "PASS",
        "segment_scope": "replacement_window",
        "session_count": len(selected_sessions),
        "total_nonempty_session_count": len(nonempty_sessions),
        "session_indices": session_indices,
        "session_index": session_indices[-1],
        "row_count": len(rows),
        "observed_deltas": {column: observed[column] for column in sorted(observed)},
        "all_volume_consumption": {
            "result": "FAIL" if all_volume_failures else "PASS",
            "counter": REPLAY_ALL_VOLUME_COUNTER,
            "minimum_schema": REPLAY_ALL_VOLUME_MIN_SCHEMA,
            "selected_schema": qstart_meta.schema,
            "qstart_value": int(rows[0][REPLAY_ALL_VOLUME_COUNTER]),
            "replacement_value": int(rows[-1][REPLAY_ALL_VOLUME_COUNTER]),
            "observed_delta": observed[REPLAY_ALL_VOLUME_COUNTER],
            "evidence": all_volume_failures,
        },
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
    window_schema = window.get("schema_version")
    if (
        not isinstance(window_schema, int)
        or isinstance(window_schema, bool)
        or window_schema not in {1, 2, 3}
    ):
        return {
            "suite": suite,
            "result": "COLLECTION_FAILED",
            "artifact_dir": str(window_dir),
            "evidence": [
                f"window result schema_version={window_schema!r} is not supported"
            ],
        }
    if window.get("result") == "RECONNECT_FAILED":
        return classify_reconnect_failure(window_dir, suite, window)
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
        failure_stage = str(window.get("camera_failure_stage") or "preflight")
        preflight_name = Path(str(window_camera.get("preflight") or "camera_preflight.json")).name
        preflight = load_json(window_dir / "camera" / preflight_name) or {}
        evidence: list[str] = []
        diagnostics: list[Any] = []
        recorder_failure = (
            window_camera.get("recorder_failure")
            if isinstance(window_camera.get("recorder_failure"), dict)
            else {}
        )
        if failure_stage == "recording":
            code = str(recorder_failure.get("code") or window.get("camera_failure_kind") or "failed")
            message = str(recorder_failure.get("message") or window.get("error") or "camera recorder failed")
            recorder_error = (
                recorder_failure.get("error")
                if isinstance(recorder_failure.get("error"), dict)
                else {}
            )
            error_identity = ""
            if isinstance(recorder_error.get("domain"), str) and isinstance(
                recorder_error.get("code"), int
            ):
                error_identity = f"; {recorder_error['domain']} {recorder_error['code']}"
            underlying_error = (
                recorder_error.get("underlying")
                if isinstance(recorder_error.get("underlying"), dict)
                else {}
            )
            if isinstance(underlying_error.get("domain"), str) and isinstance(
                underlying_error.get("code"), int
            ):
                error_identity += (
                    f"; underlying {underlying_error['domain']} "
                    f"{underlying_error['code']}"
                )
            evidence.append(f"camera recorder {code}: {message}{error_identity}")
        else:
            diagnostics = (
                preflight.get("diagnostics")
                if isinstance(preflight.get("diagnostics"), list)
                else window_camera.get("preflight_diagnostics") or []
            )
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
            "collection_status": (
                "INCOMPLETE" if failure_stage == "recording" else "NOT_STARTED"
            ),
            "window_schema_version": window_schema,
            "git_sha": window.get("git_sha", ""),
            "git_ref": window.get("git_ref", ""),
            "product_fingerprint": window.get("product_fingerprint", ""),
            "grader_fingerprint": window.get("grader_fingerprint", ""),
            "hardware_scoring_fingerprint": (
                window.get("hardware_scoring_fingerprint", "") if window_schema == 3 else ""
            ),
            "scenario_fingerprint": window.get("scenario_fingerprint", ""),
            "git_worktree_clean": window.get("git_worktree_clean") is True,
            "artifact_dir": str(window_dir),
            "camera": {
                "result": "INCONCLUSIVE",
                "capture_result": window_camera.get("result", "CAPTURE_FAILED"),
                "diagnostics": diagnostics,
                "preflight": preflight_name,
                "failure_stage": failure_stage,
                "recorder_failure": recorder_failure,
                "errors": window_camera.get("errors", []),
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
    manifest_path = window_path(window_dir, window.get("manifest_path"), "manifest.json")
    manifest = load_json(manifest_path)
    if manifest is None:
        return {
            "suite": suite,
            "result": "COLLECTION_FAILED",
            "artifact_dir": str(window_dir),
            "evidence": [f"missing or invalid performance manifest: {manifest_path}"],
        }
    if window_schema == 3:
        scoring_manifest = (
            scoring.get("manifest") if isinstance(scoring.get("manifest"), dict) else {}
        )
        hardware_scoring_values = {
            "window result": window.get("hardware_scoring_fingerprint"),
            "performance manifest": manifest.get("hardware_scoring_fingerprint"),
            "scoring manifest": scoring_manifest.get("hardware_scoring_fingerprint"),
        }
        normalized = {label: str(value or "") for label, value in hardware_scoring_values.items()}
        valid = all(valid_digest(value) for value in normalized.values())
        if not valid or len(set(normalized.values())) != 1:
            return {
                "suite": suite,
                "result": "COLLECTION_FAILED",
                "window_schema_version": window_schema,
                "git_sha": window.get("git_sha", ""),
                "git_ref": window.get("git_ref", ""),
                "product_fingerprint": window.get("product_fingerprint", ""),
                "grader_fingerprint": window.get("grader_fingerprint", ""),
                "hardware_scoring_fingerprint": "",
                "scenario_fingerprint": window.get("scenario_fingerprint", ""),
                "git_worktree_clean": window.get("git_worktree_clean") is True,
                "artifact_dir": str(window_dir),
                "evidence": [
                    "current window hardware-scoring identity is missing or inconsistent "
                    "across the window, performance manifest, and scoring manifest"
                ],
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
    raw_emulator = window.get("v1_emulator")
    emulator = raw_emulator if isinstance(raw_emulator, dict) else {}
    completion = window.get("completion") if isinstance(window.get("completion"), dict) else {}
    live_window = completion.get("source") != "from_csv"
    if window_schema == 1:
        if emulator and emulator.get("completed") is not True:
            result = "COLLECTION_FAILED"
            evidence.append("managed V1 emulator did not cover the complete metrics window")
    elif not live_window:
        pass
    elif not isinstance(raw_emulator, dict):
        result = "COLLECTION_FAILED"
        evidence.append("managed V1 emulator evidence is missing or invalid")
    else:
        expected_mode = "bench" if suite == "replay" else "idle"
        if emulator.get("mode") != expected_mode:
            result = "COLLECTION_FAILED"
            evidence.append(
                f"managed V1 emulator mode={emulator.get('mode')!r} expected={expected_mode!r}"
            )
        completed = emulator.get("completed")
        if not isinstance(completed, bool):
            result = "COLLECTION_FAILED"
            evidence.append("managed V1 emulator completion evidence is missing or invalid")
        elif not completed:
            result = "COLLECTION_FAILED"
            evidence.append("managed V1 emulator did not cover the complete metrics window")
        managed_stop = emulator.get("managed_stop")
        if not isinstance(managed_stop, bool):
            result = "COLLECTION_FAILED"
            evidence.append("managed V1 emulator stop evidence is missing or invalid")
        elif not managed_stop:
            result = "COLLECTION_FAILED"
            evidence.append("managed V1 emulator was not stopped by the runner")
        returncode = emulator.get("returncode")
        if (
            not isinstance(returncode, int)
            or isinstance(returncode, bool)
        ):
            result = "COLLECTION_FAILED"
            evidence.append("managed V1 emulator return code evidence is missing or invalid")
        elif returncode != 0:
            result = "COLLECTION_FAILED"
            evidence.append(
                f"managed V1 emulator graceful teardown exited with code {returncode}"
            )
        graceful_stop = emulator.get("graceful_stop_confirmed")
        if not isinstance(graceful_stop, bool):
            result = "COLLECTION_FAILED"
            evidence.append("managed V1 emulator graceful stop evidence is missing or invalid")
        elif not graceful_stop:
            result = "COLLECTION_FAILED"
            evidence.append("managed V1 emulator did not confirm graceful stop")
        if emulator.get("mode") == "idle":
            session_owned = emulator.get("session_transport_owned")
            if not isinstance(session_owned, bool):
                result = "COLLECTION_FAILED"
                evidence.append("idle V1 emulator session ownership evidence is missing or invalid")
            elif not session_owned:
                result = "COLLECTION_FAILED"
                evidence.append("idle V1 emulator did not own the final session transport")
            session_continuous = emulator.get("session_transport_continuous")
            if not isinstance(session_continuous, bool):
                result = "COLLECTION_FAILED"
                evidence.append(
                    "idle V1 emulator transport continuity evidence is missing or invalid"
                )
            elif not session_continuous:
                result = "COLLECTION_FAILED"
                evidence.append("idle V1 emulator lost session transport during the window")
    if suite == "replay":
        replay_process = window.get("replay") if isinstance(window.get("replay"), dict) else {}
        if replay_process.get("completed") is not True:
            result = "COLLECTION_FAILED"
            evidence.append("v1replay did not complete successfully")
        csv_path = window_path(window_dir, window.get("csv_path"), "perf.csv")
        metric_checks = score_replay_csv(csv_path, str(window.get("segment") or "last"))
        encounter_path, encounter_path_error = same_window_artifact(
            window_dir,
            window.get("encounter_csv_path"),
            "encounter CSV",
        )
        encounter_checks = (
            score_replay_encounter_csv(encounter_path)
            if encounter_path is not None
            else encounter_collection_failure(encounter_path_error)
        )
        handshake_path, handshake_path_error = same_window_artifact(
            window_dir,
            window.get("handshake_ledger_path"),
            "handshake ledger",
        )
        handshake_checks = (
            score_replay_handshake_ledger(handshake_path)
            if handshake_path is not None
            else handshake_collection_failure(handshake_path_error)
        )
        reconnect_preflight_log_path, reconnect_preflight_log_error = same_window_artifact(
            window_dir,
            window.get("reconnect_preflight_log_path"),
            "reconnect preflight log",
        )
        serial_log_path, serial_log_error = same_window_artifact(
            window_dir,
            window.get("bench_serial_log_path"),
            "bench serial log",
        )
        reconnect_raw_checks = score_reconnect_raw_evidence(
            reconnect_preflight_log_path,
            reconnect_preflight_log_error,
            serial_log_path,
            serial_log_error,
            require_graceful_shutdown=window.get("schema_version") != 1,
            expected_duration_seconds=(
                int(window["duration_seconds"])
                if isinstance(window.get("duration_seconds"), int)
                and not isinstance(window.get("duration_seconds"), bool)
                else None
            ),
        )
        reconnect_preflight_path, reconnect_preflight_path_error = same_window_artifact(
            window_dir,
            window.get("reconnect_preflight_handshake_ledger_path"),
            "reconnect preflight handshake ledger",
        )
        reconnect_checks = score_replay_reconnect(
            handshake_path,
            handshake_path_error,
            reconnect_preflight_path,
            reconnect_preflight_path_error,
            window.get("reconnect_preflight"),
            handshake_checks,
            reconnect_raw_checks,
            legacy_lifecycle=window.get("schema_version") == 1,
        )
        replay_checks = {
            **metric_checks,
            "result": worse(
                worse(
                    worse(str(metric_checks["result"]), str(encounter_checks["result"])),
                    str(handshake_checks["result"]),
                ),
                str(reconnect_checks["result"]),
            ),
            "metrics_result": metric_checks["result"],
            "encounter_checks": encounter_checks,
            "handshake_checks": handshake_checks,
            "reconnect_checks": reconnect_checks,
            "evidence": [
                *(str(item) for item in metric_checks.get("evidence") or []),
                *(str(item) for item in encounter_checks.get("evidence") or []),
                *(str(item) for item in handshake_checks.get("evidence") or []),
                *(str(item) for item in reconnect_checks.get("evidence") or []),
            ],
        }
        result = worse(result, str(replay_checks["result"]))
        evidence.extend(str(item) for item in replay_checks.get("evidence") or [])

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
        "window_schema_version": window_schema,
        "git_sha": manifest.get("git_sha", ""),
        "git_ref": manifest.get("git_ref", ""),
        "product_fingerprint": manifest.get("product_fingerprint", ""),
        "grader_fingerprint": manifest.get("grader_fingerprint", ""),
        "hardware_scoring_fingerprint": (
            manifest.get("hardware_scoring_fingerprint", "") if window_schema == 3 else ""
        ),
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
            "session_transport_owned": (
                emulator.get("session_transport_owned")
                if isinstance(emulator.get("session_transport_owned"), bool)
                else None
            ),
            "session_transport_continuous": (
                emulator.get("session_transport_continuous")
                if isinstance(emulator.get("session_transport_continuous"), bool)
                else None
            ),
            "graceful_stop_confirmed": emulator.get("graceful_stop_confirmed") is True,
            "returncode": (
                emulator.get("returncode")
                if isinstance(emulator.get("returncode"), int)
                and not isinstance(emulator.get("returncode"), bool)
                else None
            ),
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
    collection_incomplete = any(
        window.get("collection_status") == "INCOMPLETE" for window in payload["windows"]
    )
    collection_not_started = any(
        window.get("collection_status") == "NOT_STARTED" for window in payload["windows"]
    )
    if collection_failed:
        lines.append("collection: FAIL")
    elif collection_incomplete:
        lines.append("collection: INCOMPLETE (camera evidence infrastructure abort)")
    elif collection_not_started:
        lines.append("collection: NOT_STARTED (camera evidence admission)")
    else:
        lines.append("collection: PASS")
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
        if payload["result"] == "WARN":
            lines.append("warnings:")
        else:
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

    window_schemas = [window.get("window_schema_version") for window in windows]
    current_window_count = sum(schema == 3 for schema in window_schemas)
    if current_window_count:
        hardware_values = {
            str(window.get("hardware_scoring_fingerprint") or "") for window in windows
        }
        if (
            current_window_count != len(windows)
            or len(hardware_values) != 1
            or not valid_digest(next(iter(hardware_values), ""))
        ):
            identity_message = (
                "bench windows do not share one current hardware-scoring identity"
            )
            for window in windows:
                window["result"] = worse(str(window["result"]), "COLLECTION_FAILED")
                evidence = window.setdefault("evidence", [])
                if isinstance(evidence, list) and identity_message not in evidence:
                    evidence.append(identity_message)
            result = "COLLECTION_FAILED"

    git_shas = {str(window.get("git_sha") or "").strip() for window in windows}
    git_refs = {str(window.get("git_ref") or "").strip() for window in windows}
    product_fingerprints = {
        str(window.get("product_fingerprint") or "").strip() for window in windows
    }
    grader_fingerprints = {
        str(window.get("grader_fingerprint") or "").strip() for window in windows
    }
    hardware_scoring_fingerprints = {
        str(window.get("hardware_scoring_fingerprint") or "").strip()
        for window in windows
    }
    payload = {
        "schema_version": 4,
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
        "hardware_scoring_fingerprint": (
            next(iter(hardware_scoring_fingerprints))
            if len(hardware_scoring_fingerprints) == 1
            else ""
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
