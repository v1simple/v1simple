#!/usr/bin/env python3
"""Deterministic, dependency-free alignment of DUT and host monotonic clocks.

The fitter consumes four-timestamp exchanges (H1, D2, D3, H4), keeps the
original exchange records, and creates one mapping for each uninterrupted DUT
clock segment.  It deliberately refuses to extrapolate beyond observed DUT
time so callers cannot accidentally turn a weak fit into precise evidence.
"""

from __future__ import annotations

import math
from fractions import Fraction
from typing import Any, Iterable, Mapping, Sequence


CLOCK_ALIGNMENT_SCHEMA_VERSION = 1
NOMINAL_NS_PER_US = Fraction(1000, 1)
UNCERTAINTY_TARGET_NS = 2_500_000
MIN_AFFINE_OBSERVATIONS = 4
MIN_AFFINE_SPAN_US = 1_000_000
SELECTION_BUCKETS = 8
SUCCESSFUL_EXCHANGE_STATUSES = frozenset(
    {"observed", "complete", "completed", "ok", "success", "successful", "valid"}
)


_ALIASES: dict[str, tuple[str, ...]] = {
    "exchange_id": ("exchange_id", "nonce", "qsync_nonce", "request_nonce"),
    "clock_segment": (
        "clock_segment",
        "clock_segment_id",
        "boot_clock_id",
        "boot_clock_segment",
        "segment_id",
    ),
    "h1_ns": (
        "h1_host_monotonic_ns",
        "h1_host_ns",
        "h1_ns",
        "host_send_ns",
        "request_write_host_ns",
    ),
    "d2_us": (
        "d2_dut_monotonic_us",
        "d2_dut_us",
        "d2_us",
        "dut_parse_us",
        "request_parse_dut_us",
    ),
    "d3_us": (
        "d3_dut_monotonic_us",
        "d3_dut_us",
        "d3_us",
        "dut_reply_us",
        "reply_enqueue_dut_us",
    ),
    "h4_ns": (
        "h4_host_monotonic_ns",
        "h4_host_ns",
        "h4_ns",
        "host_receive_ns",
        "reply_receive_host_ns",
    ),
}


def _first(record: Mapping[str, Any], names: Sequence[str]) -> Any:
    for name in names:
        if name in record:
            return record[name]
    return None


def _integer(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 10)
        except ValueError:
            return None
    return None


def _median(values: Sequence[Fraction]) -> Fraction:
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


def _round_fraction(value: Fraction) -> int:
    """Round half away from zero, independent of the host Python runtime."""
    numerator = abs(value.numerator)
    quotient, remainder = divmod(numerator, value.denominator)
    if remainder * 2 >= value.denominator:
        quotient += 1
    return -quotient if value < 0 else quotient


def _floor_fraction(value: Fraction) -> int:
    return value.numerator // value.denominator


