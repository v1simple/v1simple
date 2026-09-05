"""Classifier for a captured adjacent main-bar redraw.

The ordinary frame reader remains the only owner of single-frame values.  This
module never assigns a bar count to an ambiguous image.  It records a sequence
interpretation only when a maximal raw refusal is bracketed by stable,
source-consecutive adjacent counts and the expectation-blind redraw profiles
follow the measured path between those endpoints.
"""
from __future__ import annotations

from copy import deepcopy
import math
import re


CLASSIFIER_ID = "v1-main-bar-adjacent-redraw-v2"
CLASSIFIER_SPEC_SHA256 = "2db75d016eb3962137489d636a9090d541c0f303452aa6bce83f517da71cc170"
CLASSIFIER_STATUS = "QUALIFIED_CAPTURE_TRANSITION"

PROFILE_READER_METHOD_VERSION = 5
PROFILE_READER_SHA256 = "3427f8bb80fe25f4d88b4709113f353a60ebb6eef8ae1c18c7326fc8b1259e55"
PROFILE_REDRAW_PROBE_METHOD_VERSION = 1
PROFILE_REDRAW_PROBE_SHA256 = "4b623360af63afe5c51f154ed5ce57325476b8419bc165bc8a5ee9ef7f55da24"

MAXIMUM_RECORDING_SOURCE_INTERVAL_NS = 1_000_000_000
MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS = 10_000_000
AUTHORED_DISPLAY_UPDATE_NS = 50_000_000
STABLE_SUPPORT_FRAMES_EACH_SIDE = 2
ENDPOINT_SEPARATION_RMS_MIN = 20.0
PROJECTION_MIN = -0.05
PROJECTION_MAX = 1.05
NORMALIZED_RESIDUAL_MAX = 0.15
MAXIMUM_BACKWARD_STEP = 0.05
MAXIMUM_TOTAL_BACKWARD_MOTION = 0.10
BOUNDARY_MEDIAN_BACKWARD_TOLERANCE = 2.0
UNCHANGED_CELL_PROFILE_DIAMETER_RMS_MAX = 8.0

MAIN_BAR_BOXES = tuple((900, y, 937, y + 10)
                       for y in (400, 363, 326, 289, 251, 214))
PROFILE_SCHEMA = {"rows": 4, "columns": 4,
                  "sample": "max-channel cell median"}
RAW_AMBIGUOUS_REASON = "partial or noncontiguous strength bars"

_SHA256 = re.compile(r"[0-9a-f]{64}")
_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image")
_FILL_STATES = {"on", "off", "partial"}


def _point(sample):
    return {key: deepcopy(sample[key]) for key in _POINT_KEYS if key in sample}


def _reject(event_id, run, code, reason):
    return {"event_id": event_id, "field": "main_bars", "code": code,
            "first": _point(run[0]), "last": _point(run[-1]), "reason": reason}


def _reading(sample):
    fields = sample.get("observed", {}).get("fields", {})
    value = fields.get("main_bars") if isinstance(fields, dict) else None
    return value if isinstance(value, dict) else {}


def _number(value):
    return (type(value) in (int, float) and not isinstance(value, bool)
            and math.isfinite(value) and 0 <= value <= 255)


def _rms(values):
    return math.sqrt(sum(value * value for value in values) / len(values))


def _diameter(profiles):
    return max((_rms([a - b for a, b in zip(left, right)])
                for index, left in enumerate(profiles)
                for right in profiles[index + 1:]), default=0.0)


def _state_from_raw(detail):
    if (not isinstance(detail, dict) or set(detail) != {"p10", "median", "p90"}
            or any(not _number(detail.get(key)) for key in ("p10", "median", "p90"))
            or not detail["p10"] <= detail["median"] <= detail["p90"]):
        return None
    return ("on" if detail["p10"] >= 45 else
            "off" if detail["p90"] <= 32 else "partial")


