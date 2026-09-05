"""Conservative same-frequency context candidates for reader refusals.

The raw reader remains authoritative and its ambiguous observations are never
rewritten.  This module can attach a policy-gated legal-presentation candidate
only when a contiguous refusal run lies inside a bounded source-consecutive
episode closed by two same-value readable frames on each side and every frame
in that episode preserves the support-derived glyph geometry.
"""
from __future__ import annotations

from copy import deepcopy
import math
import re


CLASSIFIER_ID = "v1-stable-frequency-intact-context-v1"
CLASSIFIER_SPEC_SHA256 = "cc8b2c094781c67b8ef47bbb7d4e07a6c41adb2194702889790736d0e52a02c4"
DEADLINE_OBSERVATION_SEMANTICS = "LEGAL_PRESENTATION_TRANSITION"
VERIFICATION_CLOSURE_SEMANTICS = (
    "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY")

PROFILE_READER_METHOD_VERSION = 7
PROFILE_READER_SHA256 = "f4efd6a1df4daefb3e7271a2e229e80f7378ba8b7a824dea1593aa0581f20b7c"
PROFILE_REDRAW_PROBE_METHOD_VERSION = 1
PROFILE_REDRAW_PROBE_SHA256 = "4b623360af63afe5c51f154ed5ce57325476b8419bc165bc8a5ee9ef7f55da24"

MAXIMUM_RECORDING_SOURCE_INTERVAL_NS = 1_000_000_000
MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS = 10_000_000
MAXIMUM_REFUSAL_RUN_SPAN_NS = 75_000_000
MAXIMUM_SUPPORT_CHAIN_SPAN_NS = 300_000_000
STABLE_SUPPORT_FRAMES_EACH_SIDE = 2
MAXIMUM_HOLE_INK_FRACTION = 0.10

# These are the unchanged reader7 segment thresholds. This capability cannot
# resolve any partial segment, in either its claimed run or closing context.
READER_ON_P10_MIN = 45.0
READER_OFF_P90_MAX = 32.0

BRANCH_INTACT_MASK = "intact_mask"
_BRANCH_REASON = {
    BRANCH_INTACT_MASK: "inconsistent illuminated frequency segment levels",
}

_FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
           "main_bars", "secondary", "muted_badge")
_SEGMENTS = "abcdefg"
_DIGIT_MASKS = {
    "0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg", "4": "bcfg",
    "5": "acdfg", "6": "acdefg", "7": "abc", "8": "abcdefg", "9": "abcdfg",
}
_FREQUENCY = re.compile(r"[0-9]{2}\.[0-9]{3}")
_FREQUENCY_ORIGINS = (454, 520, 616, 688, 764)
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image", "image_sha256")


def _point(sample):
    return {key: deepcopy(sample[key]) for key in _POINT_KEYS if key in sample}


def _reading(sample):
    observed = sample.get("observed")
    fields = observed.get("fields") if isinstance(observed, dict) else None
    value = fields.get("primary_frequency") if isinstance(fields, dict) else None
    return value if isinstance(value, dict) else {}


def _profiles(sample):
    observed = sample.get("observed")
    value = observed.get("redraw_profiles") if isinstance(observed, dict) else None
    return value if isinstance(value, dict) else {}


def _unique_originals(samples):
    if not isinstance(samples, list):
        raise ValueError("frequency context samples are not a list")
    originals = {}
    for sample in samples:
        if not isinstance(sample, dict):
            raise ValueError("frequency context sample is not an object")
        index = sample.get("video_frame_index")
        if type(index) is not int:
            continue
        prior = originals.get(index)
        if prior is not None and any(prior.get(key) != sample.get(key)
                                     for key in ("observed", "expected", "comparison")):
            raise ValueError("duplicate source image has conflicting frequency evidence")
        originals.setdefault(index, sample)
    return [sample for _, sample in sorted(originals.items())]