def _ceil_fraction(value: Fraction) -> int:
    return -((-value.numerator) // value.denominator)


def _theil_sen(points: Sequence[tuple[Fraction, Fraction]]) -> Fraction | None:
    slopes: list[Fraction] = []
    for left_index, (left_x, left_y) in enumerate(points):
        for right_x, right_y in points[left_index + 1 :]:
            if right_x != left_x:
                slopes.append((right_y - left_y) / (right_x - left_x))
    return _median(slopes) if slopes else None


def _normalise_exchange(record: Mapping[str, Any], source_record: int) -> dict[str, Any]:
    exchange_id = _first(record, _ALIASES["exchange_id"])
    segment = _first(record, _ALIASES["clock_segment"])
    h1 = _integer(_first(record, _ALIASES["h1_ns"]))
    d2 = _integer(_first(record, _ALIASES["d2_us"]))
    d3 = _integer(_first(record, _ALIASES["d3_us"]))
    h4 = _integer(_first(record, _ALIASES["h4_ns"]))

    errors: list[str] = []
    raw_status = record.get("status")
    if raw_status is not None and (
        not isinstance(raw_status, str)
        or raw_status.strip().lower() not in SUCCESSFUL_EXCHANGE_STATUSES
    ):
        errors.append("unsuccessful_exchange_status")
    if segment is None or isinstance(segment, (dict, list)):
        errors.append("missing_clock_segment")
    if h1 is None or d2 is None or d3 is None or h4 is None:
        errors.append("missing_or_non_integer_timestamp")
    elif min(h1, d2, d3, h4) < 0:
        errors.append("negative_timestamp")
    elif h4 < h1 or d3 < d2:
        errors.append("non_monotonic_exchange")
    elif Fraction(h4 - h1) - NOMINAL_NS_PER_US * (d3 - d2) < 0:
        errors.append("negative_adjusted_round_trip")

    return {
        "source_record": source_record,
        "exchange_id": exchange_id,
        "clock_segment": None if segment is None else str(segment),
        "h1_host_monotonic_ns": h1,
        "d2_dut_monotonic_us": d2,
        "d3_dut_monotonic_us": d3,
        "h4_host_monotonic_ns": h4,
        "status": "valid" if not errors else "invalid",
        "errors": errors,
        "raw_exchange": dict(record),
    }


def _point(exchange: Mapping[str, Any]) -> tuple[Fraction, Fraction]:
    return (
        Fraction(exchange["d2_dut_monotonic_us"] + exchange["d3_dut_monotonic_us"], 2),
        Fraction(exchange["h1_host_monotonic_ns"] + exchange["h4_host_monotonic_ns"], 2),
    )


def _adjusted_rtt(exchange: Mapping[str, Any], slope: Fraction) -> Fraction:
    return Fraction(exchange["h4_host_monotonic_ns"] - exchange["h1_host_monotonic_ns"]) - (
        slope
        * (exchange["d3_dut_monotonic_us"] - exchange["d2_dut_monotonic_us"])
    )


def _select_low_delay(
    exchanges: Sequence[dict[str, Any]], provisional_slope: Fraction
) -> list[dict[str, Any]]:
    ordered = sorted(exchanges, key=lambda item: (_point(item)[0], item["source_record"]))
    bucket_count = min(SELECTION_BUCKETS, len(ordered))
    selected: list[dict[str, Any]] = []
    for bucket in range(bucket_count):
        start = bucket * len(ordered) // bucket_count
        stop = (bucket + 1) * len(ordered) // bucket_count
        candidates = ordered[start:stop]
        selected.append(
            min(
                candidates,
                key=lambda item: (_adjusted_rtt(item, provisional_slope), item["source_record"]),
            )
        )
    return selected


def _predict(
    slope: Fraction, reference_dut_us: int, reference_host_ns: Fraction, dut_us: int
) -> Fraction:
    return reference_host_ns + slope * (dut_us - reference_dut_us)


def _fit_segment(
    segment: str,
    segment_instance: int,
    exchanges: Sequence[dict[str, Any]],
) -> dict[str, Any]:
    mapping_id = f"{segment}:{segment_instance}"
    valid = [item for item in exchanges if item["status"] == "valid"]
    base: dict[str, Any] = {
        "schema_version": CLOCK_ALIGNMENT_SCHEMA_VERSION,
        "kind": "dut_host_clock_mapping",
        "mapping_id": mapping_id,
        "clock_segment": segment,
        "segment_instance": segment_instance,
        "exchange_count": len(exchanges),
        "valid_exchange_count": len(valid),
        "selected_source_records": [],
        "fit_status": "unavailable",
        "fit_type": "unavailable",
        "uncertainty_target_ns": UNCERTAINTY_TARGET_NS,
        "uncertainty_smaller_than_2_5_ms": False,
    }
    if not valid:
        base["limitations"] = ["no_valid_four_timestamp_exchange"]
        return base

    points = [_point(item) for item in valid]
    provisional_slope = _theil_sen(points) or NOMINAL_NS_PER_US
    if provisional_slope <= 0:
        provisional_slope = NOMINAL_NS_PER_US
    selected = _select_low_delay(valid, provisional_slope)
    selected_points = [_point(item) for item in selected]
    selected_span = max(point[0] for point in selected_points) - min(
        point[0] for point in selected_points
    )
    affine_slope = _theil_sen(selected_points)
    limitations: list[str] = []
    if (
        len(selected) >= MIN_AFFINE_OBSERVATIONS
        and selected_span >= MIN_AFFINE_SPAN_US
        and affine_slope is not None
        and affine_slope > 0
    ):
        slope = affine_slope
        fit_type = "affine"
    else:
        slope = NOMINAL_NS_PER_US
        fit_type = "offset_only"
        if len(selected) < MIN_AFFINE_OBSERVATIONS:
            limitations.append("insufficient_observations_for_drift")
        if selected_span < MIN_AFFINE_SPAN_US:
            limitations.append("insufficient_dut_span_for_drift")
        if affine_slope is not None and affine_slope <= 0:
            limitations.append("nonpositive_affine_slope")

    reference_dut_us = _round_fraction(_median([point[0] for point in selected_points]))
    centered_hosts = [
        y - slope * (x - reference_dut_us) for x, y in selected_points
    ]
    reference_host = _median(centered_hosts)

    # At an exchange midpoint the true affine host time lies between H1 and H4,
    # regardless of request/reply asymmetry.  The difference between the true
    # affine map and this fitted affine map is itself affine, so bounds that hold
    # at the first and last observed midpoints hold everywhere between them.
    # Restricting validity to that midpoint span avoids asserting a bound at the
    # D2/D3 edges where fitted-slope error can otherwise escape the envelope.
    # All valid observations contribute: queueing outliers may make the result
    # wide, but cannot be silently hidden from consumers.
    lower_errors = [
        Fraction(item["h1_host_monotonic_ns"])
        - _predict(slope, reference_dut_us, reference_host, _point(item)[0])
        for item in valid
    ]
    upper_errors = [
        Fraction(item["h4_host_monotonic_ns"])
        - _predict(slope, reference_dut_us, reference_host, _point(item)[0])
        for item in valid
    ]
    lower_error = min(lower_errors)
    upper_error = max(upper_errors)
    if lower_error > upper_error:
        base["limitations"] = [*limitations, "inverted_uncertainty_envelope"]
        return base

    selected_records = {item["source_record"] for item in selected}
    residuals: list[Fraction] = []
    for item in valid:
        x, y = _point(item)
        residual = y - _predict(slope, reference_dut_us, reference_host, x)
        residuals.append(residual)
        item["selected_for_fit"] = item["source_record"] in selected_records
        item["adjusted_round_trip_ns"] = _round_fraction(_adjusted_rtt(item, slope))
        item["offset_lower_ns"] = _floor_fraction(
            Fraction(item["h1_host_monotonic_ns"])
            - slope * item["d2_dut_monotonic_us"]
        )
        item["offset_upper_ns"] = _ceil_fraction(
            Fraction(item["h4_host_monotonic_ns"])
            - slope * item["d3_dut_monotonic_us"]
        )
        item["midpoint_residual_ns"] = _round_fraction(residual)

    earliest_error_bound = _floor_fraction(lower_error)
    latest_error_bound = _ceil_fraction(upper_error)
    # Mapping adds these integral error bounds to an exact rational estimate,
    # then rounds outward once more. A fractional estimate can therefore add
    # one nanosecond to the stored-bound width. Report that worst case so the
    # 2.5 ms claim can never be stronger than a returned timestamp interval.
    uncertainty_width = latest_error_bound - earliest_error_bound + 1
    maximum_uncertainty = max(abs(earliest_error_bound), abs(latest_error_bound)) + 1
    maximum_residual = _ceil_fraction(max(abs(value) for value in residuals))
    selected_residuals = [
        residual
        for item, residual in zip(valid, residuals, strict=True)
        if item["source_record"] in selected_records
    ]
    maximum_selected_residual = _ceil_fraction(
        max(abs(value) for value in selected_residuals)
    )
    validity_start = _ceil_fraction(min(point[0] for point in points))
    validity_end = _floor_fraction(max(point[0] for point in points))
    if validity_start > validity_end:
        base["limitations"] = [*limitations, "no_integer_timestamp_in_observed_midpoint_span"]
        return base
    drift_ppm = (slope / NOMINAL_NS_PER_US - 1) * 1_000_000
    exact_offset = reference_host - slope * reference_dut_us

    base.update(
        {
            "fit_status": "fitted",
            "fit_type": fit_type,
            "slope_ns_per_us": float(slope),
            "slope_numerator": slope.numerator,
            "slope_denominator": slope.denominator,
            "offset_host_ns": float(exact_offset),
            "offset_host_ns_numerator": exact_offset.numerator,
            "offset_host_ns_denominator": exact_offset.denominator,
            "reference_dut_us": reference_dut_us,
            "reference_host_ns": _round_fraction(reference_host),
            "reference_host_ns_numerator": reference_host.numerator,
            "reference_host_ns_denominator": reference_host.denominator,
            "drift_ppm": float(drift_ppm),
            "validity_dut_us": {"start": validity_start, "end": validity_end},
            "selected_source_records": sorted(selected_records),
            "maximum_residual_ns": maximum_residual,
            "maximum_selected_residual_ns": maximum_selected_residual,
            "host_error_bounds_ns": {
                "earliest": earliest_error_bound,
                "latest": latest_error_bound,
            },
            "uncertainty_width_ns": uncertainty_width,
            "maximum_uncertainty_ns": maximum_uncertainty,
            "uncertainty_smaller_than_2_5_ms": uncertainty_width < UNCERTAINTY_TARGET_NS,
            "poor_fit": uncertainty_width >= UNCERTAINTY_TARGET_NS
            or maximum_selected_residual >= UNCERTAINTY_TARGET_NS,
            "fit_quality": (
                "poor"
                if uncertainty_width >= UNCERTAINTY_TARGET_NS
                or maximum_selected_residual >= UNCERTAINTY_TARGET_NS
                else ("limited" if fit_type == "offset_only" else "good")
            ),
            "limitations": limitations,
        }
    )
    return base


def fit_clock_alignment(exchanges: Iterable[Mapping[str, Any]]) -> dict[str, Any]:
    """Fit uninterrupted DUT-clock segments and retain every raw exchange."""
    normalised = [
        _normalise_exchange(record, index)
        for index, record in enumerate(exchanges, start=1)
    ]

    # A clock-segment identifier that reappears after another segment is a new
    # mapping instance.  Records without a segment are retained but never fit.
    occurrence_counts: dict[str, int] = {}
    current_segment: str | None = None
    current_instance: int | None = None
    pending_invalid_segment: str | None = None
    pending_invalid_instance: int | None = None
    grouped: dict[tuple[str, int], list[dict[str, Any]]] = {}
    group_order: list[tuple[str, int]] = []

    def new_group(segment: str) -> tuple[str, int]:
        occurrence_counts[segment] = occurrence_counts.get(segment, 0) + 1
        key = (segment, occurrence_counts[segment])
        grouped[key] = []
        group_order.append(key)
        return key

    for item in normalised:
        segment = item["clock_segment"]
        if segment is None:
            item["segment_instance"] = None
            pending_invalid_segment = None
            pending_invalid_instance = None
            continue
        if item["status"] == "valid":
            if segment == current_segment and current_instance is not None:
                key = (segment, current_instance)
            elif segment == pending_invalid_segment and pending_invalid_instance is not None:
                key = (segment, pending_invalid_instance)
                current_segment = segment
                current_instance = pending_invalid_instance
            else:
                key = new_group(segment)
                current_segment, current_instance = key
            pending_invalid_segment = None
            pending_invalid_instance = None
        elif segment == current_segment and current_instance is not None:
            key = (segment, current_instance)
        elif segment == pending_invalid_segment and pending_invalid_instance is not None:
            key = (segment, pending_invalid_instance)
        else:
            key = new_group(segment)
            pending_invalid_segment, pending_invalid_instance = key
        item["segment_instance"] = key[1]
        grouped[key].append(item)

    segments = [
        _fit_segment(segment, instance, grouped[(segment, instance)])
        for segment, instance in group_order
    ]
    by_mapping = {segment["mapping_id"]: segment for segment in segments}
    for item in normalised:
        if item.get("segment_instance") is not None:
            mapping_id = f"{item['clock_segment']}:{item['segment_instance']}"
            item["mapping_id"] = mapping_id
            item["fit_status"] = by_mapping[mapping_id]["fit_status"]

    return {
        "schema_version": CLOCK_ALIGNMENT_SCHEMA_VERSION,
        "kind": "bench_clock_alignment",
        "model": "centered_segmented_theil_sen_four_timestamp",
        "nominal_ns_per_us": float(NOMINAL_NS_PER_US),
        "uncertainty_target_ns": UNCERTAINTY_TARGET_NS,
        "raw_exchange_count": len(normalised),
        "raw_exchanges": normalised,
        "segments": segments,
    }


def map_dut_timestamp(
    alignment: Mapping[str, Any],
    clock_segment: Any,
    dut_timestamp_us: Any,
    *,
    segment_instance: int | None = None,
) -> dict[str, Any]:
    """Map one DUT timestamp without extrapolation.

    Reused segment identifiers are resolved by validity interval.  If those
    intervals overlap, callers must provide ``segment_instance``; the function
    returns ``ambiguous`` rather than guessing.
    """
    dut_us = _integer(dut_timestamp_us)
    if dut_us is None or dut_us < 0:
        return {"status": "invalid_timestamp"}
    if clock_segment is None:
        return {"status": "missing_evidence", "reason": "missing_clock_segment"}
    segment_text = str(clock_segment)
    segments = alignment.get("segments")
    if not isinstance(segments, list):
        return {"status": "missing_evidence", "reason": "missing_clock_alignment"}

    candidates: list[Mapping[str, Any]] = []
    known_segment = False
    known_instance = False
    fitted_mapping_seen = False
    for mapping in segments:
        if not isinstance(mapping, Mapping) or mapping.get("clock_segment") != segment_text:
            continue
        known_segment = True
        if segment_instance is not None and mapping.get("segment_instance") != segment_instance:
            continue
        known_instance = True
        validity = mapping.get("validity_dut_us")
        if mapping.get("fit_status") != "fitted" or not isinstance(validity, Mapping):
            continue
        fitted_mapping_seen = True
        start = _integer(validity.get("start"))
        end = _integer(validity.get("end"))
        if start is not None and end is not None and start <= dut_us <= end:
            candidates.append(mapping)

    if not candidates:
        if not known_segment:
            return {"status": "missing_evidence", "reason": "unknown_clock_segment"}
        if not known_instance:
            return {"status": "missing_evidence", "reason": "unknown_segment_instance"}
        if not fitted_mapping_seen:
            return {"status": "missing_evidence", "reason": "unavailable_fit"}
        return {
            "status": "out_of_range",
            "reason": "outside_mapping_validity",
        }
    if len(candidates) > 1:
        return {
            "status": "ambiguous",
            "reason": "overlapping_reused_segment_validity",
            "mapping_ids": [item.get("mapping_id") for item in candidates],
        }

    mapping = candidates[0]
    slope_value = mapping.get("slope_ns_per_us")
    slope_numerator = _integer(mapping.get("slope_numerator"))
    slope_denominator = _integer(mapping.get("slope_denominator"))
    reference_dut = _integer(mapping.get("reference_dut_us"))
    reference_host = _integer(mapping.get("reference_host_ns"))
    reference_host_numerator = _integer(mapping.get("reference_host_ns_numerator"))
    reference_host_denominator = _integer(mapping.get("reference_host_ns_denominator"))
    bounds = mapping.get("host_error_bounds_ns")
    if (
        not isinstance(slope_value, (int, float))
        or isinstance(slope_value, bool)
        or not math.isfinite(float(slope_value))
        or float(slope_value) <= 0
        or slope_numerator is None
        or slope_denominator is None
        or slope_denominator <= 0
        or reference_dut is None
        or reference_host is None
        or reference_host_numerator is None
        or reference_host_denominator is None
        or reference_host_denominator <= 0
        or not isinstance(bounds, Mapping)
    ):
        return {"status": "missing_evidence", "reason": "invalid_clock_mapping"}
    earliest_error = _integer(bounds.get("earliest"))
    latest_error = _integer(bounds.get("latest"))
    if earliest_error is None or latest_error is None:
        return {"status": "missing_evidence", "reason": "missing_uncertainty_bounds"}

    exact_slope = Fraction(slope_numerator, slope_denominator)
    exact_reference_host = Fraction(reference_host_numerator, reference_host_denominator)
    exact_estimate = exact_reference_host + exact_slope * (dut_us - reference_dut)
    estimate = _round_fraction(exact_estimate)
    earliest = _floor_fraction(exact_estimate + earliest_error)
    latest = _ceil_fraction(exact_estimate + latest_error)
    actual_uncertainty_width = latest - earliest
    return {
        "status": "mapped",
        "mapping_id": mapping.get("mapping_id"),
        "fit_type": mapping.get("fit_type"),
        "fit_quality": mapping.get("fit_quality"),
        "poor_fit": mapping.get("poor_fit"),
        "uncertainty_width_ns": actual_uncertainty_width,
        "uncertainty_smaller_than_2_5_ms": actual_uncertainty_width
        < UNCERTAINTY_TARGET_NS,
        "host_estimate_ns": estimate,
        "host_earliest_ns": earliest,
        "host_latest_ns": latest,
    }
