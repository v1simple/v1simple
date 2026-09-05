"""Classify a captured outgoing/incoming arrow target redraw.

This classifier supplies one-way failure evidence for the functional deadline.
It never assigns a value to an ambiguous frame and cannot make a frame count as
current.  A record is emitted only when the raw refusal lies inside a bounded,
source-consecutive optical path from a prior-target arrow state to a permitted
current-target state.
"""
from __future__ import annotations

from copy import deepcopy
import math
import re


CLASSIFIER_ID = "v1-arrow-target-acquisition-v1"
CLASSIFIER_SPEC_SHA256 = "f1773d6904f24c16285df435311fbcfb713ca37516ab807fafff0cb7b015b64d"
DEADLINE_OBSERVATION_SEMANTICS = "TARGET_ACQUISITION_TRANSITION"
PROFILE_READER_METHOD_VERSION = 7
PROFILE_READER_SHA256 = "f4efd6a1df4daefb3e7271a2e229e80f7378ba8b7a824dea1593aa0581f20b7c"

MAXIMUM_RECORDING_SOURCE_INTERVAL_NS = 1_000_000_000
MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS = 10_000_000
AUTHORED_DISPLAY_UPDATE_NS = 50_000_000
SUPPORT_SEARCH_FRAMES_EACH_SIDE = 8
ENDPOINT_SEPARATION_RMS_MIN = 52.0
PROJECTION_MIN = -0.05
PROJECTION_MAX = 1.05
NORMALIZED_RESIDUAL_MAX = 0.50
MAXIMUM_BACKWARD_STEP = 0.05
MAXIMUM_TOTAL_BACKWARD_MOTION = 0.10
STABLE_PAIR_PROFILE_DIAMETER_RMS_MAX = 8.0
UNCHANGED_DIRECTION_PROFILE_DIAMETER_RMS_MAX = 8.0

_DIRECTIONS = ("front", "side", "rear")
_DEFINITE_STATES = {"filled", "unlit"}
_TRANSITION_STATES = _DEFINITE_STATES | {"partial", "faint"}
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image")


def _point(sample):
    return {key: deepcopy(sample[key]) for key in _POINT_KEYS if key in sample}


def _reject(event_id, run, code, reason):
    return {"event_id": event_id, "field": "main_arrows", "code": code,
            "first": _point(run[0]), "last": _point(run[-1]), "reason": reason}


def _reading(sample):
    fields = sample.get("observed", {}).get("fields", {})
    value = fields.get("main_arrows") if isinstance(fields, dict) else None
    return value if isinstance(value, dict) else {}


def _direction_set(value):
    if (not isinstance(value, list) or any(item not in _DIRECTIONS for item in value)
            or len(value) != len(set(value))):
        return None
    return tuple(sorted(value))


def _arrow_sets(field):
    allowed = field.get("allowed") if isinstance(field, dict) else None
    if not isinstance(allowed, list) or not allowed:
        return None
    values = [_direction_set(item) for item in allowed]
    if any(item is None for item in values) or len(values) != len(set(values)):
        return None
    return tuple(sorted(values))


def _expectation_signature(sample):
    expected = sample.get("expected")
    if not isinstance(expected, dict) or expected.get("input", {}).get("ready") is not True:
        return None
    fields = expected.get("fields")
    previous = expected.get("previous_input")
    previous_fields = previous.get("fields") if isinstance(previous, dict) else None
    current_sets = _arrow_sets(fields.get("main_arrows") if isinstance(fields, dict) else None)
    previous_sets = _arrow_sets(
        previous_fields.get("main_arrows") if isinstance(previous_fields, dict) else None)
    if current_sets is None or previous_sets is None or current_sets == previous_sets:
        return None
    return {
        "previous_arrow_sets": [list(value) for value in previous_sets],
        "current_arrow_sets": [list(value) for value in current_sets],
    }


