"""Frozen, expectation-bounded classifier for captured V1 arrow phase edges.

The classifier never assigns a value to an ambiguous source image.  It emits a
separate sequence record only when a maximal raw refusal lies on the measured
line between two permitted, source-consecutive blink phases.  Selection and
capture integrity are established by the caller before this module sees data.
"""
from __future__ import annotations

from copy import deepcopy
import math
import re


CLASSIFIER_ID = "v1-arrow-phase-edge-v4"
CLASSIFIER_SPEC_SHA256 = "aa60e40aa6433a5be3bd3f89b25fe2e9a95bab9e16646e41ec10133730975587"
PROFILE_READER_METHOD_VERSION = 7
PROFILE_READER_SHA256 = "f4efd6a1df4daefb3e7271a2e229e80f7378ba8b7a824dea1593aa0581f20b7c"

AUTHORED_BLINK_PHASE_NS = 96_000_000
ENDPOINT_SEPARATION_RMS_MIN = 52.0
PROJECTION_MIN = -0.05
PROJECTION_MAX = 1.05
NORMALIZED_RESIDUAL_MAX = 0.15
MAXIMUM_BACKWARD_STEP = 0.05
MAXIMUM_TOTAL_BACKWARD_MOTION = 0.10
EXTRA_DIRECTION_PROFILE_DIAMETER_RMS_MAX = 8.0
STABLE_SUPPORT_FRAMES_EACH_SIDE = 2

_DIRECTIONS = ("front", "side", "rear")
_DEFINITE_STATES = {"filled", "unlit"}
_TRANSITION_STATES = {"partial", "faint"}
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


def _expectation_signature(sample):
    expected = sample.get("expected")
    if not isinstance(expected, dict) or expected.get("input", {}).get("ready") is not True:
        return None
    fields = expected.get("fields")
    spec = fields.get("main_arrows") if isinstance(fields, dict) else None
    allowed = spec.get("allowed") if isinstance(spec, dict) else None
    phases = expected.get("joint_states")
    if not isinstance(allowed, list) or not isinstance(phases, list):
        return None
    allowed_sets = [_direction_set(value) for value in allowed]
    phase_sets = [_direction_set(phase.get("main_arrows")) if isinstance(phase, dict) else None
                  for phase in phases]
    if (any(value is None for value in (*allowed_sets, *phase_sets)) or
            len(set(allowed_sets)) != 2 or len(set(phase_sets)) != 2 or
            set(allowed_sets) != set(phase_sets)):
        return None
    return {"allowed_arrow_sets": [list(value) for value in sorted(set(allowed_sets))],
            "joint_arrow_phases": [list(value) for value in sorted(set(phase_sets))]}


def _profile(detail):
    profile = detail.get("profile") if isinstance(detail, dict) else None
    if not isinstance(profile, dict):
        return None
    values = profile.get("max_channel_medians")
    bounds = profile.get("reference_bounds")
    if (profile.get("rows") != 4 or profile.get("columns") != 4 or
            not isinstance(values, list) or len(values) != 16 or
            any(type(value) not in (int, float) or isinstance(value, bool)
                or not math.isfinite(value) or not 0 <= value <= 255 for value in values) or
            not isinstance(bounds, list) or len(bounds) != 4 or
            any(type(value) is not int for value in bounds)):
        return None
    return tuple(float(value) for value in values), tuple(bounds)


def _definite_reading(sample):
    reading = _reading(sample)
    value = _direction_set(reading.get("value"))
    directions = reading.get("direction_states")
    if reading.get("state") != "readable" or value is None or not isinstance(directions, dict):
        return None
    profiles = {}
    for direction in _DIRECTIONS:
        detail = directions.get(direction)
        expected_state = "filled" if direction in value else "unlit"
        measured = _profile(detail)
        if not isinstance(detail, dict) or detail.get("state") != expected_state or measured is None:
            return None
        profiles[direction] = measured
    return value, profiles


def _transition_reading(sample, changed_direction, endpoint_value):
    reading = _reading(sample)
    directions = reading.get("direction_states")
    if reading.get("state") != "ambiguous" or not isinstance(directions, dict):
        return None
    profiles = {}
    ambiguous = []
    for direction in _DIRECTIONS:
        detail = directions.get(direction)
        measured = _profile(detail)
        if not isinstance(detail, dict) or measured is None:
            return None
        state = detail.get("state")
        if state in _TRANSITION_STATES:
            ambiguous.append(direction)
        elif state not in _DEFINITE_STATES:
            return None
        if direction != changed_direction:
            expected_state = "filled" if direction in endpoint_value else "unlit"
            if state != expected_state:
                return None
        profiles[direction] = measured
    return profiles if ambiguous == [changed_direction] else None