def _profiles(sample):
    """Validate and return six (fill, 4x4 profile) pairs.

    The probe's fill diagnostics must reproduce the immutable ordinary-reader
    diagnostics exactly.  Profile measurements cannot substitute a different
    crop or quietly reinterpret the raw field decision.
    """
    reading = _reading(sample)
    raw_bars = reading.get("bars")
    observed = sample.get("observed")
    redraw = observed.get("redraw_profiles") if isinstance(observed, dict) else None
    main = redraw.get("main_bars") if isinstance(redraw, dict) else None
    bars = main.get("bars") if isinstance(main, dict) else None
    if (not isinstance(raw_bars, list) or len(raw_bars) != 6
            or not isinstance(redraw, dict)
            or redraw.get("schema_version") != 1
            or redraw.get("method_version") != PROFILE_REDRAW_PROBE_METHOD_VERSION
            or not isinstance(main, dict) or main.get("profile_schema") != PROFILE_SCHEMA
            or not isinstance(bars, list) or len(bars) != 6):
        return None
    result = []
    for index, (raw, measured) in enumerate(zip(raw_bars, bars)):
        state = _state_from_raw(raw)
        if state is None or not isinstance(measured, dict):
            return None
        fill, profile = measured.get("fill"), measured.get("profile")
        if (measured.get("box") != list(MAIN_BAR_BOXES[index])
                or not isinstance(fill, dict) or fill.get("state") not in _FILL_STATES
                or {key: fill.get(key) for key in ("p10", "median", "p90")} != raw
                or fill["state"] != state
                or not isinstance(profile, list) or len(profile) != 16
                or any(not _number(value) for value in profile)):
            return None
        result.append((fill, tuple(float(value) for value in profile)))
    return result


def _count_states(count):
    return tuple("on" if index < count else "off" for index in range(6))


def _definite_reading(sample):
    reading = _reading(sample)
    count = reading.get("value")
    profiles = _profiles(sample)
    if (reading.get("state") != "readable" or type(count) is not int
            or not 0 <= count <= 6 or profiles is None
            or tuple(fill["state"] for fill, _ in profiles) != _count_states(count)):
        return None
    return count, profiles


def _transition_reading(sample, boundary, unchanged_states):
    reading = _reading(sample)
    profiles = _profiles(sample)
    if (reading.get("state") != "ambiguous"
            or reading.get("value") is not None
            or reading.get("reason") != RAW_AMBIGUOUS_REASON
            or profiles is None):
        return None
    states = tuple(fill["state"] for fill, _ in profiles)
    if (states[boundary] != "partial"
            or any(state not in ("on", "off") for index, state in enumerate(states)
                   if index != boundary)
            or any(state != unchanged_states[index] for index, state in enumerate(states)
                   if index != boundary)):
        return None
    return profiles


def _expectation_signature(sample):
    expected = sample.get("expected")
    if not isinstance(expected, dict) or expected.get("input", {}).get("ready") is not True:
        return None
    fields = expected.get("fields")
    current = fields.get("main_bars") if isinstance(fields, dict) else None
    previous_input = expected.get("previous_input")
    previous_fields = previous_input.get("fields") if isinstance(previous_input, dict) else None
    previous = previous_fields.get("main_bars") if isinstance(previous_fields, dict) else None
    current_allowed = current.get("allowed") if isinstance(current, dict) else None
    previous_allowed = previous.get("allowed") if isinstance(previous, dict) else None
    if (not isinstance(current_allowed, list) or len(current_allowed) != 1
            or not isinstance(previous_allowed, list) or len(previous_allowed) != 1
            or type(current_allowed[0]) is not int or type(previous_allowed[0]) is not int
            or not 0 <= current_allowed[0] <= 6 or not 0 <= previous_allowed[0] <= 6
            or abs(current_allowed[0] - previous_allowed[0]) != 1):
        return None
    return {"previous_count": previous_allowed[0], "current_count": current_allowed[0]}


def _consecutive(left, right, maximum_gap_ns):
    return (type(left.get("video_frame_index")) is int
            and type(left.get("source_frame_seq")) is int
            and right.get("video_frame_index") == left["video_frame_index"] + 1
            and right.get("source_frame_seq") == left["source_frame_seq"] + 1
            and type(left.get("capture_ns")) is int
            and type(right.get("capture_ns")) is int
            and 0 < right["capture_ns"] - left["capture_ns"] <= maximum_gap_ns)