def _profile(detail):
    profile = detail.get("profile") if isinstance(detail, dict) else None
    values = profile.get("max_channel_medians") if isinstance(profile, dict) else None
    bounds = profile.get("reference_bounds") if isinstance(profile, dict) else None
    if (not isinstance(profile, dict) or profile.get("rows") != 4
            or profile.get("columns") != 4 or not isinstance(values, list)
            or len(values) != 16 or any(type(value) not in (int, float)
                                        or isinstance(value, bool)
                                        or not math.isfinite(value)
                                        or not 0 <= value <= 255 for value in values)
            or not isinstance(bounds, list) or len(bounds) != 4
            or any(type(value) is not int for value in bounds)):
        return None
    return tuple(float(value) for value in values), tuple(bounds)


def _arrow_measurement(sample, *, definite):
    reading = _reading(sample)
    value = _direction_set(reading.get("value")) if definite else None
    directions = reading.get("direction_states")
    if (not isinstance(directions, dict)
            or (definite and (reading.get("state") != "readable" or value is None))
            or (not definite and reading.get("state") not in {"readable", "ambiguous"})):
        return None
    profiles, states = {}, {}
    for direction in _DIRECTIONS:
        detail = directions.get(direction)
        measured = _profile(detail)
        state = detail.get("state") if isinstance(detail, dict) else None
        if measured is None or state not in (_DEFINITE_STATES if definite else _TRANSITION_STATES):
            return None
        if definite and state != ("filled" if direction in value else "unlit"):
            return None
        profiles[direction] = measured
        states[direction] = state
    return {"value": value, "profiles": profiles, "states": states}


def _consecutive(left, right, maximum_gap_ns):
    return (type(left.get("video_frame_index")) is int
            and type(left.get("source_frame_seq")) is int
            and right.get("video_frame_index") == left["video_frame_index"] + 1
            and right.get("source_frame_seq") == left["source_frame_seq"] + 1
            and type(left.get("capture_ns")) is int and type(right.get("capture_ns")) is int
            and 0 < right["capture_ns"] - left["capture_ns"] <= maximum_gap_ns)


def _rms(values):
    return math.sqrt(sum(value * value for value in values) / len(values))


def _diameter(profiles):
    return max((_rms([a - b for a, b in zip(left, right)])
                for index, left in enumerate(profiles)
                for right in profiles[index + 1:]), default=0.0)


def _stable_pair(left, right, maximum_gap_ns):
    if not _consecutive(left, right, maximum_gap_ns):
        return None
    measured = [_arrow_measurement(item, definite=True) for item in (left, right)]
    if any(item is None for item in measured) or measured[0]["value"] != measured[1]["value"]:
        return None
    for direction in _DIRECTIONS:
        if measured[0]["profiles"][direction][1] != measured[1]["profiles"][direction][1]:
            return None
        if _diameter([item["profiles"][direction][0] for item in measured]) \
                > STABLE_PAIR_PROFILE_DIAMETER_RMS_MAX:
            return None
    return measured


def _nearest_left_pair(selected, first, maximum_gap_ns):
    lower = max(1, first - SUPPORT_SEARCH_FRAMES_EACH_SIDE)
    for right in range(first - 1, lower - 1, -1):
        measured = _stable_pair(selected[right - 1], selected[right], maximum_gap_ns)
        if measured is not None:
            return right - 1, right, measured
    return None


def _nearest_right_pair(selected, stop, maximum_gap_ns):
    upper = min(len(selected) - 1, stop + SUPPORT_SEARCH_FRAMES_EACH_SIDE)
    for left in range(stop, upper):
        measured = _stable_pair(selected[left], selected[left + 1], maximum_gap_ns)
        if measured is not None:
            return left, left + 1, measured
    return None


def _context(value):
    if not isinstance(value, dict):
        raise ValueError("arrow acquisition classifier context is missing")
    required = ("capture_id", "selection_manifest_sha256",
                "verified_maximum_source_interval_ns", "reader_method_version", "reader_sha256")
    if any(key not in value for key in required):
        raise ValueError("arrow acquisition classifier context is incomplete")
    if (_SHA256.fullmatch(str(value["capture_id"])) is None
            or _SHA256.fullmatch(str(value["selection_manifest_sha256"])) is None
            or type(value["verified_maximum_source_interval_ns"]) is not int
            or not 0 < value["verified_maximum_source_interval_ns"]
                       <= MAXIMUM_RECORDING_SOURCE_INTERVAL_NS
            or value["reader_method_version"] != PROFILE_READER_METHOD_VERSION
            or value["reader_sha256"] != PROFILE_READER_SHA256):
        raise ValueError("arrow acquisition classifier context does not match the frozen reader/capture contract")
    return value