def _context(value):
    if not isinstance(value, dict):
        raise ValueError("frequency context classifier context is missing")
    required = ("capture_id", "selection_manifest_sha256", "verified_maximum_source_interval_ns",
                "reader_method_version", "reader_sha256", "redraw_probe_method_version",
                "redraw_probe_sha256")
    if any(key not in value for key in required):
        raise ValueError("frequency context classifier context is incomplete")
    if (_SHA256.fullmatch(str(value["capture_id"])) is None
            or _SHA256.fullmatch(str(value["selection_manifest_sha256"])) is None
            or type(value["verified_maximum_source_interval_ns"]) is not int
            or not 0 < value["verified_maximum_source_interval_ns"]
            <= MAXIMUM_RECORDING_SOURCE_INTERVAL_NS
            or value["reader_method_version"] != PROFILE_READER_METHOD_VERSION
            or value["reader_sha256"] != PROFILE_READER_SHA256
            or value["redraw_probe_method_version"] != PROFILE_REDRAW_PROBE_METHOD_VERSION
            or value["redraw_probe_sha256"] != PROFILE_REDRAW_PROBE_SHA256):
        raise ValueError("frequency context does not match the frozen optical profile")
    return value


def _consecutive(left, right, maximum_gap_ns):
    return (type(left.get("video_frame_index")) is int
            and type(left.get("source_frame_seq")) is int
            and right.get("video_frame_index") == left["video_frame_index"] + 1
            and right.get("source_frame_seq") == left["source_frame_seq"] + 1
            and type(left.get("capture_ns")) is int and type(right.get("capture_ns")) is int
            and 0 < right["capture_ns"] - left["capture_ns"] <= maximum_gap_ns)


def _resolved_allowed(target, field):
    fields = target.get("fields") if isinstance(target, dict) else None
    spec = fields.get(field) if isinstance(fields, dict) else None
    allowed = spec.get("allowed") if isinstance(spec, dict) else None
    if not isinstance(allowed, list) or len(allowed) != 1 or "unresolved" in spec:
        return None
    return deepcopy(allowed[0])


def _canonical_masks(value):
    if not isinstance(value, str) or _FREQUENCY.fullmatch(value) is None:
        return None
    return [_DIGIT_MASKS[digit] for digit in value.replace(".", "")]


def _number(value):
    if type(value) not in (int, float) or isinstance(value, bool) \
            or not math.isfinite(value) or not 0 <= value <= 255:
        return None
    return float(value)


def _segment(detail):
    if not isinstance(detail, dict) or set(detail) != {"state", "p10", "median", "p90"} \
            or detail.get("state") not in {"on", "off", "partial"}:
        return None
    values = [_number(detail.get(key)) for key in ("p10", "median", "p90")]
    if any(value is None for value in values) or not values[0] <= values[1] <= values[2]:
        return None
    derived = ("on" if values[0] >= READER_ON_P10_MIN else
               "off" if values[2] <= READER_OFF_P90_MAX else "partial")
    if detail["state"] != derived:
        return None
    return {"state": derived, "p10": values[0], "median": values[1], "p90": values[2]}


