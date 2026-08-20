#!/usr/bin/env python3
"""Validate generic replay stimulus and compare idle mode changes to renderer commits."""

from __future__ import annotations

import csv
import json
import math
import re
from pathlib import Path
from typing import Any

HEX_BYTES = re.compile(r"(?:[0-9A-F]{2})+")
MODE_CHARS = {"A", "l", "L", "C", "u", "U"}


def _integer(value: Any, minimum: int = 0) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= minimum


def _number(value: Any, minimum: float = 0.0) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
        and float(value) >= minimum
    )


def _valid_alert(value: Any) -> bool:
    if not isinstance(value, dict) or set(value) != {
        "band",
        "bandMask",
        "frequencyMHz",
        "bars",
        "direction",
        "priority",
    }:
        return False
    return (
        isinstance(value["band"], str)
        and 0 < len(value["band"]) <= 16
        and _integer(value["bandMask"])
        and value["bandMask"] <= 255
        and _integer(value["frequencyMHz"], 1)
        and value["frequencyMHz"] <= 65_535
        and _integer(value["bars"], 1)
        and value["bars"] <= 8
        and value["direction"] in {"FRONT", "SIDE", "REAR"}
        and isinstance(value["priority"], bool)
    )


def _valid_expected(value: Any) -> bool:
    if not isinstance(value, dict) or set(value) != {
        "phase",
        "alerts",
        "muted",
        "mainVolume",
        "muteVolume",
        "modeChar",
        "displayOn",
        "arrowBlink",
    }:
        return False
    alerts = value["alerts"]
    return (
        isinstance(value["phase"], str)
        and 0 < len(value["phase"]) <= 96
        and isinstance(alerts, list)
        and len(alerts) <= 3
        and all(_valid_alert(alert) for alert in alerts)
        and (not alerts or sum(alert["priority"] for alert in alerts) == 1)
        and isinstance(value["muted"], bool)
        and _integer(value["mainVolume"])
        and value["mainVolume"] <= 9
        and _integer(value["muteVolume"])
        and value["muteVolume"] <= 9
        and value["modeChar"] in MODE_CHARS
        and isinstance(value["displayOn"], bool)
        and isinstance(value["arrowBlink"], bool)
        and (not value["arrowBlink"] or any(alert["priority"] for alert in alerts))
    )


def _valid_notification(value: Any, ordinal: int) -> bool:
    if not isinstance(value, dict):
        return False
    kind = value.get("kind")
    required = {"ordinal", "channel", "kind", "bytesHex"}
    if kind == "alert_row":
        required |= {"alertRowIndex", "alertRowCount"}
    if set(value) != required:
        return False
    payload = value.get("bytesHex")
    common = (
        _integer(value.get("ordinal"))
        and value.get("ordinal") == ordinal
        and value.get("channel") in {"display_short", "display_long"}
        and kind in {"alert_row", "display_frame"}
        and isinstance(payload, str)
        and HEX_BYTES.fullmatch(payload) is not None
        and payload.startswith("AA")
        and payload.endswith("AB")
    )
    if kind != "alert_row":
        return common
    return (
        common
        and _integer(value.get("alertRowIndex"))
        and _integer(value.get("alertRowCount"))
        and value["alertRowIndex"] <= 3
        and value["alertRowCount"] <= 3
    )


def _valid_event(value: Any) -> bool:
    if not isinstance(value, dict) or set(value) != {
        "state",
        "schemaVersion",
        "stimulusSequence",
        "sourceIndex",
        "replayOffsetSeconds",
        "requestedHostMonotonicSeconds",
        "expected",
        "notifications",
    }:
        return False
    notifications = value["notifications"]
    if not (
        value["state"] == "stimulus_requested"
        and value["schemaVersion"] == 1
        and not isinstance(value["schemaVersion"], bool)
        and _integer(value["stimulusSequence"], 1)
        and _integer(value["sourceIndex"])
        and _number(value["replayOffsetSeconds"])
        and _number(value["requestedHostMonotonicSeconds"])
        and _valid_expected(value["expected"])
        and isinstance(notifications, list)
        and 1 <= len(notifications) <= 4
        and all(_valid_notification(item, index) for index, item in enumerate(notifications))
        and notifications[-1]["kind"] == "display_frame"
    ):
        return False
    alerts = value["expected"]["alerts"]
    alert_rows = [item for item in notifications if item["kind"] == "alert_row"]
    expected_indexes = [0] if not alerts else list(range(1, len(alerts) + 1))
    expected_count = len(alerts)
    return (
        len(notifications) == len(alert_rows) + 1
        and [row["alertRowIndex"] for row in alert_rows] == expected_indexes
        and all(row["alertRowCount"] == expected_count for row in alert_rows)
    )


