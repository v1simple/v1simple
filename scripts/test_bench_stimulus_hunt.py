#!/usr/bin/env python3
"""Focused tests for generic replay stimulus validation and mode comparison."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from bench_stimulus_hunt import (  # noqa: E402
    compare_idle_modes_to_renderer,
    parse_stimulus_ledger,
)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def event(
    sequence: int,
    offset: float,
    mode: str,
    *,
    priority: bool = False,
    phase: str = "replaceable",
) -> dict[str, object]:
    alerts = (
        [
            {
                "band": "k",
                "bandMask": 4,
                "frequencyMHz": 24_150,
                "bars": 1,
                "direction": "FRONT",
                "priority": True,
            }
        ]
        if priority
        else []
    )
    return {
        "state": "stimulus_requested",
        "schemaVersion": 1,
        "stimulusSequence": sequence,
        "sourceIndex": sequence - 1,
        "replayOffsetSeconds": offset,
        "requestedHostMonotonicSeconds": 100.0 + offset,
        "expected": {
            "phase": phase,
            "alerts": alerts,
            "muted": False,
            "mainVolume": 4,
            "muteVolume": 0,
            "modeChar": mode,
            "displayOn": True,
            "arrowBlink": False,
        },
        "notifications": [
            {
                "ordinal": 0,
                "channel": "display_short",
                "kind": "alert_row",
                "alertRowIndex": 1 if alerts else 0,
                "alertRowCount": len(alerts),
                "bytesHex": "AAD8EA430800000000000000B7AB",
            },
            {
                "ordinal": 1,
                "channel": "display_short",
                "kind": "display_frame",
                "bytesHex": "AAD8EA310800000000000000A5AB",
            },
        ],
    }


def payload(events: list[dict[str, object]]) -> bytes:
    return b"".join(
        (json.dumps(item, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")
        for item in events
    )


def scenario() -> list[dict[str, object]]:
    return [
        event(1, 5.0, "l", priority=True, phase="new_alert_shape"),
        event(2, 5.5, "l"),
        event(3, 10.0, "L", phase="new_idle_shape"),
        event(4, 10.4, "L"),
        event(5, 14.0, "A"),
        event(6, 14.4, "A"),
    ]


def test_changed_scenario_needs_no_parser_change() -> None:
    events = scenario()
    summary, parsed = parse_stimulus_ledger(
        payload(events),
        owner_event_count=len(events),
        configured_total_samples=len(events),
    )
    assert_true(summary["status"] == "complete", f"changed scenario was rejected: {summary}")
    assert_true(parsed == events, "generic ledger changed scenario values")
    assert_true(summary["event_count"] == 6, "scenario count was hard-coded")
    assert_true(summary["transition_counts"]["mode"] == 2, "mode transitions were not derived")
    assert_true(summary["maximum_request_timing_error_ms"] == 0.0, "request timing changed")

    malformed = [dict(item) for item in events]
    malformed[2] = {**malformed[2], "stimulusSequence": 9}
    rejected, consumed = parse_stimulus_ledger(
        payload(malformed),
        owner_event_count=len(malformed),
        configured_total_samples=len(malformed),
    )
    assert_true(rejected["reason"] == "sequence_invalid" and consumed == [], "gap was trusted")


def write_csv(path: Path, header: str, rows: list[str]) -> None:
    path.write_text(header + "\n" + "\n".join(rows) + "\n", encoding="utf-8")


def test_idle_mode_comparison_matches_and_mismatches_without_a_gate() -> None:
    events = scenario()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        encounters = root / "encounters.csv"
        display = root / "display.csv"
        write_csv(
            encounters,
            "millis,priority,band,frequency_mhz,direction",
            ["1000,1,K,24150,FRONT"],
        )
        write_csv(
            display,
            "seq,millis,path,dispatch,pushes,mode_char",
            [
                "1,5900,RESTING,FULL,1,l",
                "2,6025,RESTING,FULL,1,L",
                "3,10050,RESTING,FULL,1,A",
            ],
        )
        matched = compare_idle_modes_to_renderer(
            events,
            display_commit_path=display,
            encounter_path=encounters,
        )
        assert_true(matched["status"] == "complete", f"mode comparison failed: {matched}")
        assert_true(matched["matched_count"] == 2 and matched["mismatched_count"] == 0, str(matched))
        assert_true(matched["maximum_response_ms"] == 50.0, "relative response changed")
        assert_true(
            all(item["deadline_basis"] == "next_stimulus_request" for item in matched["comparisons"]),
            "scenario-derived deadlines were replaced",
        )

        write_csv(
            display,
            "seq,millis,path,dispatch,pushes,mode_char",
            [
                "1,5900,RESTING,FULL,1,l",
                "2,6025,RESTING,FULL,1,L",
                "3,10450,RESTING,FULL,1,A",
            ],
        )
        mismatched = compare_idle_modes_to_renderer(
            events,
            display_commit_path=display,
            encounter_path=encounters,
        )
        assert_true(
            mismatched["matched_count"] == 1 and mismatched["mismatched_count"] == 1,
            f"late renderer state was accepted: {mismatched}",
        )

        encounters.write_text("millis,priority,band,frequency_mhz,direction\n", encoding="utf-8")
        unknown = compare_idle_modes_to_renderer(
            events,
            display_commit_path=display,
            encounter_path=encounters,
        )
        assert_true(
            unknown["status"] == "partial" and unknown["reason"] == "clock_anchor_unavailable",
            f"missing clock evidence became a mismatch: {unknown}",
        )


def main() -> int:
    test_changed_scenario_needs_no_parser_change()
    test_idle_mode_comparison_matches_and_mismatches_without_a_gate()
    print("bench stimulus hunter tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