def _frame_geometry(sample, masks):
    root = _profiles(sample)
    frequency = root.get("primary_frequency") if isinstance(root, dict) else None
    cells = frequency.get("cells") if isinstance(frequency, dict) else None
    decimal = frequency.get("decimal") if isinstance(frequency, dict) else None
    raw_cells = _reading(sample).get("cells")
    if (root.get("schema_version") != 1 or root.get("method_version") != 1
            or not isinstance(cells, list) or len(cells) != 5
            or not isinstance(raw_cells, list) or len(raw_cells) != 5
            or not isinstance(decimal, dict) or set(decimal) != {"state", "p10", "median", "p90"}
            or decimal.get("state") != "on"):
        return "frequency frame lacks the fixed five-digit profile and on decimal"
    decimal_detail = _segment(decimal)
    if decimal_detail is None or decimal_detail["state"] != "on":
        return "frequency decimal is not a valid on segment"

    expected_off_p90 = []
    for origin, cell, raw_cell, expected_mask in zip(
            _FREQUENCY_ORIGINS, cells, raw_cells, masks):
        segments = cell.get("segments") if isinstance(cell, dict) else None
        raw_segments = raw_cell.get("segments") if isinstance(raw_cell, dict) else None
        holes = cell.get("hole_ink_fractions") if isinstance(cell, dict) else None
        if (cell.get("origin") != origin or not isinstance(segments, dict)
                or set(segments) != set(_SEGMENTS) or not isinstance(raw_segments, dict)
                or set(raw_segments) != set(_SEGMENTS) or not isinstance(holes, list)
                or len(holes) != 2 or any(_number(value) is None
                                         or float(value) > MAXIMUM_HOLE_INK_FRACTION
                                         for value in holes)):
            return "frequency cells lack exact origins, segments, or clear holes"
        for name in _SEGMENTS:
            detail = _segment(segments[name])
            raw_detail = raw_segments[name]
            if detail is None or not isinstance(raw_detail, dict) \
                    or raw_detail != segments[name]:
                return "frequency profile disagrees with raw segment evidence"
            expected_state = "on" if name in expected_mask else "off"
            if detail["state"] != expected_state:
                return "frequency segment is not its exact intact on/off state"
            if expected_state == "off":
                expected_off_p90.append(detail["p90"])
        if raw_cell.get("mask") != expected_mask:
            return "frequency raw mask disagrees with its segment states"
    if not expected_off_p90 or max(expected_off_p90) > READER_OFF_P90_MAX:
        return "frequency frame has no valid expected-off reference"
    return None


def _comparison_matches(sample, *, refusal):
    comparison = sample.get("comparison")
    checks = comparison.get("checks") if isinstance(comparison, dict) else None
    joint = comparison.get("joint_state") if isinstance(comparison, dict) else None
    if not isinstance(checks, dict) or set(checks) != set(_FIELDS) or not isinstance(joint, dict):
        return False
    statuses = {field: checks[field].get("status") if isinstance(checks[field], dict) else None
                for field in _FIELDS}
    if any(status == "DIFFERENCE" for status in statuses.values()):
        return False
    if refusal:
        if statuses["primary_frequency"] not in {"UNRESOLVED", "CONDITIONAL"} \
                or any(statuses[field] != "MATCH" for field in _FIELDS
                       if field != "primary_frequency"):
            return False
        return joint.get("status") in {"MATCH", "UNRESOLVED"}
    return all(status == "MATCH" for status in statuses.values()) \
        and joint.get("status") == "MATCH"


def _runs(selected, reason):
    index = 0
    while index < len(selected):
        reading = _reading(selected[index])
        if reading.get("state") != "ambiguous" or reading.get("reason") != reason:
            index += 1
            continue
        stop = index + 1
        while stop < len(selected):
            following = _reading(selected[stop])
            if (following.get("state") != "ambiguous" or following.get("reason") != reason
                    or not _consecutive(selected[stop - 1], selected[stop],
                                        MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS)):
                break
            stop += 1
        yield index, stop
        index = stop


def _support_chain(selected, first, stop, context):
    run = selected[first:stop]
    if run[-1]["capture_ns"] - run[0]["capture_ns"] > MAXIMUM_REFUSAL_RUN_SPAN_NS:
        return None, "RUN_SPAN", "frequency refusal run exceeds 75 ms"
    left = first - 1
    while left >= 1:
        if all(_reading(sample).get("state") == "readable"
               for sample in selected[left - 1:left + 1]):
            break
        left -= 1
    right = stop
    while right + 1 < len(selected):
        if all(_reading(sample).get("state") == "readable"
               for sample in selected[right:right + 2]):
            break
        right += 1
    if left < 1 or right + 1 >= len(selected):
        return None, "UNCLOSED_RUN", "frequency refusal lacks two readable closing supports on each side"
    chain = selected[left - 1:right + 2]
    maximum_gap = min(context["verified_maximum_source_interval_ns"],
                      MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS)
    if not all(_consecutive(left, right, maximum_gap)
               for left, right in zip(chain, chain[1:])):
        return None, "SOURCE_GAP", "frequency support chain crosses an unobserved source position"
    if chain[-1]["capture_ns"] - chain[0]["capture_ns"] > MAXIMUM_SUPPORT_CHAIN_SPAN_NS:
        return None, "SUPPORT_SPAN", "frequency support chain exceeds 300 ms"
    return chain, None, None