def parse_stimulus_ledger(
    payload: bytes,
    *,
    owner_event_count: int,
    configured_total_samples: int,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Return bounded facts plus validated events; malformed evidence is never consumed."""
    try:
        lines = payload.splitlines()
        values = [json.loads(line) for line in lines]
    except (UnicodeDecodeError, json.JSONDecodeError):
        return {"status": "partial", "reason": "invalid_ndjson"}, []
    if not values:
        return {"status": "partial", "reason": "events_empty"}, []
    if any(not _valid_event(value) for value in values):
        return {"status": "partial", "reason": "event_invalid"}, []
    events: list[dict[str, Any]] = values
    sequences = [event["stimulusSequence"] for event in events]
    source_indexes = [event["sourceIndex"] for event in events]
    offsets = [float(event["replayOffsetSeconds"]) for event in events]
    requested = [float(event["requestedHostMonotonicSeconds"]) for event in events]
    if sequences != list(range(1, len(events) + 1)):
        return {"status": "partial", "reason": "sequence_invalid"}, []
    if source_indexes != list(range(len(events))):
        return {"status": "partial", "reason": "source_index_invalid"}, []
    if any(current <= previous for previous, current in zip(offsets, offsets[1:])):
        return {"status": "partial", "reason": "replay_time_invalid"}, []
    if any(current <= previous for previous, current in zip(requested, requested[1:])):
        return {"status": "partial", "reason": "request_time_invalid"}, []
    if owner_event_count != len(events) or configured_total_samples != len(events):
        return {"status": "partial", "reason": "event_count_mismatch"}, []

    first_offset = offsets[0]
    first_requested = requested[0]
    timing_errors_ms = [
        abs((request - first_requested) - (offset - first_offset)) * 1000.0
        for request, offset in zip(requested, offsets)
    ]
    transition_counts = {"volume": 0, "mute": 0, "mode": 0, "alerts": 0}
    for previous, current in zip(events, events[1:]):
        before = previous["expected"]
        after = current["expected"]
        transition_counts["volume"] += (
            (before["mainVolume"], before["muteVolume"])
            != (after["mainVolume"], after["muteVolume"])
        )
        transition_counts["mute"] += before["muted"] != after["muted"]
        transition_counts["mode"] += before["modeChar"] != after["modeChar"]
        transition_counts["alerts"] += before["alerts"] != after["alerts"]
    return (
        {
            "status": "complete",
            "reason": "",
            "event_count": len(events),
            "notification_request_count": sum(len(event["notifications"]) for event in events),
            "first_stimulus_sequence": sequences[0],
            "last_stimulus_sequence": sequences[-1],
            "first_replay_offset_seconds": round(offsets[0], 6),
            "last_replay_offset_seconds": round(offsets[-1], 6),
            "maximum_request_timing_error_ms": round(max(timing_errors_ms), 3),
            "transition_counts": transition_counts,
        },
        events,
    )


def _csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(line for line in handle if not line.startswith("#")))


def compare_idle_modes_to_renderer(
    events: list[dict[str, Any]],
    *,
    display_commit_path: Path,
    encounter_path: Path,
) -> dict[str, Any]:
    """Compare code-resolved idle mode transitions inside the next sample interval."""
    try:
        display_rows = _csv_rows(display_commit_path)
        encounter_rows = _csv_rows(encounter_path)
        first_priority_event = next(
            event
            for event in events
            if any(alert["priority"] for alert in event["expected"]["alerts"])
        )
        priority = next(
            alert for alert in first_priority_event["expected"]["alerts"] if alert["priority"]
        )
        first_priority_row = next(row for row in encounter_rows if int(row["priority"]) == 1)
        if (
            int(first_priority_row["frequency_mhz"]) != priority["frequencyMHz"]
            or first_priority_row["direction"] != priority["direction"]
            or first_priority_row["band"].lower() != priority["band"].lower()
        ):
            raise ValueError("first priority stimulus and encounter disagree")
        anchor_offset = float(first_priority_event["replayOffsetSeconds"])
        anchor_millis = int(first_priority_row["millis"])
        parsed_display = [
            {
                **row,
                "seq": int(row["seq"]),
                "millis": int(row["millis"]),
                "pushes": int(row["pushes"]),
            }
            for row in display_rows
        ]
    except (OSError, csv.Error, KeyError, TypeError, ValueError, StopIteration):
        return {"status": "partial", "reason": "clock_anchor_unavailable", "comparisons": []}

    transitions: list[tuple[dict[str, Any], dict[str, Any], dict[str, Any]]] = []
    for index, event in enumerate(events[1:], start=1):
        before = events[index - 1]["expected"]
        after = event["expected"]
        if (
            before["modeChar"] != after["modeChar"]
            and not after["alerts"]
            and after["displayOn"]
        ):
            transitions.append(
                (events[index - 1], event, events[index + 1] if index + 1 < len(events) else event)
            )
    if not transitions:
        return {"status": "not_applicable", "reason": "idle_mode_transitions_absent", "comparisons": []}

    comparisons: list[dict[str, Any]] = []
    for previous, event, following in transitions:
        offset = float(event["replayOffsetSeconds"])
        following_offset = float(following["replayOffsetSeconds"])
        expected_millis = anchor_millis + (offset - anchor_offset) * 1000.0
        deadline_millis = anchor_millis + (following_offset - anchor_offset) * 1000.0
        expected_mode = event["expected"]["modeChar"]
        previous_expected_mode = previous["expected"]["modeChar"]
        prior = next(
            (
                row
                for row in reversed(parsed_display)
                if row["millis"] < expected_millis
                and row["path"] == "RESTING"
                and row["dispatch"] != "NONE"
                and row["pushes"] > 0
            ),
            None,
        )
        candidate = next(
            (
                row
                for row in parsed_display
                if row["millis"] >= expected_millis
                and row["millis"] < deadline_millis
                and row["mode_char"] == expected_mode
                and row["path"] == "RESTING"
                and row["dispatch"] != "NONE"
                and row["pushes"] > 0
            ),
            None,
        )
        comparison = {
            "stimulus_sequence": event["stimulusSequence"],
            "source_index": event["sourceIndex"],
            "replay_offset_seconds": round(offset, 6),
            "expected_mode_char": expected_mode,
            "expected_dut_millis": round(expected_millis, 3),
            "deadline_dut_millis": round(deadline_millis, 3),
            "deadline_basis": "next_stimulus_request",
            "state": (
                "unknown"
                if prior is None or prior["mode_char"] != previous_expected_mode
                else ("matched" if candidate else "mismatched")
            ),
            "previous_expected_mode_char": previous_expected_mode,
            "previous_renderer_mode_char": prior["mode_char"] if prior else None,
            "renderer": None,
        }
        if candidate and comparison["state"] == "matched":
            comparison["renderer"] = {
                "sequence": candidate["seq"],
                "dut_millis": candidate["millis"],
                "path": candidate["path"],
                "dispatch": candidate["dispatch"],
                "pushes": candidate["pushes"],
                "mode_char": candidate["mode_char"],
                "response_ms": round(candidate["millis"] - expected_millis, 3),
            }
        comparisons.append(comparison)
    matched = [item for item in comparisons if item["state"] == "matched"]
    mismatched = [item for item in comparisons if item["state"] == "mismatched"]
    unknown = [item for item in comparisons if item["state"] == "unknown"]
    return {
        "status": "complete",
        "reason": "",
        "clock_mapping": {
            "basis": "first_matching_priority_stimulus_to_encounter",
            "anchor_stimulus_sequence": first_priority_event["stimulusSequence"],
            "anchor_replay_offset_seconds": round(anchor_offset, 6),
            "anchor_dut_millis": anchor_millis,
        },
        "comparison_count": len(comparisons),
        "matched_count": len(matched),
        "mismatched_count": len(mismatched),
        "unknown_count": len(unknown),
        "maximum_response_ms": (
            max(item["renderer"]["response_ms"] for item in matched) if matched else None
        ),
        "comparisons": comparisons,
    }