def _consecutive(left, right, maximum_gap_ns):
    return (type(left.get("video_frame_index")) is int and
            type(left.get("source_frame_seq")) is int and
            right.get("video_frame_index") == left["video_frame_index"] + 1 and
            right.get("source_frame_seq") == left["source_frame_seq"] + 1 and
            type(left.get("capture_ns")) is int and type(right.get("capture_ns")) is int and
            0 < right["capture_ns"] - left["capture_ns"] <= maximum_gap_ns)


def _rms(values):
    return math.sqrt(sum(value * value for value in values) / len(values))


def _diameter(profiles):
    return max((_rms([a - b for a, b in zip(left, right)])
                for index, left in enumerate(profiles) for right in profiles[index + 1:]), default=0.0)


def _context(value):
    if not isinstance(value, dict):
        raise ValueError("arrow classifier context is missing")
    required = ("capture_id", "selection_manifest_sha256", "verified_maximum_source_interval_ns",
                "reader_method_version", "reader_sha256")
    if any(key not in value for key in required):
        raise ValueError("arrow classifier context is incomplete")
    if (_SHA256.fullmatch(str(value["capture_id"])) is None or
            _SHA256.fullmatch(str(value["selection_manifest_sha256"])) is None or
            type(value["verified_maximum_source_interval_ns"]) is not int or
            not 0 < value["verified_maximum_source_interval_ns"] <= 1_000_000_000 or
            value["reader_method_version"] != PROFILE_READER_METHOD_VERSION or
            value["reader_sha256"] != PROFILE_READER_SHA256):
        raise ValueError("arrow classifier context does not match the frozen reader/capture contract")
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