def _support_value(chain):
    supports = chain[:2] + chain[-2:]
    readings = [_reading(sample) for sample in supports]
    values = [reading.get("value") for reading in readings]
    if any(reading.get("state") != "readable" or reading.get("reason") is not None
           for reading in readings) or len(set(values)) != 1:
        return None, "SUPPORT_VALUE", "frequency supports are not one same readable value"
    value = values[0]
    if _canonical_masks(value) is None:
        return None, "SUPPORT_VALUE", "frequency supports do not derive one canonical DD.DDD value"
    if any(not _comparison_matches(sample, refusal=False) for sample in supports):
        return None, "SUPPORT_COMPARISON", "frequency supports are not exact current comparisons"
    return value, None, None


def _current_target_frequency(event, run, support_value):
    target = _resolved_allowed(event.get("target"), "primary_frequency")
    if target != support_value:
        return None, "TARGET_MISMATCH", "support-derived frequency does not equal the exact current target"
    for sample in run:
        if _resolved_allowed(sample.get("expected"), "primary_frequency") != support_value:
            return None, "TARGET_MISMATCH", "sample target does not equal the support-derived frequency"
    return target, None, None


def _context_geometry(chain, masks, support_value, claimed_indices):
    for sample in chain[2:-2]:
        if sample["video_frame_index"] in claimed_indices:
            continue
        reading = _reading(sample)
        if reading.get("state") == "readable":
            if reading.get("value") != support_value or reading.get("reason") is not None \
                    or not _comparison_matches(sample, refusal=False):
                return None, "an interior readable frame is not the same exact current frequency"
        elif (reading.get("state") != "ambiguous" or reading.get("value") is not None
              or reading.get("reason") != _BRANCH_REASON[BRANCH_INTACT_MASK]
              or not _comparison_matches(sample, refusal=True)):
            return None, "an interior refusal is outside the intact-only frequency scope"
        geometry_reason = _frame_geometry(sample, masks)
        if geometry_reason is not None:
            return None, geometry_reason
    return [sample["video_frame_index"] for sample in chain[2:-2]], None


