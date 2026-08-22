#!/usr/bin/env python3
"""Deterministic synthetic tests for offline bench time alignment."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
from fractions import Fraction
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from aligned_timeline import (  # noqa: E402
    build_aligned_timeline,
    extract_qsync_exchanges,
    generate_alignment_artifacts,
    read_csv_records,
)
from clock_alignment import fit_clock_alignment, map_dut_timestamp  # noqa: E402


TRUE_SLOPE = Fraction(25_001, 25)  # 1000.04 ns/us, or +40 ppm.
TRUE_OFFSET = 1_000_000_000_000


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def nearest(value: Fraction) -> int:
    quotient, remainder = divmod(abs(value.numerator), value.denominator)
    if remainder * 2 >= value.denominator:
        quotient += 1
    return -quotient if value < 0 else quotient


def true_host(dut_us: int, offset: int = TRUE_OFFSET, slope: Fraction = TRUE_SLOPE) -> int:
    return nearest(Fraction(offset) + slope * dut_us)


def qualification_host_boundaries(
    session_token: str,
    *,
    start_dut_us: int,
    end_dut_us: int,
    terminal_observation_delay_ns: int = 0,
) -> list[dict[str, object]]:
    return [
        {
            "event": "serial_receive",
            "host_monotonic_ns": true_host(start_dut_us),
            "line": "QRESP "
            + json.dumps(
                {
                    "ok": True,
                    "state": "running",
                    "sessionToken": session_token,
                    "startedAtDutMicros": start_dut_us,
                    "clockSegment": "boot-chain",
                },
                separators=(",", ":"),
            ),
        },
        {
            "event": "serial_receive",
            "host_monotonic_ns": true_host(end_dut_us) + terminal_observation_delay_ns,
            "line": "QEVENT "
            + json.dumps(
                {
                    "ok": True,
                    "state": "finalizing",
                    "sessionToken": session_token,
                    "dutMicros": end_dut_us,
                    "clockSegment": "boot-chain",
                },
                separators=(",", ":"),
            ),
        },
    ]


def qualification_session_end(
    session_token: str,
    *,
    trace_seq: int,
    end_dut_us: int,
) -> dict[str, object]:
    return {
        "trace_seq": trace_seq,
        "qualification_session_token": session_token,
        "stage": "SESSION_END",
        "stage_dut_micros": end_dut_us,
        "clock_segment": "boot-chain",
    }


def qsync_fixture(
    segment: str,
    *,
    start_us: int = 100_000,
    count: int = 18,
    step_us: int = 1_000_000,
    offset: int = TRUE_OFFSET,
    slope: Fraction = TRUE_SLOPE,
    outlier_indices: set[int] | None = None,
) -> list[dict[str, object]]:
    outliers = outlier_indices or set()
    result: list[dict[str, object]] = []
    for index in range(count):
        d2 = start_us + index * step_us
        d3 = d2 + 40
        delay = 120_000 + (index % 3) * 2_000
        asymmetry = (0, 3_000, -3_000, 1_000, -1_000)[index % 5]
        forward = delay + asymmetry + (4_000_000 if index in outliers else 0)
        reverse = delay - asymmetry
        result.append(
            {
                "event": "qsync_exchange",
                "status": "observed",
                "nonce": f"synthetic-{segment}-{index}",
                "clock_segment": segment,
                # These are the field names emitted by run_window.
                "h1_host_ns": true_host(d2, offset, slope) - forward,
                "d2_dut_us": d2,
                "d3_dut_us": d3,
                "h4_host_ns": true_host(d3, offset, slope) + reverse,
                "future_additive_column": f"kept-{index}",
            }
        )
    return result


def test_run_window_qsync_names_robust_fit_bounds_and_outliers() -> None:
    timeline = qsync_fixture("boot-a", outlier_indices={1, 7, 13})
    timeline.append(
        {
            "event": "qsync_exchange",
            "status": "timeout",
            "nonce": "synthetic-timeout",
            "clock_segment": "boot-a",
            "h1_host_ns": 123,
        }
    )
    exchanges = extract_qsync_exchanges(timeline)
    assert_true(len(exchanges) == len(timeline), "QSYNC parser dropped an observed or failed exchange")
    alignment = fit_clock_alignment(exchanges)
    assert_true(alignment["raw_exchange_count"] == len(timeline), "raw exchanges were not retained")
    assert_true(
        alignment["raw_exchanges"][-1]["status"] == "invalid",
        "failed exchange was not retained as invalid evidence",
    )
    assert_true(
        alignment["raw_exchanges"][0]["raw_exchange"]["future_additive_column"] == "kept-0",
        "additive QSYNC fields were discarded",
    )

    mapping = alignment["segments"][0]
    assert_true(mapping["fit_type"] == "affine", f"unexpected fit: {mapping}")
    assert_true(
        abs(mapping["slope_ns_per_us"] - float(TRUE_SLOPE)) < 0.002,
        f"known oscillator drift was not recovered: {mapping}",
    )
    assert_true(abs(mapping["drift_ppm"] - 40.0) < 2.0, "ppm drift report is inaccurate")
    exact_offset = Fraction(
        mapping["offset_host_ns_numerator"], mapping["offset_host_ns_denominator"]
    )
    assert_true(
        abs(exact_offset - TRUE_OFFSET) < 10_000,
        f"known clock offset was not recovered: {exact_offset}",
    )
    assert_true(mapping["selected_source_records"], "low-delay selection is absent")
    assert_true(
        mapping["maximum_selected_residual_ns"] < 5_000,
        "serial outlier biased the robust selected fit",
    )
    assert_true(
        mapping["maximum_residual_ns"] > 1_000_000,
        "all-observation residual hid the serial outlier",
    )
    assert_true(
        mapping["uncertainty_smaller_than_2_5_ms"]
        and mapping["uncertainty_width_ns"] < 500_000,
        "an interior serial delay widened the selected-endpoint clock bounds",
    )
    assert_true(
        mapping["uncertainty_boundary_source_records"] == [1, 16],
        f"low-delay boundary observations are wrong: {mapping}",
    )
    for raw in alignment["raw_exchanges"][:-1]:
        assert_true("adjusted_round_trip_ns" in raw, "adjusted RTT is absent")
        assert_true("offset_lower_ns" in raw and "offset_upper_ns" in raw, "offset bounds are absent")
        assert_true("midpoint_residual_ns" in raw, "exchange residual is absent")
    assert_true(
        max(raw["adjusted_round_trip_ns"] for raw in alignment["raw_exchanges"][:-1])
        > 4_000_000,
        "interior serial-delay evidence was discarded with the uncertainty outlier",
    )

    validity = mapping["validity_dut_us"]
    for dut_us in (
        validity["start"],
        (validity["start"] + validity["end"]) // 2,
        validity["end"],
    ):
        mapped = map_dut_timestamp(alignment, "boot-a", dut_us)
        assert_true(mapped["status"] == "mapped", f"in-range timestamp did not map: {mapped}")
        truth = true_host(dut_us)
        assert_true(
            mapped["host_earliest_ns"] <= truth <= mapped["host_latest_ns"],
            f"true host timestamp escaped conservative bounds: {mapped}, truth={truth}",
        )
        assert_true(mapped["fit_quality"] == "good", "a sound affine fit was mislabeled")
    final_raw_midpoint = 17_100_020
    assert_true(
        validity["end"] < final_raw_midpoint
        and map_dut_timestamp(alignment, "boot-a", validity["end"] + 1)["status"]
        == "out_of_range",
        "mapper extended selected-endpoint bounds across unbounded raw observations",
    )
    outside = map_dut_timestamp(alignment, "boot-a", 50_000_000)
    assert_true(outside["status"] == "out_of_range", "mapper extrapolated outside validity")


def test_late_qsync_reply_is_rejoined_by_nonce() -> None:
    timeline = [
        {
            "event": "qsync_exchange",
            "status": "failed",
            "nonce": "0000000000000001",
            "h1_host_ns": 1_000_000_000,
        },
        {
            "event": "qsync_exchange",
            "status": "observed",
            "nonce": "0000000000000002",
            "clock_segment": "34",
            "h1_host_ns": 1_010_000_000,
            "d2_dut_us": 10_000,
            "d3_dut_us": 10_010,
            "h4_host_ns": 1_010_200_000,
            "unexpected_replies": [
                {
                    "status": "late_observed",
                    "reply_nonce": "0000000000000001",
                    "clock_segment": "17",
                    "d2_dut_us": 100,
                    "d3_dut_us": 110,
                    "h4_host_ns": 1_005_000_000,
                }
            ],
        },
        {
            "event": "qsync_exchange",
            "status": "observed",
            "nonce": "0000000000000003",
            "clock_segment": "34",
            "h1_host_ns": 1_020_000_000,
            "d2_dut_us": 20_000,
            "d3_dut_us": 20_010,
            "h4_host_ns": 1_020_200_000,
        },
    ]
    exchanges = extract_qsync_exchanges(timeline)
    assert_true(len(exchanges) == 4, f"late physical exchange was not reconstructed: {exchanges}")
    late = exchanges[1]
    assert_true(
        late["late_reply"] is True
        and late["h1_host_ns"] == timeline[0]["h1_host_ns"]
        and late["h4_host_ns"] == timeline[1]["unexpected_replies"][0]["h4_host_ns"],
        f"late reply did not retain its original four timestamps: {late}",
    )
    alignment = fit_clock_alignment(exchanges)
    assert_true(
        [segment["mapping_id"] for segment in alignment["segments"]] == ["17:1", "34:1"],
        f"late prior-segment reply split the current segment: {alignment['segments']}",
    )
    raw_late = next(
        record
        for record in alignment["raw_exchanges"]
        if record["raw_exchange"].get("late_reply") is True
    )
    assert_true(
        raw_late["status"] == "valid" and "adjusted_round_trip_ns" in raw_late,
        f"late exchange was not evaluated with its long physical delay: {raw_late}",
    )


def test_raw_serial_qsync_reply_is_joined_once_after_timeout() -> None:
    first_nonce = "0000000000000001"
    second_nonce = "0000000000000002"
    timeline = [
        {
            "event": "qsync_exchange",
            "status": "failed",
            "nonce": first_nonce,
            "h1_host_ns": 2_000_000_000,
        },
        {
            "event": "serial_receive",
            "host_monotonic_ns": 2_005_000_000,
            "line": (
                f"QSYNC {first_nonce} 0000000000000011 "
                "0000000000000100 0000000000000110"
            ),
        },
        {
            "event": "serial_receive",
            "host_monotonic_ns": 2_010_200_000,
            "line": (
                f"QSYNC {second_nonce} 0000000000000022 "
                "0000000000001000 0000000000001010"
            ),
        },
        {
            "event": "qsync_exchange",
            "status": "observed",
            "nonce": second_nonce,
            "reply_nonce": second_nonce,
            "clock_segment": "34",
            "h1_host_ns": 2_010_000_000,
            "d2_dut_us": 0x1000,
            "d3_dut_us": 0x1010,
            "h4_host_ns": 2_010_200_000,
        },
    ]
    exchanges = extract_qsync_exchanges(timeline)
    assert_true(
        len(exchanges) == 3,
        f"serial QSYNC reconstruction duplicated an already-owned reply: {exchanges}",
    )
    reconstructed = exchanges[1]
    assert_true(
        reconstructed["nonce"] == first_nonce
        and reconstructed["clock_segment"] == "17"
        and reconstructed["h1_host_ns"] == 2_000_000_000
        and reconstructed["h4_host_ns"] == 2_005_000_000
        and reconstructed["d2_dut_us"] == 0x100
        and reconstructed["d3_dut_us"] == 0x110
        and reconstructed["reconstructed_from_serial_receive"] is True,
        f"raw fixed-width reply was not joined to its timed-out request: {reconstructed}",
    )
    assert_true(
        sum(exchange.get("nonce") == second_nonce for exchange in exchanges) == 1,
        f"normal QSYNC reply was counted once from serial and again from its exchange: {exchanges}",
    )
    alignment = fit_clock_alignment(exchanges)
    raw_reconstructed = next(
        record
        for record in alignment["raw_exchanges"]
        if record["raw_exchange"].get("reconstructed_from_serial_receive") is True
    )
    assert_true(
        raw_reconstructed["status"] == "valid",
        f"reconstructed serial reply was not usable clock evidence: {raw_reconstructed}",
    )


def test_clock_segment_changes_reboots_and_offset_only() -> None:
    first_a = qsync_fixture("boot-a", start_us=100_000, count=5)
    boot_b = qsync_fixture("boot-b", start_us=100_000, count=5, offset=TRUE_OFFSET + 9_000_000)
    second_a = qsync_fixture("boot-a", start_us=100_000, count=5, offset=TRUE_OFFSET + 20_000_000)
    short_c = qsync_fixture("boot-c", start_us=50_000, count=3, step_us=100_000)
    alignment = fit_clock_alignment([*first_a, *boot_b, *second_a, *short_c])
    identifiers = [segment["mapping_id"] for segment in alignment["segments"]]
    assert_true(
        identifiers == ["boot-a:1", "boot-b:1", "boot-a:2", "boot-c:1"],
        f"clock reboot segmentation is wrong: {identifiers}",
    )
    assert_true(alignment["segments"][-1]["fit_type"] == "offset_only", "weak drift fit was forced")
    ambiguous = map_dut_timestamp(alignment, "boot-a", 2_100_000)
    assert_true(ambiguous["status"] == "ambiguous", "reused segment ID was guessed")
    resolved = map_dut_timestamp(alignment, "boot-a", 2_100_000, segment_instance=2)
    assert_true(resolved["status"] == "mapped", "explicit segment instance did not resolve reboot")
    offset_only = map_dut_timestamp(alignment, "boot-c", 150_000)
    assert_true(offset_only["status"] == "mapped", f"offset-only evidence did not map: {offset_only}")
    assert_true(
        offset_only["host_earliest_ns"] <= true_host(150_000) <= offset_only["host_latest_ns"],
        f"offset-only bounds excluded truth: {offset_only}",
    )
    assert_true(
        map_dut_timestamp(alignment, "boot-c", 400_000)["status"] == "out_of_range",
        "offset-only mapper extrapolated beyond its midpoint span",
    )


def test_midpoint_validity_is_conservative_under_asymmetric_delay() -> None:
    offset = 700_000_000_000
    true_slope = Fraction(1000, 1)
    exchanges: list[dict[str, object]] = []
    for index in range(5):
        d2 = index * 1_000_000
        d3 = d2 + 40
        midpoint = Fraction(d2 + d3, 2)
        forward_delay = 900_000_000 - 200 * int(midpoint)
        reverse_delay = 1_000
        assert_true(forward_delay >= 0, "invalid asymmetric fixture")
        exchanges.append(
            {
                "event": "qsync_exchange",
                "status": "observed",
                "nonce": f"asymmetric-{index}",
                "clock_segment": "asymmetric",
                "h1_host_ns": true_host(d2, offset, true_slope) - forward_delay,
                "d2_dut_us": d2,
                "d3_dut_us": d3,
                "h4_host_ns": true_host(d3, offset, true_slope) + reverse_delay,
            }
        )
    alignment = fit_clock_alignment(exchanges)
    mapping = alignment["segments"][0]
    assert_true(mapping["slope_ns_per_us"] > 1_050, f"adversarial slope was not exercised: {mapping}")
    assert_true(mapping["poor_fit"] is True, "adversarial fit quality was hidden")
    assert_true(
        mapping["validity_dut_us"] == {"start": 20, "end": 4_000_020},
        f"validity was not restricted to observed midpoints: {mapping}",
    )
    assert_true(
        map_dut_timestamp(alignment, "asymmetric", 0)["status"] == "out_of_range",
        "mapper asserted a bound before the first midpoint",
    )
    for dut_us in (20, 4_000_020):
        mapped = map_dut_timestamp(alignment, "asymmetric", dut_us)
        truth = true_host(dut_us, offset, true_slope)
        assert_true(
            mapped["host_earliest_ns"] <= truth <= mapped["host_latest_ns"],
            f"asymmetric truth escaped midpoint envelope: {mapped}, truth={truth}",
        )


def test_exchange_status_unavailable_fit_and_exact_mapping_fields() -> None:
    failed = qsync_fixture("failed-segment", count=1)[0]
    failed["status"] = "timeout"
    alignment = fit_clock_alignment([failed])
    assert_true(
        alignment["raw_exchanges"][0]["status"] == "invalid"
        and "unsuccessful_exchange_status" in alignment["raw_exchanges"][0]["errors"],
        "complete timestamps from a failed exchange were accepted",
    )
    unavailable = map_dut_timestamp(alignment, "failed-segment", 100_020)
    assert_true(
        unavailable == {"status": "missing_evidence", "reason": "unavailable_fit"},
        f"unavailable fit was mislabeled: {unavailable}",
    )

    uninterrupted = qsync_fixture("segment-a", count=2)
    false_segment = qsync_fixture("untrusted-segment", count=1)[0]
    false_segment["status"] = "nonce_mismatch"
    false_segment["h1_host_ns"] += 500_000
    false_segment["h4_host_ns"] += 500_000
    combined = fit_clock_alignment([uninterrupted[0], false_segment, uninterrupted[1]])
    assert_true(
        [segment["mapping_id"] for segment in combined["segments"]]
        == ["segment-a:1", "untrusted-segment:1"],
        f"invalid exchange split an uninterrupted valid segment: {combined['segments']}",
    )
    assert_true(
        [item.get("mapping_id") for item in combined["raw_exchanges"]]
        == ["segment-a:1", "untrusted-segment:1", "segment-a:1"],
        f"raw exchange mapping lost uninterrupted segment identity: {combined['raw_exchanges']}",
    )

    clean = clean_alignment()
    mapping = clean["segments"][0]
    assert_true(mapping["uncertainty_smaller_than_2_5_ms"] is True, "clean fit missed 2.5 ms target")
    exact_slope = Fraction(mapping["slope_numerator"], mapping["slope_denominator"])
    exact_offset = Fraction(
        mapping["offset_host_ns_numerator"], mapping["offset_host_ns_denominator"]
    )
    assert_true(
        exact_offset
        == Fraction(mapping["reference_host_ns_numerator"], mapping["reference_host_ns_denominator"])
        - exact_slope * mapping["reference_dut_us"],
        "serialized exact affine offset is inconsistent",
    )
    mapped = map_dut_timestamp(clean, "boot-chain", 2_000_000)
    exact_estimate = exact_slope * 2_000_000 + exact_offset
    assert_true(
        mapped["host_estimate_ns"] == nearest(exact_estimate),
        "mapper did not use the serialized exact rational model",
    )

    boundary = fit_clock_alignment(
        [
            {
                "status": "observed",
                "nonce": "boundary-width",
                "clock_segment": "boundary-width",
                "h1_host_ns": 1_000_000_000,
                "d2_dut_us": 0,
                "d3_dut_us": 0,
                "h4_host_ns": 1_002_499_999,
            }
        ]
    )
    boundary_mapping = boundary["segments"][0]
    boundary_timestamp = map_dut_timestamp(boundary, "boundary-width", 0)
    assert_true(
        boundary_mapping["uncertainty_width_ns"] == 2_500_001
        and boundary_mapping["uncertainty_smaller_than_2_5_ms"] is False,
        f"segment rounding understated the 2.5 ms boundary: {boundary_mapping}",
    )
    assert_true(
        boundary_timestamp["uncertainty_width_ns"]
        == boundary_timestamp["host_latest_ns"] - boundary_timestamp["host_earliest_ns"]
        == 2_500_001
        and boundary_timestamp["uncertainty_smaller_than_2_5_ms"] is False,
        f"mapped rounding understated the 2.5 ms boundary: {boundary_timestamp}",
    )


def clean_alignment() -> dict[str, object]:
    return fit_clock_alignment(qsync_fixture("boot-chain", count=12))


def complete_chain_inputs() -> tuple[list[dict[str, object]], list[dict[str, object]], list[dict[str, object]], list[dict[str, object]]]:
    payload_hex = "10203040"
    bench = [
        {
            "state": "stimulus_requested",
            "requestedHostMonotonicNs": true_host(2_000_000),
            "observer_host_monotonic_ns": true_host(2_100_000),
            "intendedHostMonotonicNs": true_host(1_990_000),
            "stimulusSequence": 11,
            "notifications": [
                {
                    "emissionOrdinal": 0,
                    "channel": "B2CE",
                    "bytesHex": payload_hex,
                }
            ],
        },
        {
            "state": "notification_requested",
            "hostMonotonicNs": true_host(2_001_000),
            "stimulusSequence": 11,
            "emissionOrdinal": 0,
            "globalTxSequence": 101,
            "characteristic": "B2CE",
            "payloadHex": payload_hex,
        },
        {
            "state": "notification_accepted",
            "hostMonotonicNs": true_host(2_002_000),
            "globalTxSequence": 101,
            "characteristic": "B2CE",
            "payloadHex": payload_hex,
        },
    ]
    causal_base: dict[str, object] = {
        "clock_segment": "boot-chain",
        "packet_id": 7,
        "event_seq": 88,
        "ble_session_generation": 4,
        "rx_first_seq": 12,
        "rx_last_seq": 12,
        "characteristic": "B2CE",
        "payload_length": 4,
        "exact_payload_hex": payload_hex,
    }
    causal = [
        {**causal_base, "stage": "BLE_RX", "stage_dut_micros": 2_003_000},
        {**causal_base, "stage": "PACKET_PARSE", "stage_dut_micros": 2_003_100},
        {
            **causal_base,
            "stage": "STATE_PUBLISH",
            "stage_dut_micros": 2_003_200,
            "state_revision": 3,
        },
    ]
    display = [
        {
            "clock_segment": "boot-chain",
            "render_request_dut_micros": 2_003_300,
            "display_commit_dut_micros": 2_003_500,
            "state_revision": 3,
            "event_seq": 88,
            "commit_seq": 9,
            "future_display_column": "accepted",
        }
    ]
    camera = [
        {
            "schema_version": 1,
            "frame_seq": 501,
            "source_pts_value": 25_000,
            "source_pts_timescale": 1_000_000,
            "host_capture_ns": true_host(2_003_400),
            "callback_host_ns": true_host(2_003_450),
            "video_pts_value": 20_000,
            "video_pts_timescale": 1_000_000,
            "duration_ns": 5_000_000,
            "status": "written",
            "drop_reason": None,
            "source_clock": "AVCaptureSession.synchronizationClock",
        }
    ]
    return bench, causal, display, camera


def test_complete_replay_to_display_to_frame_chain_and_required_schema() -> None:
    bench, causal, display, camera = complete_chain_inputs()
    records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=bench,
        causal_trace=causal,
        display_commits=display,
        camera_sidecar=camera,
    )
    required = {
        "schema_version",
        "kind",
        "raw_clock",
        "raw_timestamp",
        "host_estimate_ns",
        "host_earliest_ns",
        "host_latest_ns",
        "source_artifact",
        "source_record",
        "causal_identifiers",
    }
    for record in records:
        assert_true(required <= record.keys(), f"aligned record lacks required fields: {record}")
    stimulus = next(record for record in records if record["kind"] == "stimulus_requested")
    assert_true(
        stimulus["raw_timestamp"] == true_host(2_000_000)
        and stimulus["host_estimate_ns"] == true_host(2_000_000),
        f"stimulus alignment did not prefer the requested deadline timestamp: {stimulus}",
    )
    associations = [record for record in records if record["kind"] == "causal_association"]
    expected_relations = {
        "replay_deadline_to_notification_requested",
        "notification_requested_to_accepted",
        "notification_accepted_to_dut_ble_rx",
        "dut_ble_rx_to_packet_parse",
        "packet_parse_to_revision",
        "state_revision_to_render",
        "render_to_display_commit",
        "display_commit_to_camera_frame",
    }
    by_relation = {record["relation"]: record for record in associations}
    assert_true(expected_relations <= by_relation.keys(), f"causal chain is incomplete: {by_relation}")
    for relation in expected_relations:
        assert_true(
            by_relation[relation]["association_status"] == "matched",
            f"complete synthetic chain failed at {relation}: {by_relation[relation]}",
        )
    frame = next(record for record in records if record["kind"] == "camera_frame")
    for field in (
        "frame_seq",
        "source_pts_value",
        "source_pts_timescale",
        "video_pts_value",
        "video_pts_timescale",
        "duration_ns",
        "status",
    ):
        assert_true(field in frame, f"camera evidence lost {field}")


def test_repeated_missing_packets_and_explicit_interval_outcomes() -> None:
    payload_hex = "a1b2"
    digest = hashlib.sha256(bytes.fromhex(payload_hex)).hexdigest()
    bench = [
        {
            "state": "notification_accepted",
            "hostMonotonicNs": true_host(3_000_000 + index * 100),
            "globalTxSequence": index + 1,
            "characteristic": "alert",
            "payloadHex": payload_hex,
        }
        for index in range(2)
    ]
    causal = [
        {
            "stage": "BLE_RX",
            "stage_dut_monotonic_us": 3_000_250,
            "clock_segment": "boot-chain",
            "packet_id": 1,
            "characteristic": "alert",
            "payload_length": 2,
            "payload_sha256": digest,
        }
    ]
    records = build_aligned_timeline(clean_alignment(), bench_timeline=bench, causal_trace=causal)
    packet = [
        record
        for record in records
        if record.get("relation") == "notification_accepted_to_dut_ble_rx"
    ]
    statuses = sorted(record["association_status"] for record in packet)
    assert_true(
        statuses == ["ambiguous", "ambiguous"],
        f"missing repeated packet was forced into an ordered match: {packet}",
    )

    unique_missing = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            {
                "state": "notification_accepted",
                "hostMonotonicNs": true_host(3_100_000),
                "globalTxSequence": 9,
                "characteristic": "alert",
                "payloadHex": "ffff",
            }
        ],
        causal_trace=causal,
    )
    unique_relation = next(
        record
        for record in unique_missing
        if record.get("relation") == "notification_accepted_to_dut_ble_rx"
    )
    assert_true(
        unique_relation["association_status"] == "no_match",
        "a uniquely identified missing packet was not reported as no_match",
    )

    display = [
        {
            "clock_segment": "boot-chain",
            "display_commit_dut_us": 4_000_000,
            "commit_seq": 12,
        }
    ]
    commit_host = true_host(4_000_000)
    overlapping_camera = [
        {
            "frame_seq": index,
            "source_pts_value": index,
            "source_pts_timescale": 200,
            "host_capture_ns": commit_host - 100_000 + index * 50_000,
            "callback_host_ns": commit_host,
            "video_pts_value": index,
            "video_pts_timescale": 200,
            "duration_ns": 5_000_000,
            "status": "written",
            "source_clock": "synthetic_camera_clock",
        }
        for index in range(2)
    ]
    ambiguous_records = build_aligned_timeline(
        clean_alignment(), display_commits=display, camera_sidecar=overlapping_camera
    )
    interval = next(
        record
        for record in ambiguous_records
        if record.get("relation") == "display_commit_to_camera_frame"
    )
    assert_true(interval["association_status"] == "ambiguous", "nearest frame was forced")

    missing_records = build_aligned_timeline(clean_alignment(), display_commits=display)
    missing = next(
        record
        for record in missing_records
        if record.get("relation") == "display_commit_to_camera_frame"
    )
    assert_true(missing["association_status"] == "missing_evidence", "absent camera looked like no-match")

    far_camera = [{**overlapping_camera[0], "host_capture_ns": commit_host + 50_000_000}]
    no_match_records = build_aligned_timeline(
        clean_alignment(), display_commits=display, camera_sidecar=far_camera
    )
    no_match = next(
        record
        for record in no_match_records
        if record.get("relation") == "display_commit_to_camera_frame"
    )
    assert_true(no_match["association_status"] == "no_match", "non-overlap was guessed")

    unmapped_camera = {
        **overlapping_camera[0],
        "frame_seq": 99,
        "host_capture_ns": None,
        "status": "timestamp_error",
        "drop_reason": "camera_clock_conversion_unavailable",
    }
    unmapped_absence_records = build_aligned_timeline(
        clean_alignment(),
        display_commits=display,
        camera_sidecar=[far_camera[0], unmapped_camera],
    )
    unmapped_absence = next(
        record
        for record in unmapped_absence_records
        if record.get("relation") == "display_commit_to_camera_frame"
    )
    assert_true(
        unmapped_absence["association_status"] == "missing_evidence"
        and not unmapped_absence["candidate_record_ids"]
        and unmapped_absence["reason"]
        == "unmapped_camera_sample_prevents_frame_absence_claim",
        f"unmapped camera sample permitted a frame absence claim: {unmapped_absence}",
    )
    unknown_duration_camera = {
        **far_camera[0],
        "frame_seq": 100,
        "host_capture_ns": true_host(3_999_000),
        "duration_ns": None,
        "status": "timestamp_error",
        "drop_reason": "invalid_source_duration",
    }
    unknown_duration_records = build_aligned_timeline(
        clean_alignment(),
        display_commits=display,
        camera_sidecar=[unknown_duration_camera],
    )
    unknown_duration_sample = next(
        record for record in unknown_duration_records if record.get("kind") == "camera_drop"
    )
    unknown_duration_relation = next(
        record
        for record in unknown_duration_records
        if record.get("relation") == "display_commit_to_camera_frame"
    )
    assert_true(
        unknown_duration_sample["alignment_status"] == "missing_evidence"
        and unknown_duration_sample["host_earliest_ns"] is None
        and "camera_frame_duration_unavailable" in unknown_duration_sample["limitations"],
        f"unknown camera duration was collapsed to a point interval: {unknown_duration_sample}",
    )
    assert_true(
        unknown_duration_relation["association_status"] == "missing_evidence"
        and unknown_duration_relation["reason"]
        == "unmapped_camera_sample_prevents_frame_absence_claim",
        f"unknown camera duration permitted a frame absence claim: {unknown_duration_relation}",
    )
    unmapped_unique_records = build_aligned_timeline(
        clean_alignment(),
        display_commits=display,
        camera_sidecar=[overlapping_camera[0], unmapped_camera],
    )
    unmapped_unique = next(
        record
        for record in unmapped_unique_records
        if record.get("relation") == "display_commit_to_camera_frame"
    )
    assert_true(
        unmapped_unique["association_status"] == "missing_evidence"
        and len(unmapped_unique["candidate_record_ids"]) == 1
        and unmapped_unique["reason"]
        == "unmapped_camera_sample_prevents_unique_frame_evidence",
        f"unmapped camera sample permitted a unique frame claim: {unmapped_unique}",
    )

    dropped_camera = [
        {
            **overlapping_camera[0],
            "status": "writer_drop",
            "drop_reason": "writer_backpressure",
        }
    ]
    dropped_records = build_aligned_timeline(
        clean_alignment(), display_commits=display, camera_sidecar=dropped_camera
    )
    dropped = next(
        record
        for record in dropped_records
        if record.get("relation") == "display_commit_to_camera_frame"
    )
    assert_true(
        dropped["association_status"] == "missing_evidence"
        and dropped["reason"] == "only_dropped_frame_intervals_overlap",
        f"unencoded writer drop was claimed as video evidence: {dropped}",
    )
    assert_true(
        any(record.get("kind") == "camera_drop" for record in dropped_records),
        "writer-drop evidence disappeared from the aligned timeline",
    )

    mixed_records = build_aligned_timeline(
        clean_alignment(),
        display_commits=display,
        camera_sidecar=[overlapping_camera[0], dropped_camera[0]],
    )
    mixed = next(
        record
        for record in mixed_records
        if record.get("relation") == "display_commit_to_camera_frame"
    )
    assert_true(
        mixed["association_status"] == "missing_evidence"
        and mixed["reason"] == "overlapping_camera_drop_prevents_unique_frame_evidence",
        f"overlapping camera loss was hidden behind one encoded frame: {mixed}",
    )


def test_repeated_packet_order_and_reported_losses_force_abstention() -> None:
    repeated_host = [
        {
            "state": "notification_accepted",
            "hostMonotonicNs": true_host(3_250_000 + index * 100),
            "characteristic": "alert",
            "payloadHex": "a1b2",
        }
        for index in range(2)
    ]
    repeated_dut = [
        {
            "stage": "BLE_RX",
            "stage_dut_monotonic_us": 3_250_200 + index * 100,
            "clock_segment": "boot-chain",
            "characteristic": "alert",
            "payload_length": 2,
            "exact_payload_hex": "a1b2",
        }
        for index in range(2)
    ]
    repeated_records = build_aligned_timeline(
        clean_alignment(), bench_timeline=repeated_host, causal_trace=repeated_dut
    )
    repeated_relations = [
        record
        for record in repeated_records
        if record.get("relation") == "notification_accepted_to_dut_ble_rx"
    ]
    assert_true(
        len(repeated_relations) == 2
        and all(record["association_status"] == "missing_evidence" for record in repeated_relations),
        f"repeated packets without sequence identity were rank-matched: {repeated_relations}",
    )

    lossy_records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            {
                "state": "notification_accepted",
                "hostMonotonicNs": true_host(3_300_000),
                "globalTxSequence": 50,
                "characteristic": "alert",
                "payloadHex": "ffff",
            }
        ],
        causal_trace=[
            {
                "trace_seq": 4,
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 3_300_100,
                "clock_segment": "boot-chain",
                "rx_first_seq": 8,
                "rx_last_seq": 8,
                "characteristic": "alert",
                "payload_length": 2,
                "exact_payload_hex": "a1b2",
                "lost_trace_records": 1,
            }
        ],
    )
    lossy_relation = next(
        record
        for record in lossy_records
        if record.get("relation") == "notification_accepted_to_dut_ble_rx"
    )
    assert_true(
        lossy_relation["association_status"] == "missing_evidence"
        and lossy_relation["reason"] == "target_evidence_loss_prevents_absence_claim",
        f"reported causal loss became a false packet absence: {lossy_relation}",
    )


def test_packet_correlation_is_scoped_to_current_qualification_session() -> None:
    old_token = "11111111"
    current_token = "22222222"
    stale_payload = "a1b2"
    current_other_payload = "ffff"
    records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            {
                "event": "qstatus_round_trip",
                "phase": "pre_window",
                "status": "observed",
                "host_monotonic_ns": true_host(3_398_000),
                "response": {"state": "idle", "sessionToken": old_token},
            },
            qualification_host_boundaries(
                current_token,
                start_dut_us=3_399_000,
                end_dut_us=3_401_000,
            )[0],
            {
                "state": "notification_accepted",
                "hostMonotonicNs": true_host(3_400_000),
                "globalTxSequence": 1,
                "characteristic": "alert",
                "payloadHex": stale_payload,
            },
            qualification_host_boundaries(
                current_token,
                start_dut_us=3_399_000,
                end_dut_us=3_401_000,
            )[1],
            {
                "event": "qstatus_round_trip",
                "phase": "post_window",
                "status": "observed",
                "host_monotonic_ns": true_host(3_402_000),
                "response": {"state": "idle", "sessionToken": current_token},
            },
        ],
        causal_trace=[
            {
                "trace_seq": 1,
                "qualification_session_token": old_token,
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 50_000,
                "clock_segment": "boot-chain",
                "ble_session_generation": 1,
                "rx_first_seq": 1,
                "rx_last_seq": 1,
                "characteristic": "alert",
                "payload_length": 2,
                "exact_payload_hex": stale_payload,
            },
            {
                "trace_seq": 2,
                "qualification_session_token": current_token,
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 3_400_100,
                "clock_segment": "boot-chain",
                "ble_session_generation": 2,
                "rx_first_seq": 8,
                "rx_last_seq": 8,
                "characteristic": "alert",
                "payload_length": 2,
                "exact_payload_hex": current_other_payload,
            },
            qualification_session_end(
                current_token,
                trace_seq=3,
                end_dut_us=3_401_000,
            ),
        ],
    )
    relation = next(
        record
        for record in records
        if record.get("relation") == "notification_accepted_to_dut_ble_rx"
    )
    assert_true(
        relation["association_status"] == "no_match"
        and relation["reason"] == "packet_identity_not_observed",
        f"current host packet was matched to an older qualification session: {relation}",
    )
    dut_receives = [record for record in records if record.get("kind") == "dut_ble_rx"]
    assert_true(
        len(dut_receives) == 1
        and dut_receives[0]["causal_identifiers"]["qualification_session_token"]
        == current_token,
        f"boot-prefix causal rows were not scoped to the current session: {dut_receives}",
    )


def test_reconnect_preflight_packets_are_outside_qualification_session() -> None:
    current_token = "22222222"
    packet = "a1b2"
    boundaries = qualification_host_boundaries(
        current_token,
        start_dut_us=3_451_000,
        end_dut_us=3_460_000,
    )
    records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            {
                "state": "notification_requested",
                "hostMonotonicNs": true_host(3_449_000),
                "globalTxSequence": 1,
                "characteristic": "alert",
                "payloadHex": packet,
                "timeline_source": "v1replay_reconnect_preflight",
                "source": "v1replay_reconnect_preflight",
            },
            {
                "state": "notification_accepted",
                "hostMonotonicNs": true_host(3_449_100),
                "globalTxSequence": 1,
                "characteristic": "alert",
                "payloadHex": packet,
                "timeline_source": "v1replay_reconnect_preflight",
                "source": "v1replay_reconnect_preflight",
            },
            boundaries[0],
            {
                "state": "notification_requested",
                "hostMonotonicNs": true_host(3_452_000),
                "globalTxSequence": 1,
                "characteristic": "alert",
                "payloadHex": packet,
                "timeline_source": "v1replay",
                "source": "v1replay",
            },
            {
                "state": "notification_accepted",
                "hostMonotonicNs": true_host(3_452_100),
                "globalTxSequence": 1,
                "characteristic": "alert",
                "payloadHex": packet,
                "timeline_source": "v1replay",
                "source": "v1replay",
            },
            boundaries[1],
        ],
        causal_trace=[
            {
                "trace_seq": 1,
                "qualification_session_token": current_token,
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 3_452_100,
                "clock_segment": "boot-chain",
                "ble_session_generation": 2,
                "rx_first_seq": 8,
                "rx_last_seq": 8,
                "characteristic": "alert",
                "payload_length": 2,
                "exact_payload_hex": packet,
            },
            qualification_session_end(
                current_token,
                trace_seq=2,
                end_dut_us=3_460_000,
            ),
        ],
    )
    relations = [
        record
        for record in records
        if record.get("relation") == "notification_accepted_to_dut_ble_rx"
    ]
    assert_true(
        len(relations) == 1 and relations[0]["association_status"] == "matched",
        f"preflight replay packet contaminated current-session matching: {relations}",
    )
    requested_relations = [
        record
        for record in records
        if record.get("relation") == "notification_requested_to_accepted"
    ]
    assert_true(
        len(requested_relations) == 1
        and requested_relations[0]["association_status"] == "matched",
        f"preflight global sequence contaminated scored request acceptance: {requested_relations}",
    )
    outside = [
        record
        for record in records
        if record.get("kind") in {"notification_requested", "notification_accepted"}
        and record.get("qualification_scope") == "outside_current_session"
    ]
    assert_true(
        len(outside) == 2
        and all(
            "outside_current_qualification_session" in record.get("limitations", [])
            for record in outside
        ),
        f"unscored preflight events were not retained with explicit scope: {outside}",
    )


def test_post_qevent_idle_packets_do_not_enter_scored_relations() -> None:
    current_token = "22222222"
    packet = "a1b2"
    boundaries = qualification_host_boundaries(
        current_token,
        start_dut_us=3_470_000,
        end_dut_us=3_480_000,
        terminal_observation_delay_ns=5_000_000,
    )
    records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            boundaries[0],
            {
                "state": "notification_accepted",
                "hostMonotonicNs": true_host(3_475_000),
                "globalTxSequence": 1,
                "characteristic": "alert",
                "payloadHex": packet,
                "timeline_source": "v1replay",
                "source": "v1replay",
            },
            {
                "state": "notification_accepted",
                "hostMonotonicNs": true_host(3_481_000),
                "globalTxSequence": 2,
                "characteristic": "alert",
                "payloadHex": packet,
                "timeline_source": "v1replay",
                "source": "v1replay",
            },
            boundaries[1],
        ],
        causal_trace=[
            {
                "trace_seq": 1,
                "qualification_session_token": current_token,
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 3_475_100,
                "clock_segment": "boot-chain",
                "ble_session_generation": 2,
                "rx_first_seq": 8,
                "rx_last_seq": 8,
                "characteristic": "alert",
                "payload_length": 2,
                "exact_payload_hex": packet,
            },
            qualification_session_end(
                current_token,
                trace_seq=2,
                end_dut_us=3_480_000,
            ),
        ],
    )
    relations = [
        record
        for record in records
        if record.get("relation") == "notification_accepted_to_dut_ble_rx"
    ]
    assert_true(
        len(relations) == 1 and relations[0]["association_status"] == "matched",
        f"post-QEVENT idle packet contaminated the scored packet group: {relations}",
    )
    post_window = [
        record
        for record in records
        if record.get("kind") == "notification_accepted"
        and record.get("qualification_scope") == "outside_current_session"
    ]
    assert_true(
        len(post_window) == 1
        and "qualification_session_token"
        not in post_window[0]["causal_identifiers"],
        f"post-QEVENT idle packet retained the scored session token: {post_window}",
    )


def test_missing_host_session_bounds_force_packet_abstention() -> None:
    current_token = "22222222"
    packet = "a1b2"
    records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            {
                "event": "qstatus_round_trip",
                "phase": "post_window",
                "status": "observed",
                "host_monotonic_ns": true_host(3_490_000),
                "response": {"state": "idle", "sessionToken": current_token},
            },
            {
                "state": "notification_accepted",
                "hostMonotonicNs": true_host(3_485_000),
                "globalTxSequence": 1,
                "characteristic": "alert",
                "payloadHex": packet,
                "timeline_source": "v1replay",
                "source": "v1replay",
            },
        ],
        causal_trace=[
            {
                "trace_seq": 1,
                "qualification_session_token": current_token,
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 3_485_100,
                "clock_segment": "boot-chain",
                "ble_session_generation": 2,
                "rx_first_seq": 8,
                "rx_last_seq": 8,
                "characteristic": "alert",
                "payload_length": 2,
                "exact_payload_hex": packet,
            },
            qualification_session_end(
                current_token,
                trace_seq=2,
                end_dut_us=3_490_000,
            ),
        ],
    )
    relation = next(
        record
        for record in records
        if record.get("relation") == "notification_accepted_to_dut_ble_rx"
    )
    accepted = next(record for record in records if record.get("kind") == "notification_accepted")
    assert_true(
        relation["association_status"] == "missing_evidence"
        and relation["reason"] == "collision_resistant_packet_identity_incomplete"
        and accepted["qualification_scope"] == "indeterminate_current_session"
        and "qualification_session_bounds_incomplete" in accepted["limitations"],
        f"missing QSTART/QEVENT bounds permitted a packet match: {relation}, {accepted}",
    )


def test_filtered_session_ignores_later_boot_terminal_metadata() -> None:
    current_token = "22222222"
    terminal_metadata = {"terminal_seq": "12", "dropped_commits": "0"}
    commits = [
        {
            "seq": sequence,
            "qualification_session_token": token,
            "clock_segment": "boot-chain",
            "render_request_dut_micros": dut_us,
            "display_commit_dut_micros": dut_us + 50,
            "dropped_commits": dropped,
            "__csv_metadata__": terminal_metadata,
        }
        for sequence, token, dut_us, dropped in (
            (10, current_token, 3_460_000, 0),
            (11, current_token, 3_461_000, 0),
            (12, "00000000", 3_462_000, 0),
        )
    ]
    records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            {
                "event": "qstatus_round_trip",
                "phase": "post_window",
                "status": "observed",
                "host_monotonic_ns": true_host(3_463_000),
                "response": {"state": "idle", "sessionToken": current_token},
            }
        ],
        display_commits=commits,
    )
    aligned_commits = [record for record in records if record.get("kind") == "display_commit"]
    assert_true(
        len(aligned_commits) == 2
        and all(record["evidence_complete"] is True for record in aligned_commits),
        f"later boot-wide terminal metadata became current-session loss: {aligned_commits}",
    )

    missing_tail_metadata = {"terminal_seq": "13", "dropped_commits": "1"}
    lossy_commits = [
        {
            **record,
            "__csv_metadata__": missing_tail_metadata,
        }
        for record in commits[:2]
    ] + [
        {
            **commits[2],
            "seq": 13,
            "dropped_commits": 1,
            "__csv_metadata__": missing_tail_metadata,
        }
    ]
    lossy_records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            {
                "event": "qstatus_round_trip",
                "phase": "post_window",
                "status": "observed",
                "host_monotonic_ns": true_host(3_464_000),
                "response": {"state": "idle", "sessionToken": current_token},
            }
        ],
        display_commits=lossy_commits,
    )
    lossy_aligned = [
        record for record in lossy_records if record.get("kind") == "display_commit"
    ]
    assert_true(
        lossy_aligned
        and all(record["evidence_complete"] is False for record in lossy_aligned)
        and all(
            "terminal_loss_counter_increased" in record["evidence_loss_reasons"]
            for record in lossy_aligned
        ),
        f"unexplained boot-tail loss was suppressed: {lossy_aligned}",
    )


def test_filtered_causal_session_preserves_terminal_loss() -> None:
    current_token = "22222222"
    metadata = {"terminal_trace_seq": "12", "lost_trace_records": "1"}
    records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            {
                "event": "qstatus_round_trip",
                "phase": "post_window",
                "status": "observed",
                "host_monotonic_ns": true_host(3_490_000),
                "response": {"state": "idle", "sessionToken": current_token},
            }
        ],
        causal_trace=[
            {
                "trace_seq": 10,
                "qualification_session_token": current_token,
                "stage": "SESSION_START",
                "stage_dut_monotonic_us": 3_485_000,
                "clock_segment": "boot-chain",
                "lost_trace_records": 0,
                "__csv_metadata__": metadata,
            },
            {
                "trace_seq": 11,
                "qualification_session_token": current_token,
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 3_486_000,
                "clock_segment": "boot-chain",
                "lost_trace_records": 0,
                "__csv_metadata__": metadata,
            },
        ],
    )
    dut_rows = [
        record
        for record in records
        if record.get("kind") in {"dut_causal_event", "dut_ble_rx"}
    ]
    assert_true(
        dut_rows
        and all(record["evidence_complete"] is False for record in dut_rows)
        and all(
            "terminal_sequence_does_not_match_rows" in record["evidence_loss_reasons"]
            and "terminal_loss_counter_increased" in record["evidence_loss_reasons"]
            for record in dut_rows
        ),
        f"dropped final causal trace record was hidden by session filtering: {dut_rows}",
    )


def test_incomplete_target_identity_and_nonphysical_commits_force_abstention() -> None:
    packet_records = build_aligned_timeline(
        clean_alignment(),
        bench_timeline=[
            {
                "state": "notification_accepted",
                "hostMonotonicNs": true_host(3_500_000),
                "globalTxSequence": 20,
                "characteristic": "alert",
                "payloadHex": "a1b2",
            }
        ],
        causal_trace=[
            {
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 3_500_100,
                "clock_segment": "boot-chain",
                "ble_session_generation": 9,
                "rx_first_seq": 4,
                "rx_last_seq": 4,
                "characteristic": "alert",
                "payload_length": 2,
            }
        ],
    )
    packet_relation = next(
        record
        for record in packet_records
        if record.get("relation") == "notification_accepted_to_dut_ble_rx"
    )
    assert_true(
        packet_relation["association_status"] == "missing_evidence"
        and packet_relation["reason"] == "compatible_target_packet_identity_incomplete",
        f"incomplete target packet identity became a no-match: {packet_relation}",
    )

    rx_parse_records = build_aligned_timeline(
        clean_alignment(),
        causal_trace=[
            {
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 3_600_000,
                "clock_segment": "boot-chain",
                "ble_session_generation": 9,
                "rx_first_seq": 5,
                "rx_last_seq": 5,
                "characteristic": "alert",
                "payload_length": 2,
                "exact_payload_hex": "a1b2",
            },
            {
                "stage": "PACKET_PARSE",
                "stage_dut_monotonic_us": 3_600_100,
                "clock_segment": "boot-chain",
                "ble_session_generation": 9,
                "rx_first_seq": 5,
            },
        ],
    )
    rx_parse_relation = next(
        record
        for record in rx_parse_records
        if record.get("relation") == "dut_ble_rx_to_packet_parse"
    )
    assert_true(
        rx_parse_relation["association_status"] == "missing_evidence"
        and rx_parse_relation["reason"] == "compatible_parse_identity_incomplete",
        f"incomplete parse range became a no-match: {rx_parse_relation}",
    )

    invalid_rx_range_records = build_aligned_timeline(
        clean_alignment(),
        causal_trace=[
            {
                "stage": "BLE_RX",
                "stage_dut_monotonic_us": 3_650_000,
                "clock_segment": "boot-chain",
                "ble_session_generation": 9,
                "rx_first_seq": 7,
                "rx_last_seq": 6,
                "characteristic": "alert",
                "payload_length": 2,
                "exact_payload_hex": "a1b2",
            },
            {
                "stage": "PACKET_PARSE",
                "stage_dut_monotonic_us": 3_650_100,
                "clock_segment": "boot-chain",
                "ble_session_generation": 9,
                "rx_first_seq": 6,
                "rx_last_seq": 7,
            },
        ],
    )
    invalid_rx_relation = next(
        record
        for record in invalid_rx_range_records
        if record.get("relation") == "dut_ble_rx_to_packet_parse"
    )
    assert_true(
        invalid_rx_relation["association_status"] == "missing_evidence"
        and invalid_rx_relation["reason"]
        == "receive_sequence_identity_incomplete_or_invalid",
        f"invalid receive range was treated as causal identity: {invalid_rx_relation}",
    )

    generic_records = build_aligned_timeline(
        clean_alignment(),
        causal_trace=[
            {
                "stage": "STATE_PUBLISH",
                "stage_dut_monotonic_us": 3_700_000,
                "clock_segment": "boot-chain",
                "state_revision": 5,
            }
        ],
        display_commits=[
            {
                "clock_segment": "boot-chain",
                "render_request_dut_micros": 3_700_100,
                "display_commit_dut_micros": 0,
                "pushes": 0,
                "state_revision": 6,
            },
            {
                "clock_segment": "boot-chain",
                "render_request_dut_micros": 3_700_200,
                "display_commit_dut_micros": 0,
                "pushes": 0,
            },
        ],
    )
    generic_relation = next(
        record for record in generic_records if record.get("relation") == "state_revision_to_render"
    )
    assert_true(
        generic_relation["association_status"] == "missing_evidence"
        and generic_relation["reason"] == "compatible_target_identity_incomplete",
        f"mixed complete/incomplete render identities produced a false no-match: {generic_relation}",
    )
    no_transfer = [
        record
        for record in generic_records
        if record.get("kind") == "display_commit"
        and "no_physical_display_transfer" in record.get("limitations", [])
    ]
    assert_true(
        len(no_transfer) == 2
        and all(record["alignment_status"] == "missing_evidence" for record in no_transfer),
        f"zero-push display records invented physical commit times: {no_transfer}",
    )


def test_flexible_csv_reader_and_exclusive_artifact_generation() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        run_dir = Path(temporary)
        csv_path = run_dir / "future.csv"
        csv_path.write_text(
            "# causal_trace_schema=9,timebase=dut_us\n"
            "stage,stage_dut_monotonic_us,clock_segment,future_column\n"
            "BLE_RX,1234,boot-future,preserved\n",
            encoding="utf-8",
        )
        rows = read_csv_records(csv_path)
        assert_true(rows[0]["future_column"] == "preserved", "future CSV column was rejected")
        assert_true(rows[0]["__source_record__"] == 3, "physical CSV source line was lost")
        assert_true(
            rows[0]["__csv_metadata__"]["causal_trace_schema"] == "9",
            "versioned CSV metadata was lost",
        )
        perf_path = run_dir / "perf.csv"
        perf_path.write_text(
            "millis,dutMicros,clockSegment\n#session_start,seq=1,token=PERF0001\n1,1999999,boot-chain\n"
            "millis,dutMicros,clockSegment\n#session_start,seq=2,token=PERF0002\n2,2000000,boot-chain\n3,2100000,boot-chain\n4,2100001,boot-chain\n",
            encoding="utf-8",
        )
        perf_rows = read_csv_records(perf_path)
        assert_true(
            [(row["__source_record__"], row["__csv_session__"].get("token")) for row in perf_rows]
            == [(3, "PERF0001"), (6, "PERF0002"), (7, "PERF0002"), (8, "PERF0002")],
            f"perf sessions or physical lines were misparsed: {perf_rows}",
        )
        qualification_token = "22222222"
        scoped = build_aligned_timeline(
            clean_alignment(),
            causal_trace=[
                {"trace_seq": 1, "qualification_session_token": qualification_token, "stage": "SESSION_START", "stage_dut_micros": 2_000_000, "clock_segment": "boot-chain"},
                qualification_session_end(qualification_token, trace_seq=2, end_dut_us=2_100_000),
            ],
            perf_csv=perf_path,
        )
        samples = [record for record in scoped if record["kind"] == "metric_sample"]
        assert_true([record["source_record"] for record in samples] == [6, 7], f"perf rows escaped exact qualification bounds: {samples}")
        fallback = build_aligned_timeline(
            clean_alignment(), bench_timeline=qualification_host_boundaries(qualification_token, start_dut_us=2_000_000, end_dut_us=2_100_000), perf_csv=perf_path
        )
        fallback_samples = [record for record in fallback if record["kind"] == "metric_sample"]
        assert_true([record["source_record"] for record in fallback_samples] == [6, 7], f"best-effort causal loss discarded perf rows: {fallback_samples}")
        camera_path = run_dir / "camera" / "frame_timing.ndjson"
        camera_path.parent.mkdir()
        _, _, display, camera = complete_chain_inputs()
        frame = dict(camera[0])
        frame.update({"phase": "window", "source_clock": "avcapture_session_synchronization_clock", "callback_clock": "host_monotonic", "source_duration_value": 5_000, "source_duration_timescale": 1_000_000, "video_pts_value": 0, "video_duration_value": 5_000, "video_duration_timescale": 1_000_000})
        camera_cases = (("{bad-json\n", True), (json.dumps({**frame, "status": []}), True), (json.dumps(frame), False))
        for payload, verified in camera_cases:
            camera_path.write_text(payload + ("" if payload.endswith("\n") else "\n"), encoding="utf-8")
            camera_records = build_aligned_timeline(clean_alignment(), display_commits=display, camera_sidecar=camera_path, camera_verified=verified)
            relation = next(record for record in camera_records if record.get("relation") == "display_commit_to_camera_frame")
            assert_true(relation["association_status"] == "missing_evidence" and not relation["candidate_record_ids"], f"invalid camera evidence matched or expanded candidates: {relation}")
        camera_path.write_text(json.dumps(frame) + "\n", encoding="utf-8")

        timeline_path = run_dir / "bench_timeline.ndjson"
        with timeline_path.open("w", encoding="utf-8") as handle:
            for exchange in qsync_fixture("boot-generated", count=8):
                handle.write(json.dumps(exchange, sort_keys=True) + "\n")
        result = generate_alignment_artifacts(run_dir, camera_result={"frame_timing": camera_path.name, "video_timing_verification_result": {"status": "verified"}})
        assert_true((run_dir / "clock_alignment.json").is_file(), "clock alignment was not written")
        aligned_path = run_dir / "aligned_timeline.ndjson"
        assert_true(aligned_path.is_file(), "aligned timeline was not written")
        lines = [json.loads(line) for line in aligned_path.read_text(encoding="utf-8").splitlines()]
        assert_true(result["aligned_timeline"]["record_count"] == len(lines), "record count is inaccurate")
        assert_true(any(record["kind"] == "camera_frame" and record["alignment_status"] == "mapped" for record in lines), "verified nested camera sidecar was not resolved")
        assert_true(
            all(str(run_dir) not in json.dumps(record) for record in lines),
            "absolute temporary path leaked into aligned evidence",
        )
        try:
            generate_alignment_artifacts(run_dir)
        except FileExistsError:
            pass
        else:
            raise AssertionError("existing immutable derived artifacts were overwritten")


def main() -> int:
    tests = (
        test_run_window_qsync_names_robust_fit_bounds_and_outliers,
        test_late_qsync_reply_is_rejoined_by_nonce,
        test_raw_serial_qsync_reply_is_joined_once_after_timeout,
        test_clock_segment_changes_reboots_and_offset_only,
        test_midpoint_validity_is_conservative_under_asymmetric_delay,
        test_exchange_status_unavailable_fit_and_exact_mapping_fields,
        test_complete_replay_to_display_to_frame_chain_and_required_schema,
        test_repeated_missing_packets_and_explicit_interval_outcomes,
        test_repeated_packet_order_and_reported_losses_force_abstention,
        test_packet_correlation_is_scoped_to_current_qualification_session,
        test_reconnect_preflight_packets_are_outside_qualification_session,
        test_post_qevent_idle_packets_do_not_enter_scored_relations,
        test_missing_host_session_bounds_force_packet_abstention,
        test_filtered_session_ignores_later_boot_terminal_metadata,
        test_filtered_causal_session_preserves_terminal_loss,
        test_incomplete_target_identity_and_nonphysical_commits_force_abstention,
        test_flexible_csv_reader_and_exclusive_artifact_generation,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} bench time-alignment tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