def _runs(samples):
    index = 0
    while index < len(samples):
        if _reading(samples[index]).get("state") != "ambiguous":
            index += 1
            continue
        end = index + 1
        while end < len(samples) and _reading(samples[end]).get("state") == "ambiguous":
            end += 1
        yield index, end
        index = end


def _claimable(sample):
    comparison = sample.get("comparison")
    checks = comparison.get("checks") if isinstance(comparison, dict) else None
    arrow = checks.get("main_arrows") if isinstance(checks, dict) else None
    return (isinstance(arrow, dict) and arrow.get("status") == "UNRESOLVED"
            and all(isinstance(check, dict)
                    and check.get("status") in {"MATCH", "UNRESOLVED"}
                    for check in checks.values()))


def _endpoint_pair(left_value, right_value, signature):
    previous = {tuple(value) for value in signature["previous_arrow_sets"]}
    current = {tuple(value) for value in signature["current_arrow_sets"]}
    if right_value not in current or left_value in current:
        return None
    for prior in sorted(previous):
        if prior == right_value:
            continue
        if left_value in {prior, tuple(sorted(set(prior) | set(right_value)))}:
            return {"previous_phase": list(prior), "current_phase": list(right_value),
                    "left_phase": list(left_value), "right_phase": list(right_value)}
    return None


def _path_metrics(left, middle, right):
    delta = [b - a for a, b in zip(left, right)]
    separation = _rms(delta)
    if separation < ENDPOINT_SEPARATION_RMS_MIN:
        return None, "ENDPOINT_SEPARATION", "arrow acquisition endpoints lack optical separation"
    denominator = sum(value * value for value in delta)
    projections, residuals = [], []
    for current in middle:
        alpha = sum((value - base) * change
                    for value, base, change in zip(current, left, delta)) / denominator
        fitted = [base + alpha * change for base, change in zip(left, delta)]
        residual = _rms([value - fit for value, fit in zip(current, fitted)]) / separation
        projections.append(alpha)
        residuals.append(residual)
    if any(not PROJECTION_MIN <= value <= PROJECTION_MAX for value in projections):
        return None, "PROJECTION_RANGE", "arrow acquisition path leaves its endpoint range"
    if any(value > NORMALIZED_RESIDUAL_MAX for value in residuals):
        return None, "NORMALIZED_RESIDUAL", "arrow acquisition shape is not an endpoint interpolation"
    path = [0.0, *projections, 1.0]
    backwards = [max(0.0, left_alpha - right_alpha)
                 for left_alpha, right_alpha in zip(path, path[1:])]
    if max(backwards, default=0.0) > MAXIMUM_BACKWARD_STEP:
        return None, "MAXIMUM_BACKWARD_STEP", "arrow acquisition has a large backward step"
    if sum(backwards) > MAXIMUM_TOTAL_BACKWARD_MOTION:
        return None, "TOTAL_BACKWARD_MOTION", "arrow acquisition has cumulative backward motion"
    return {"endpoint_separation_rms": separation, "projections": projections,
            "normalized_residuals": residuals,
            "maximum_backward_step": max(backwards, default=0.0),
            "total_backward_motion": sum(backwards)}, None, None