def _record(event, selected, first, stop, context):
    run = selected[first:stop]
    chain, code, reason = _support_chain(selected, first, stop, context)
    if chain is None:
        return None, code, reason

    support_value, code, reason = _support_value(chain)
    if support_value is None:
        return None, code, reason
    masks = _canonical_masks(support_value)

    # Geometry is established from the support-derived value before the event
    # or per-sample target is consulted.
    for sample in chain[:2] + chain[-2:]:
        geometry_reason = _frame_geometry(sample, masks)
        if geometry_reason is not None:
            return None, "SUPPORT_GEOMETRY", geometry_reason

    _, code, reason = _current_target_frequency(event, chain[2:-2], support_value)
    if code is not None:
        return None, code, reason
    claimed_indices = {sample["video_frame_index"] for sample in run}
    context_indices, geometry_reason = _context_geometry(
        chain, masks, support_value, claimed_indices)
    if context_indices is None:
        return None, "CONTEXT_GEOMETRY", geometry_reason

    expected_reason = _BRANCH_REASON[BRANCH_INTACT_MASK]
    for sample in run:
        reading = _reading(sample)
        if not _comparison_matches(sample, refusal=True):
            return None, "PRODUCT_FIELD_SCOPE", "refusal is not solely unresolved frequency with no difference"
        if reading.get("state") != "ambiguous" or reading.get("value") is not None \
                or reading.get("reason") != expected_reason:
            return None, "READER_REASON", "frequency refusal does not match its frozen branch reason"
        geometry_reason = _frame_geometry(sample, masks)
        if geometry_reason is not None:
            return None, "FREQUENCY_GEOMETRY", geometry_reason

    indices = [sample["video_frame_index"] for sample in run]
    if any(right != left + 1 for left, right in zip(indices, indices[1:])):
        return None, "NONCONTIGUOUS_PRODUCT_CLAIM", "frequency product claim is not contiguous"
    return {
        "event_id": event["event_id"],
        "classifier_id": CLASSIFIER_ID,
        "classifier_spec_sha256": CLASSIFIER_SPEC_SHA256,
        "status": "QUALIFIED_CAPTURE_TRANSITION",
        "deadline_observation_semantics": DEADLINE_OBSERVATION_SEMANTICS,
        "verification_closure_semantics": VERIFICATION_CLOSURE_SEMANTICS,
        "branch": BRANCH_INTACT_MASK,
        "raw_affected_fields": ["primary_frequency"],
        "video_frame_indices": indices,
        "first": _point(run[0]),
        "last": _point(run[-1]),
        "left_support": [_point(sample) for sample in chain[:2]],
        "right_support": [_point(sample) for sample in chain[-2:]],
        "context_frame_indices": context_indices,
        "context_observed_branches": [BRANCH_INTACT_MASK],
        "support_derived_frequency": support_value,
        "support_derived_digit_masks": masks,
        "ambiguity_reason": expected_reason,
        "maximum_refusal_run_span_ns": MAXIMUM_REFUSAL_RUN_SPAN_NS,
        "maximum_support_chain_span_ns": MAXIMUM_SUPPORT_CHAIN_SPAN_NS,
        "verified_maximum_source_interval_ns": context["verified_maximum_source_interval_ns"],
        "capture_id": context["capture_id"],
        "selection_manifest_sha256": context["selection_manifest_sha256"],
        "reader_method_version": context["reader_method_version"],
        "reader_sha256": context["reader_sha256"],
        "redraw_probe_method_version": context["redraw_probe_method_version"],
        "redraw_probe_sha256": context["redraw_probe_sha256"],
        "event_signature": {
            "mode": event.get("mode"),
            "changed_fields": deepcopy(event.get("changed_fields")),
            "current_primary_frequency": support_value,
        },
        "basis": "A bounded raw frequency refusal and its entire closing context retain intact support-derived canonical glyphs with no partial segments between source-consecutive same-value readable supports; raw frames remain unresolved.",
    }, None, None


def _reject(event, run, code, reason):
    return {"event_id": event.get("event_id"), "field": "primary_frequency",
            "branch": BRANCH_INTACT_MASK, "code": code, "first": _point(run[0]),
            "last": _point(run[-1]), "reason": reason}


def classify_frequency_context_runs(samples, events, context):
    """Return closed frequency-context candidates without mutating inputs."""
    result = {"classifications": [], "rejected_runs": [], "errors": []}
    try:
        context = _context(context)
        originals = _unique_originals(samples)
        if not isinstance(events, list):
            raise ValueError("frequency context events are not a list")
        for event in events:
            if (not isinstance(event, dict) or not isinstance(event.get("event_id"), str)
                    or type(event.get("start_ns")) is not int or type(event.get("end_ns")) is not int
                    or event["start_ns"] >= event["end_ns"]):
                raise ValueError("frequency context event is malformed")
            selected = [sample for sample in originals
                        if type(sample.get("capture_ns")) is int
                        and event["start_ns"] <= sample["capture_ns"] < event["end_ns"]]
            for first, stop in _runs(selected, _BRANCH_REASON[BRANCH_INTACT_MASK]):
                run = selected[first:stop]
                record, code, explanation = _record(event, selected, first, stop, context)
                if record is None:
                    result["rejected_runs"].append(_reject(event, run, code, explanation))
                else:
                    result["classifications"].append(record)
    except (KeyError, TypeError, ValueError) as exc:
        result["classifications"] = []
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    return result