def _classify(event_id, selected, first, stop, context):
    run = selected[first:stop]
    if first < STABLE_SUPPORT_FRAMES_EACH_SIDE or stop + STABLE_SUPPORT_FRAMES_EACH_SIDE > len(selected):
        return None, _reject(event_id, run, "UNCLOSED_RUN", "arrow run lacks two readable support frames on each side")
    chain = selected[first - 2:stop + 2]
    maximum_gap = context["verified_maximum_source_interval_ns"]
    if not all(_consecutive(left, right, maximum_gap) for left, right in zip(chain, chain[1:])):
        return None, _reject(event_id, run, "SOURCE_GAP", "arrow support chain crosses an unobserved source position")
    left_support, left_endpoint = chain[0], chain[1]
    right_endpoint, right_support = chain[-2], chain[-1]
    if right_endpoint["capture_ns"] - left_endpoint["capture_ns"] > AUTHORED_BLINK_PHASE_NS + maximum_gap:
        return None, _reject(event_id, run, "ENDPOINT_SPAN", "arrow endpoints exceed one blink phase plus the verified source interval")
    definite = [_definite_reading(sample) for sample in (left_support, left_endpoint,
                                                          right_endpoint, right_support)]
    if any(value is None for value in definite):
        return None, _reject(event_id, run, "UNCLOSED_RUN", "arrow support is not definite and readable")
    left_value, left_profiles = definite[1]
    right_value, right_profiles = definite[2]
    if definite[0][0] != left_value or definite[3][0] != right_value:
        return None, _reject(event_id, run, "UNSTABLE_ENDPOINT", "same-side arrow support does not match its endpoint")
    changed = set(left_value) ^ set(right_value)
    if len(changed) != 1:
        return None, _reject(event_id, run, "NOT_ONE_DIRECTION_PHASE_EDGE", "arrow endpoints do not differ by exactly one direction")
    changed_direction = next(iter(changed))
    signatures = [_expectation_signature(sample) for sample in chain]
    signature = signatures[0]
    endpoint_sets = {left_value, right_value}
    signature_sets = ({tuple(value) for value in signature["allowed_arrow_sets"]}
                      if signature is not None else set())
    if signature is None or any(value != signature for value in signatures) or signature_sets != endpoint_sets:
        return None, _reject(event_id, run, "EXPECTATION_SIGNATURE", "support chain lacks one identical exact two-phase arrow expectation")
    run_profiles = []
    for sample in run:
        profiles = _transition_reading(sample, changed_direction, left_value)
        if profiles is None:
            return None, _reject(event_id, run, "EXTRA_DIRECTION_STATE", "raw arrow refusal is not solely the changed direction")
        run_profiles.append(profiles)
    all_profiles = [definite[0][1], left_profiles, *run_profiles, right_profiles, definite[3][1]]
    references = {direction: {profiles[direction][1] for profiles in all_profiles}
                  for direction in _DIRECTIONS}
    if any(len(bounds) != 1 for bounds in references.values()):
        return None, _reject(event_id, run, "INVALID_PROFILE", "arrow profile reference bounds change inside support")
    # The nearest readable image can already be partway through a fade. Fit
    # the entire fixed support chain, including those readable inner images,
    # between its outer supports. Never search for brighter anchors.
    left = all_profiles[0][changed_direction][0]
    right = all_profiles[-1][changed_direction][0]
    delta = [b - a for a, b in zip(left, right)]
    separation = _rms(delta)
    if separation < ENDPOINT_SEPARATION_RMS_MIN:
        return None, _reject(event_id, run, "ENDPOINT_SEPARATION", "arrow endpoint profiles are not sufficiently separated")
    denominator = sum(value * value for value in delta)
    projections, residuals = [], []
    for profiles in all_profiles[1:-1]:
        current = profiles[changed_direction][0]
        alpha = sum((value - base) * change for value, base, change in zip(current, left, delta)) / denominator
        fitted = [base + alpha * change for base, change in zip(left, delta)]
        residual = _rms([value - fit for value, fit in zip(current, fitted)]) / separation
        projections.append(alpha)
        residuals.append(residual)
    if any(not PROJECTION_MIN <= value <= PROJECTION_MAX for value in projections):
        return None, _reject(event_id, run, "PROJECTION_RANGE", "arrow transition projects beyond its endpoints")
    if any(value > NORMALIZED_RESIDUAL_MAX for value in residuals):
        return None, _reject(event_id, run, "NORMALIZED_RESIDUAL", "arrow transition shape is not an endpoint-profile interpolation")
    path = [0.0, *projections, 1.0]
    backwards = [max(0.0, left_alpha - right_alpha) for left_alpha, right_alpha in zip(path, path[1:])]
    maximum_backward = max(backwards, default=0.0)
    total_backward = sum(backwards)
    if maximum_backward > MAXIMUM_BACKWARD_STEP:
        return None, _reject(event_id, run, "MAXIMUM_BACKWARD_STEP", "arrow transition has a large backward projection step")
    if total_backward > MAXIMUM_TOTAL_BACKWARD_MOTION:
        return None, _reject(event_id, run, "TOTAL_BACKWARD_MOTION", "arrow transition has excessive cumulative backward motion")
    extra_diameters = {direction: _diameter([profiles[direction][0] for profiles in all_profiles])
                       for direction in _DIRECTIONS if direction != changed_direction}
    if any(value > EXTRA_DIRECTION_PROFILE_DIAMETER_RMS_MAX for value in extra_diameters.values()):
        return None, _reject(event_id, run, "EXTRA_DIRECTION_MOTION", "an unchanged arrow direction moves beyond the frozen profile bound")
    record = {
        "event_id": event_id,
        "classifier_id": CLASSIFIER_ID,
        "classifier_spec_sha256": CLASSIFIER_SPEC_SHA256,
        "status": "QUALIFIED_CAPTURE_TRANSITION",
        "raw_affected_fields": ["main_arrows"],
        "video_frame_indices": [sample["video_frame_index"] for sample in run],
        "first": _point(run[0]), "last": _point(run[-1]),
        "left_support": _point(left_support), "left_endpoint": _point(left_endpoint),
        "right_endpoint": _point(right_endpoint), "right_support": _point(right_support),
        "endpoint_values": [list(left_value), list(right_value)],
        "changed_direction": changed_direction,
        "arrow_expectation_signature": deepcopy(signature),
        "endpoint_separation_rms": separation,
        "profile_frame_indices": [sample["video_frame_index"] for sample in chain[1:-1]],
        "projections": projections,
        "normalized_residuals": residuals,
        "maximum_backward_step": maximum_backward,
        "total_backward_motion": total_backward,
        "extra_direction_profile_diameter_rms": extra_diameters,
        "maximum_endpoint_span_ns": AUTHORED_BLINK_PHASE_NS + maximum_gap,
        "verified_maximum_source_interval_ns": maximum_gap,
        "profile_schema": {"rows": 4, "columns": 4, "cells": 16,
                           "sample": "max-channel cell median"},
        "profile_reference_bounds": {direction: list(next(iter(bounds)))
                                     for direction, bounds in references.items()},
        "capture_id": context["capture_id"],
        "selection_manifest_sha256": context["selection_manifest_sha256"],
        "reader_method_version": context["reader_method_version"],
        "reader_sha256": context["reader_sha256"],
        "basis": "The whole fixed support chain, including its readable inner images, follows the measured monotone profile path between its outer supports in two permitted blink phases; only the maximal raw refusal is classified and raw frames remain unresolved.",
    }
    return record, None


def classify_arrow_runs(samples, events, context):
    """Return frozen arrow classifications and explicit rejected candidates."""
    result = {"classifications": [], "rejected_runs": [], "errors": []}
    try:
        context = _context(context)
        if not isinstance(events, list):
            raise ValueError("arrow events are not a list")
        for event in events:
            if not isinstance(event, dict) or not isinstance(event.get("event_id"), str):
                raise ValueError("arrow event is malformed")
            start, end = event.get("start_ns"), event.get("end_ns")
            if type(start) is not int or type(end) is not int or start >= end:
                raise ValueError("arrow event bounds are invalid")
            selected = [sample for sample in samples if type(sample.get("capture_ns")) is int
                        and start <= sample["capture_ns"] < end]
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