def _classify(event_id, selected, first, stop, context):
    raw_run = selected[first:stop]
    gap = min(context["verified_maximum_source_interval_ns"],
              MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS)
    left_pair = _nearest_left_pair(selected, first, gap)
    right_pair = _nearest_right_pair(selected, stop, gap)
    if left_pair is None or right_pair is None:
        return None, _reject(event_id, raw_run, "UNCLOSED_RUN",
                             "arrow acquisition lacks a nearby stable pair on each side")
    left_first, left_endpoint_index, left_measured = left_pair
    right_endpoint_index, right_last, right_measured = right_pair
    chain = selected[left_first:right_last + 1]
    if not all(_consecutive(left, right, gap) for left, right in zip(chain, chain[1:])):
        return None, _reject(event_id, raw_run, "SOURCE_GAP",
                             "arrow acquisition support crosses an unobserved source position")
    left_endpoint = selected[left_endpoint_index]
    right_endpoint = selected[right_endpoint_index]
    if right_endpoint["capture_ns"] - left_endpoint["capture_ns"] \
            > AUTHORED_DISPLAY_UPDATE_NS + gap:
        return None, _reject(event_id, raw_run, "ENDPOINT_SPAN",
                             "arrow acquisition endpoints exceed one display update plus the source interval")

    signatures = [_expectation_signature(sample) for sample in chain]
    signature = signatures[0]
    if signature is None or any(value != signature for value in signatures):
        return None, _reject(event_id, raw_run, "EXPECTATION_SIGNATURE",
                             "arrow acquisition lacks one unchanged prior/current expectation")
    left_value = left_measured[1]["value"]
    right_value = right_measured[0]["value"]
    endpoint_phase = _endpoint_pair(left_value, right_value, signature)
    if endpoint_phase is None:
        return None, _reject(event_id, raw_run, "NOT_ACQUISITION_ENDPOINTS",
                             "nearest stable arrow states do not run from prior-target content to current content")

    claim = [sample for sample in raw_run if _claimable(sample)]
    if not claim:
        return None, _reject(event_id, raw_run, "NO_PRODUCT_CLAIM",
                             "arrow acquisition has no product-unresolved member")
    if any(right["video_frame_index"] != left["video_frame_index"] + 1
           for left, right in zip(claim, claim[1:])):
        return None, _reject(event_id, raw_run, "NONCONTIGUOUS_PRODUCT_CLAIM",
                             "arrow acquisition product members are not contiguous")

    transition = selected[left_endpoint_index + 1:right_endpoint_index]
    measured = [_arrow_measurement(sample, definite=False) for sample in transition]
    if any(item is None for item in measured):
        return None, _reject(event_id, raw_run, "TRANSITION_READING",
                             "arrow acquisition contains an unreadable or malformed transition member")
    changed = tuple(sorted(set(left_value) ^ set(right_value)))
    if not changed:
        return None, _reject(event_id, raw_run, "NOT_ACQUISITION_ENDPOINTS",
                             "arrow acquisition endpoints are identical")
    all_profiles = [left_measured[0]["profiles"], left_measured[1]["profiles"],
                    *(item["profiles"] for item in measured),
                    right_measured[0]["profiles"], right_measured[1]["profiles"]]
    bounds = {direction: {profiles[direction][1] for profiles in all_profiles}
              for direction in _DIRECTIONS}
    if any(len(value) != 1 for value in bounds.values()):
        return None, _reject(event_id, raw_run, "INVALID_PROFILE",
                             "arrow acquisition profile bounds change inside support")

    metrics = {}
    for direction in changed:
        result, code, reason = _path_metrics(
            left_measured[1]["profiles"][direction][0],
            [item["profiles"][direction][0] for item in measured],
            right_measured[0]["profiles"][direction][0])
        if result is None:
            return None, _reject(event_id, raw_run, code, f"{direction}: {reason}")
        metrics[direction] = result
    unchanged = {
        direction: _diameter([profiles[direction][0] for profiles in all_profiles])
        for direction in _DIRECTIONS if direction not in changed
    }
    if any(value > UNCHANGED_DIRECTION_PROFILE_DIAMETER_RMS_MAX
           for value in unchanged.values()):
        return None, _reject(event_id, raw_run, "UNCHANGED_DIRECTION_MOTION",
                             "an unchanged arrow direction moves beyond the frozen profile bound")

    transition_by_index = {
        sample["video_frame_index"]: measurement
        for sample, measurement in zip(transition, measured)
    }
    claimed_frame_proof = []
    for sample in claim:
        measurement = transition_by_index.get(sample["video_frame_index"])
        if measurement is None:
            return None, _reject(
                event_id, raw_run, "TRANSITION_READING",
                "a claimed arrow frame is not inside the measured transition")
        states = {direction: measurement["states"][direction] for direction in changed}
        noncurrent = sorted(
            direction for direction, state in states.items()
            if state != ("filled" if direction in right_value else "unlit"))
        if not noncurrent:
            return None, _reject(
                event_id, raw_run, "CLAIMED_FRAME_AT_CURRENT_ENDPOINT",
                "a claimed arrow frame has the current endpoint state in every changed direction")
        claimed_frame_proof.append({
            "video_frame_index": sample["video_frame_index"],
            "changed_direction_states": states,
            "noncurrent_changed_directions": noncurrent,
        })

    return {
        "event_id": event_id,
        "classifier_id": CLASSIFIER_ID,
        "classifier_spec_sha256": CLASSIFIER_SPEC_SHA256,
        "status": "QUALIFIED_CAPTURE_TRANSITION",
        "deadline_observation_semantics": DEADLINE_OBSERVATION_SEMANTICS,
        "raw_affected_fields": ["main_arrows"],
        "video_frame_indices": [sample["video_frame_index"] for sample in claim],
        "first": _point(claim[0]), "last": _point(claim[-1]),
        "full_transition_indices": [sample["video_frame_index"] for sample in transition],
        "left_support": [_point(selected[left_first]), _point(left_endpoint)],
        "right_support": [_point(right_endpoint), _point(selected[right_last])],
        "endpoint_values": [list(left_value), list(right_value)],
        "endpoint_phase_basis": endpoint_phase,
        "changed_directions": list(changed),
        "arrow_expectation_signature": deepcopy(signature),
        "direction_metrics": metrics,
        "claimed_frame_acquisition_proof": claimed_frame_proof,
        "unchanged_direction_profile_diameter_rms": unchanged,
        "maximum_endpoint_span_ns": AUTHORED_DISPLAY_UPDATE_NS + gap,
        "support_search_frames_each_side": SUPPORT_SEARCH_FRAMES_EACH_SIDE,
        "verified_maximum_source_interval_ns": context["verified_maximum_source_interval_ns"],
        "profile_schema": {"rows": 4, "columns": 4, "cells": 16,
                           "sample": "max-channel cell median"},
        "profile_reference_bounds": {direction: list(next(iter(value)))
                                     for direction, value in bounds.items()},
        "capture_id": context["capture_id"],
        "selection_manifest_sha256": context["selection_manifest_sha256"],
        "reader_method_version": context["reader_method_version"],
        "reader_sha256": context["reader_sha256"],
        "basis": "A raw arrow refusal lies inside a bounded monotone optical path from prior-target arrow content to one permitted current-target state; it proves target acquisition was still in progress and leaves every raw frame unresolved.",
    }, None


def classify_arrow_acquisition_runs(samples, events, context):
    """Return target-acquisition arrow records and explicit refusals."""
    result = {"classifications": [], "rejected_runs": [], "errors": []}
    try:
        context = _context(context)
        if not isinstance(events, list):
            raise ValueError("arrow acquisition events are not a list")
        for event in events:
            if (not isinstance(event, dict) or not isinstance(event.get("event_id"), str)
                    or type(event.get("start_ns")) is not int
                    or type(event.get("end_ns")) is not int
                    or event["start_ns"] >= event["end_ns"]):
                raise ValueError("arrow acquisition event is malformed")
            selected = [sample for sample in samples
                        if type(sample.get("capture_ns")) is int
                        and event["start_ns"] <= sample["capture_ns"] < event["end_ns"]]
            for first, stop in _runs(selected):
                record, rejected = _classify(event["event_id"], selected, first, stop, context)
                if record is not None:
                    result["classifications"].append(record)
                else:
                    result["rejected_runs"].append(rejected)
    except (KeyError, TypeError, ValueError) as exc:
        result["classifications"] = []
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    return result