def _context(value):
    if not isinstance(value, dict):
        raise ValueError("main-bar classifier context is missing")
    required = ("capture_id", "selection_manifest_sha256",
                "verified_maximum_source_interval_ns", "reader_method_version",
                "reader_sha256", "redraw_probe_method_version",
                "redraw_probe_sha256")
    if any(key not in value for key in required):
        raise ValueError("main-bar classifier context is incomplete")
    if (_SHA256.fullmatch(str(value["capture_id"])) is None
            or _SHA256.fullmatch(str(value["selection_manifest_sha256"])) is None
            or type(value["verified_maximum_source_interval_ns"]) is not int
            or not 0 < value["verified_maximum_source_interval_ns"] <= MAXIMUM_RECORDING_SOURCE_INTERVAL_NS
            or value["reader_method_version"] != PROFILE_READER_METHOD_VERSION
            or value["reader_sha256"] != PROFILE_READER_SHA256
            or value["redraw_probe_method_version"] != PROFILE_REDRAW_PROBE_METHOD_VERSION
            or value["redraw_probe_sha256"] != PROFILE_REDRAW_PROBE_SHA256):
        raise ValueError("main-bar classifier context does not match the frozen reader/capture contract")
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
    if (first < STABLE_SUPPORT_FRAMES_EACH_SIDE
            or stop + STABLE_SUPPORT_FRAMES_EACH_SIDE > len(selected)):
        return None, _reject(event_id, run, "UNCLOSED_RUN",
                             "main-bar run lacks two readable support frames on each side")
    chain = selected[first - 2:stop + 2]
    maximum_gap = min(context["verified_maximum_source_interval_ns"],
                      MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS)
    if not all(_consecutive(left, right, maximum_gap)
               for left, right in zip(chain, chain[1:])):
        return None, _reject(event_id, run, "SOURCE_GAP",
                             "main-bar support chain crosses an unobserved source position")

    left_support, left_endpoint = chain[0], chain[1]
    right_endpoint, right_support = chain[-2], chain[-1]
    if (right_endpoint["capture_ns"] - left_endpoint["capture_ns"]
            > AUTHORED_DISPLAY_UPDATE_NS + maximum_gap):
        return None, _reject(event_id, run, "ENDPOINT_SPAN",
                             "main-bar endpoints exceed one display update plus the verified source interval")

    definite = [_definite_reading(sample) for sample in
                (left_support, left_endpoint, right_endpoint, right_support)]
    if any(value is None for value in definite):
        return None, _reject(event_id, run, "UNCLOSED_RUN",
                             "main-bar support is not definite and readable")
    left_count, left_profiles = definite[1]
    right_count, right_profiles = definite[2]
    if definite[0][0] != left_count or definite[3][0] != right_count:
        return None, _reject(event_id, run, "UNSTABLE_ENDPOINT",
                             "same-side main-bar support does not match its endpoint")
    if abs(left_count - right_count) != 1:
        return None, _reject(event_id, run, "NOT_ADJACENT_COUNTS",
                             "main-bar endpoints do not differ by exactly one count")

    signatures = [_expectation_signature(sample) for sample in chain]
    signature = signatures[0]
    if (signature is None or any(value != signature for value in signatures)
            or signature["previous_count"] != left_count
            or signature["current_count"] != right_count):
        return None, _reject(event_id, run, "EXPECTATION_SIGNATURE",
                             "support chain lacks one identical resolved previous/current main-bar expectation matching its endpoints")

    boundary = min(left_count, right_count)
    unchanged_states = _count_states(boundary)
    run_profiles = []
    for sample in run:
        profiles = _transition_reading(sample, boundary, unchanged_states)
        if profiles is None:
            return None, _reject(event_id, run, "NOT_BOUNDARY_ONLY",
                                 "raw main-bar refusal is not solely the adjacent changing boundary cell")
        run_profiles.append(profiles)

    all_profiles = [definite[0][1], left_profiles, *run_profiles,
                    right_profiles, definite[3][1]]
    unchanged_diameters = {
        str(index): _diameter([profiles[index][1] for profiles in all_profiles])
        for index in range(6) if index != boundary
    }
    if any(value > UNCHANGED_CELL_PROFILE_DIAMETER_RMS_MAX
           for value in unchanged_diameters.values()):
        return None, _reject(event_id, run, "UNCHANGED_CELL_MOTION",
                             "an unchanged main-bar cell moves beyond the profile bound")

    left_profile = left_profiles[boundary][1]
    right_profile = right_profiles[boundary][1]
    delta = [right - left for left, right in zip(left_profile, right_profile)]
    separation = _rms(delta)
    if separation < ENDPOINT_SEPARATION_RMS_MIN:
        return None, _reject(event_id, run, "ENDPOINT_SEPARATION",
                             "main-bar boundary endpoint profiles are not sufficiently separated")
    denominator = sum(value * value for value in delta)
    projections, residuals = [], []
    for profiles in run_profiles:
        current = profiles[boundary][1]
        alpha = sum((value - base) * change
                    for value, base, change in zip(current, left_profile, delta)) / denominator
        fitted = [base + alpha * change for base, change in zip(left_profile, delta)]
        residual = _rms([value - fit for value, fit in zip(current, fitted)]) / separation
        projections.append(alpha)
        residuals.append(residual)
    if any(not PROJECTION_MIN <= value <= PROJECTION_MAX for value in projections):
        return None, _reject(event_id, run, "PROJECTION_RANGE",
                             "main-bar transition projects beyond its endpoints")
    if any(value > NORMALIZED_RESIDUAL_MAX for value in residuals):
        return None, _reject(event_id, run, "NORMALIZED_RESIDUAL",
                             "main-bar transition shape is not an endpoint-profile interpolation")
    path = [0.0, *projections, 1.0]
    backwards = [max(0.0, left - right) for left, right in zip(path, path[1:])]
    maximum_backward = max(backwards, default=0.0)
    total_backward = sum(backwards)
    if maximum_backward > MAXIMUM_BACKWARD_STEP:
        return None, _reject(event_id, run, "MAXIMUM_BACKWARD_STEP",
                             "main-bar transition has a large backward projection step")
    if total_backward > MAXIMUM_TOTAL_BACKWARD_MOTION:
        return None, _reject(event_id, run, "TOTAL_BACKWARD_MOTION",
                             "main-bar transition has excessive cumulative backward motion")

    medians = [left_profiles[boundary][0]["median"],
               *(profiles[boundary][0]["median"] for profiles in run_profiles),
               right_profiles[boundary][0]["median"]]
    direction = 1.0 if medians[-1] >= medians[0] else -1.0
    backward_median_steps = [max(0.0, -direction * (right - left))
                             for left, right in zip(medians, medians[1:])]
    if any(value > BOUNDARY_MEDIAN_BACKWARD_TOLERANCE
           for value in backward_median_steps):
        return None, _reject(event_id, run, "BOUNDARY_MEDIAN_BACKTRACK",
                             "main-bar boundary median reverses beyond the fixed tolerance")

    record = {
        "event_id": event_id,
        "classifier_id": CLASSIFIER_ID,
        "classifier_spec_sha256": CLASSIFIER_SPEC_SHA256,
        "status": CLASSIFIER_STATUS,
        "raw_affected_fields": ["main_bars"],
        "video_frame_indices": [sample["video_frame_index"] for sample in run],
        "first": _point(run[0]), "last": _point(run[-1]),
        "left_support": _point(left_support), "left_endpoint": _point(left_endpoint),
        "right_endpoint": _point(right_endpoint), "right_support": _point(right_support),
        "endpoint_values": [left_count, right_count],
        "changed_bar_index": boundary,
        "main_bar_expectation_signature": deepcopy(signature),
        "endpoint_separation_rms": separation,
        "projections": projections,
        "normalized_residuals": residuals,
        "maximum_backward_step": maximum_backward,
        "total_backward_motion": total_backward,
        "boundary_medians": medians,
        "maximum_boundary_median_backward_step": max(backward_median_steps, default=0.0),
        "unchanged_cell_profile_diameter_rms": unchanged_diameters,
        "maximum_endpoint_span_ns": AUTHORED_DISPLAY_UPDATE_NS + maximum_gap,
        "verified_maximum_source_interval_ns": context["verified_maximum_source_interval_ns"],
        "profile_schema": deepcopy(PROFILE_SCHEMA),
        "profile_boxes": [list(box) for box in MAIN_BAR_BOXES],
        "redraw_probe_method_version": context["redraw_probe_method_version"],
        "redraw_probe_sha256": context["redraw_probe_sha256"],
        "capture_id": context["capture_id"],
        "selection_manifest_sha256": context["selection_manifest_sha256"],
        "reader_method_version": context["reader_method_version"],
        "reader_sha256": context["reader_sha256"],
        "basis": "A maximal raw main-bar refusal contains only the changing boundary cell and follows the measured monotone profile path between stable, source-consecutive adjacent counts; raw frames remain unresolved.",
    }
    return record, None


def classify_main_bar_runs(samples, events, context):
    """Return candidate main-bar classifications and explicit refusals."""
    result = {"classifications": [], "rejected_runs": [], "errors": []}
    try:
        context = _context(context)
        if not isinstance(events, list):
            raise ValueError("main-bar events are not a list")
        for event in events:
            if not isinstance(event, dict) or not isinstance(event.get("event_id"), str):
                raise ValueError("main-bar event is malformed")
            start, end = event.get("start_ns"), event.get("end_ns")
            if type(start) is not int or type(end) is not int or start >= end:
                raise ValueError("main-bar event bounds are invalid")
            selected = [sample for sample in samples
                        if type(sample.get("capture_ns")) is int
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
