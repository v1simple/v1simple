"""Frozen-shape candidates for optical mute-on and unmute redraw intervals.

These classifiers never replace a single-frame observation. They may describe
only a maximal, source-consecutive run of raw reader refusals bracketed by two
stable readable frames on each side. Product policy must explicitly allow each
classifier before its record can affect a verdict.
"""
from __future__ import annotations

from copy import deepcopy
import math
import re


BADGE_CLASSIFIER_ID = "v1-muted-badge-rising-fill-v2"
BADGE_CLASSIFIER_SPEC_SHA256 = "901367b4cf331f60ea03d5548b2d4bf68ecc2466abb1fde86264bd9588cba89e"
FREQUENCY_CLASSIFIER_ID = "v1-unmute-stable-frequency-sweep-v2"
FREQUENCY_CLASSIFIER_SPEC_SHA256 = "c1f3e2a7217f8c5fb649a8139860b5794f82ecc4f03c9a459c5f4cb87f2220e5"

PROFILE_READER_METHOD_VERSION = 6
PROFILE_READER_SHA256 = "17988f82323e53a6d3a507cf101606e546ce61caa2d815cf0ca7024fdeffad98"
PROFILE_REDRAW_PROBE_METHOD_VERSION = 1
PROFILE_REDRAW_PROBE_SHA256 = "4b623360af63afe5c51f154ed5ce57325476b8419bc165bc8a5ee9ef7f55da24"
MAXIMUM_RECORDING_SOURCE_INTERVAL_NS = 1_000_000_000
MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS = 10_000_000
MAXIMUM_SUPPORT_CHAIN_SPAN_NS = 50_000_000
STABLE_SUPPORT_FRAMES_EACH_SIDE = 2

BADGE_COMPONENT_SEPARATION_MIN = 20.0
BADGE_PROGRESS_MIN = -0.15
BADGE_PROGRESS_MAX = 1.15
BADGE_MAXIMUM_BACKWARD_STEP = 0.10
BADGE_MAXIMUM_TOTAL_BACKWARD_MOTION = 0.15

FREQUENCY_COMPONENT_SEPARATION_MIN = 50.0
FREQUENCY_PROGRESS_MIN = -0.10
FREQUENCY_PROGRESS_MAX = 1.10
FREQUENCY_MAXIMUM_BACKWARD_STEP = 0.10
FREQUENCY_MAXIMUM_TOTAL_BACKWARD_MOTION = 0.15

_FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
           "main_bars", "secondary", "muted_badge")
_SEGMENTS = "abcdefg"
_DIGIT_MASKS = {
    "0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg", "4": "bcfg",
    "5": "acdfg", "6": "acdefg", "7": "abc", "8": "abcdefg", "9": "abcdfg",
}
_FREQUENCY = re.compile(r"[0-9]{2}\.[0-9]{3}")
_FREQUENCY_ORIGINS = (454, 520, 616, 688, 764)
_MUTED_BADGE_BOX = [569, 216, 617, 228]
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image", "image_sha256")


def _point(sample):
    return {key: deepcopy(sample[key]) for key in _POINT_KEYS if key in sample}


def _reading(sample, field):
    value = sample.get("observed", {}).get("fields", {}).get(field)
    return value if isinstance(value, dict) else {}


def _profiles(sample):
    observed = sample.get("observed")
    value = observed.get("redraw_profiles") if isinstance(observed, dict) else None
    return value if isinstance(value, dict) else {}


def _unique_originals(samples):
    originals = {}
    for sample in samples:
        index = sample.get("video_frame_index")
        if type(index) is not int:
            continue
        prior = originals.get(index)
        if prior is not None and (prior.get("observed") != sample.get("observed")
                                  or prior.get("comparison") != sample.get("comparison")):
            raise ValueError("duplicate source image has conflicting redraw evidence")
        originals.setdefault(index, sample)
    return [sample for _, sample in sorted(originals.items())]


def _context(value):
    if not isinstance(value, dict):
        raise ValueError("mute redraw classifier context is missing")
    required = ("capture_id", "selection_manifest_sha256", "verified_maximum_source_interval_ns",
                "reader_method_version", "reader_sha256", "redraw_probe_method_version",
                "redraw_probe_sha256")
    if any(key not in value for key in required):
        raise ValueError("mute redraw classifier context is incomplete")
    if (_SHA256.fullmatch(str(value["capture_id"])) is None
            or _SHA256.fullmatch(str(value["selection_manifest_sha256"])) is None
            or type(value["verified_maximum_source_interval_ns"]) is not int
            or not 0 < value["verified_maximum_source_interval_ns"] <= MAXIMUM_RECORDING_SOURCE_INTERVAL_NS
            or value["reader_method_version"] != PROFILE_READER_METHOD_VERSION
            or value["reader_sha256"] != PROFILE_READER_SHA256
            or value["redraw_probe_method_version"] != PROFILE_REDRAW_PROBE_METHOD_VERSION
            or value["redraw_probe_sha256"] != PROFILE_REDRAW_PROBE_SHA256):
        raise ValueError("mute redraw classifier context does not match the frozen optical profile")
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


def _mute_event_signature(event, previous_mute, target_mute):
    if (not isinstance(event, dict) or event.get("mode") != "CHANGED"
            or event.get("changed_fields") != ["muted_badge"]):
        return None
    previous, target = event.get("previous_target"), event.get("target")
    if (_resolved_allowed(previous, "muted_badge") is not previous_mute
            or _resolved_allowed(target, "muted_badge") is not target_mute):
        return None
    previous_fields = previous.get("fields") if isinstance(previous, dict) else None
    target_fields = target.get("fields") if isinstance(target, dict) else None
    if not isinstance(previous_fields, dict) or not isinstance(target_fields, dict):
        return None
    for field in _FIELDS:
        if field != "muted_badge" and previous_fields.get(field) != target_fields.get(field):
            return None
        if _resolved_allowed(previous, field) is None or _resolved_allowed(target, field) is None:
            return None
    if previous.get("joint_states") != target.get("joint_states"):
        return None
    return {
        "previous_muted_badge": previous_mute,
        "target_muted_badge": target_mute,
        "stable_primary_frequency": _resolved_allowed(target, "primary_frequency"),
        "stable_fields": {field: deepcopy(target_fields[field]) for field in _FIELDS
                          if field != "muted_badge"},
        "joint_states": deepcopy(target.get("joint_states")),
    }


def _claimable_for_current(sample, field):
    comparison = sample.get("comparison")
    checks = comparison.get("checks") if isinstance(comparison, dict) else None
    joint = comparison.get("joint_state") if isinstance(comparison, dict) else None
    if not isinstance(checks, dict) or not isinstance(joint, dict):
        return False
    field_status = checks.get(field, {}).get("status") if isinstance(checks.get(field), dict) else None
    return (field_status in {"UNRESOLVED", "CONDITIONAL"}
            and all(isinstance(checks.get(name), dict)
                    and checks[name].get("status") == "MATCH"
                    for name in _FIELDS if name != field)
            and joint.get("status") in {"MATCH", "NOT_EVALUATED"})


def _runs(selected, field, reason):
    index = 0
    while index < len(selected):
        reading = _reading(selected[index], field)
        if reading.get("state") != "ambiguous" or reading.get("reason") != reason:
            index += 1
            continue
        stop = index + 1
        while stop < len(selected):
            following = _reading(selected[stop], field)
            if (following.get("state") != "ambiguous" or following.get("reason") != reason
                    or not _consecutive(selected[stop - 1], selected[stop],
                                        MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS)):
                break
            stop += 1
        yield index, stop
        index = stop


def _support_chain(selected, first, stop, context):
    if first < STABLE_SUPPORT_FRAMES_EACH_SIDE \
            or stop + STABLE_SUPPORT_FRAMES_EACH_SIDE > len(selected):
        return None, "UNCLOSED_RUN", "redraw run lacks two immediate support frames on each side"
    chain = selected[first - 2:stop + 2]
    gap = min(context["verified_maximum_source_interval_ns"],
              MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS)
    if not all(_consecutive(left, right, gap) for left, right in zip(chain, chain[1:])):
        return None, "SOURCE_GAP", "redraw support chain crosses an unobserved source position"
    if chain[-1]["capture_ns"] - chain[0]["capture_ns"] > MAXIMUM_SUPPORT_CHAIN_SPAN_NS:
        return None, "SUPPORT_SPAN", "redraw support chain exceeds 50 ms"
    return chain, None, None


def _vector(value, length):
    if (not isinstance(value, list) or len(value) != length
            or any(type(item) not in (int, float) or isinstance(item, bool)
                   or not math.isfinite(item) or not 0 <= item <= 255 for item in value)):
        return None
    return [float(item) for item in value]


def _component_progress(vectors, separation_min, progress_min, progress_max,
                        maximum_backward_step, maximum_total_backward):
    left, right = vectors[0], vectors[-1]
    separation = [r - l for l, r in zip(left, right)]
    if any(value < separation_min for value in separation):
        return None, "ENDPOINT_SEPARATION", "one or more redraw profile components lack endpoint separation"
    progress = [[(value - l) / delta for value, l, delta in zip(vector, left, separation)]
                for vector in vectors]
    if any(value < progress_min or value > progress_max for vector in progress for value in vector):
        return None, "PROGRESS_RANGE", "redraw profile leaves its frozen endpoint range"
    backward_steps = [[max(0.0, before - after) for before, after in zip(a, b)]
                      for a, b in zip(progress, progress[1:])]
    maximum_backward = max((value for vector in backward_steps for value in vector), default=0.0)
    totals = [sum(step[index] for step in backward_steps) for index in range(len(left))]
    if maximum_backward > maximum_backward_step:
        return None, "MAXIMUM_BACKWARD_STEP", "redraw profile has a backward component step"
    if max(totals, default=0.0) > maximum_total_backward:
        return None, "TOTAL_BACKWARD_MOTION", "redraw profile has cumulative backward component motion"
    return {
        "endpoint_component_separation": separation,
        "component_progress": progress,
        "maximum_backward_step": maximum_backward,
        "total_backward_motion_by_component": totals,
    }, None, None


def _badge_profile(sample):
    root = _profiles(sample)
    badge = root.get("muted_badge") if isinstance(root, dict) else None
    schema = badge.get("profile_schema") if isinstance(badge, dict) else None
    profile = _vector(badge.get("profile"), 8) if isinstance(badge, dict) else None
    lit_fraction = badge.get("lit_fraction") if isinstance(badge, dict) else None
    p95 = badge.get("p95") if isinstance(badge, dict) else None
    if (root.get("schema_version") != 1 or root.get("method_version") != 1
            or badge.get("box") != _MUTED_BADGE_BOX
            or not isinstance(schema, dict) or schema.get("rows") != 2
            or schema.get("columns") != 4 or schema.get("sample") != "max-channel cell median"
            or profile is None or type(lit_fraction) not in (int, float)
            or type(p95) not in (int, float)
            or not math.isfinite(lit_fraction) or not 0 <= lit_fraction <= 1
            or not math.isfinite(p95) or not 0 <= p95 <= 255):
        return None
    return {"profile": profile, "lit_fraction": float(lit_fraction), "p95": float(p95)}


def _badge_measurement_matches(reading, profile):
    state = reading.get("state")
    if state == "readable" and reading.get("value") is True:
        return profile["lit_fraction"] > 0.3
    if state == "readable" and reading.get("value") is False:
        return profile["lit_fraction"] <= 0.3 and profile["p95"] < 25
    if state == "ambiguous" and reading.get("value") is None \
            and reading.get("reason") == "partial muted badge":
        return profile["lit_fraction"] <= 0.3 and profile["p95"] >= 25
    return False


def _badge_record(event, selected, first, stop, context, signature):
    run = selected[first:stop]
    chain, code, reason = _support_chain(selected, first, stop, context)
    if chain is None:
        return None, code, reason
    readings = [_reading(sample, "muted_badge") for sample in chain]
    if any(reading.get("state") != "readable" or reading.get("value") is not False
           for reading in readings[:2]):
        return None, "LEFT_SUPPORT", "mute-on redraw lacks two readable false support frames"
    if any(reading.get("state") != "readable" or reading.get("value") is not True
           for reading in readings[-2:]):
        return None, "RIGHT_SUPPORT", "mute-on redraw lacks two readable true support frames"
    if any(not _claimable_for_current(sample, "muted_badge") for sample in run):
        return None, "PRODUCT_FIELD_SCOPE", "mute-on redraw is not solely an unresolved current badge field"
    measured = [_badge_profile(sample) for sample in chain]
    if any(item is None for item in measured):
        return None, "PROFILE_SCHEMA", "mute-on redraw lacks the fixed 2 by 4 optical profile"
    if any(not _badge_measurement_matches(reading, profile)
           for reading, profile in zip(readings, measured)):
        return None, "PROFILE_READER_MISMATCH", "badge profile disagrees with its raw field state"
    metrics, code, reason = _component_progress(
        [item["profile"] for item in measured], BADGE_COMPONENT_SEPARATION_MIN,
        BADGE_PROGRESS_MIN, BADGE_PROGRESS_MAX, BADGE_MAXIMUM_BACKWARD_STEP,
        BADGE_MAXIMUM_TOTAL_BACKWARD_MOTION)
    if metrics is None:
        return None, code, reason
    return {
        "event_id": event["event_id"], "classifier_id": BADGE_CLASSIFIER_ID,
        "classifier_spec_sha256": BADGE_CLASSIFIER_SPEC_SHA256,
        "status": "QUALIFIED_CAPTURE_TRANSITION", "raw_affected_fields": ["muted_badge"],
        "video_frame_indices": [sample["video_frame_index"] for sample in run],
        "first": _point(run[0]), "last": _point(run[-1]),
        "left_support": [_point(sample) for sample in chain[:2]],
        "right_support": [_point(sample) for sample in chain[-2:]],
        "event_signature": signature, **metrics,
        "maximum_support_chain_span_ns": MAXIMUM_SUPPORT_CHAIN_SPAN_NS,
        "verified_maximum_source_interval_ns": context["verified_maximum_source_interval_ns"],
        "capture_id": context["capture_id"],
        "selection_manifest_sha256": context["selection_manifest_sha256"],
        "reader_method_version": context["reader_method_version"],
        "reader_sha256": context["reader_sha256"],
        "redraw_probe_method_version": context["redraw_probe_method_version"],
        "redraw_probe_sha256": context["redraw_probe_sha256"],
        "basis": "A maximal raw badge refusal is a componentwise monotone spatial fill between two source-consecutive readable unmuted and muted supports; raw frames remain unresolved.",
    }, None, None


def _expected_frequency_masks(value):
    if not isinstance(value, str) or _FREQUENCY.fullmatch(value) is None:
        return None
    return [_DIGIT_MASKS[digit] for digit in value.replace(".", "")]


def _frequency_profile(sample, expected_masks):
    root = _profiles(sample)
    frequency = root.get("primary_frequency") if isinstance(root, dict) else None
    cells = frequency.get("cells") if isinstance(frequency, dict) else None
    decimal = frequency.get("decimal") if isinstance(frequency, dict) else None
    raw_cells = _reading(sample, "primary_frequency").get("cells")
    if (root.get("schema_version") != 1 or root.get("method_version") != 1
            or not isinstance(cells, list) or len(cells) != 5
            or not isinstance(raw_cells, list) or len(raw_cells) != 5
            or not isinstance(decimal, dict) or decimal.get("state") != "on"
            or set(decimal) != {"state", "p10", "median", "p90"}
            or any(type(decimal.get(key)) not in (int, float)
                   or not math.isfinite(decimal[key]) or not 0 <= decimal[key] <= 255
                   for key in ("p10", "median", "p90"))
            or not decimal["p10"] <= decimal["median"] <= decimal["p90"]
            or decimal["p10"] < 45):
        return None
    profile = []
    for origin, cell, raw_cell, expected_mask in zip(
            _FREQUENCY_ORIGINS, cells, raw_cells, expected_masks):
        segments = cell.get("segments") if isinstance(cell, dict) else None
        raw_segments = raw_cell.get("segments") if isinstance(raw_cell, dict) else None
        holes = cell.get("hole_ink_fractions") if isinstance(cell, dict) else None
        if (cell.get("origin") != origin or raw_cell.get("mask") != expected_mask
                or not isinstance(segments, dict) or set(segments) != set(_SEGMENTS)
                or not isinstance(raw_segments, dict) or set(raw_segments) != set(_SEGMENTS)
                or not isinstance(holes, list) or len(holes) != 2
                or any(type(value) not in (int, float) or isinstance(value, bool)
                       or not math.isfinite(value) or not 0 <= value <= 0.10 for value in holes)):
            return None
        for name in _SEGMENTS:
            detail = segments[name]
            raw_detail = raw_segments[name]
            if (not isinstance(detail, dict) or set(detail) != {"state", "p10", "median", "p90"}
                    or detail.get("state") not in {"on", "off"}
                    or not isinstance(raw_detail, dict)
                    or {key: raw_detail.get(key) for key in ("state", "p10", "median", "p90")}
                       != {key: detail.get(key) for key in ("state", "p10", "median", "p90")}):
                return None
            if detail["state"] != ("on" if name in expected_mask else "off"):
                return None
            values = [detail.get(key) for key in ("p10", "median", "p90")]
            if (any(type(value) not in (int, float) or isinstance(value, bool)
                    or not math.isfinite(value) or not 0 <= value <= 255 for value in values)
                    or not values[0] <= values[1] <= values[2]
                    or detail["state"] != ("on" if values[0] >= 45 else
                                            "off" if values[2] <= 32 else "partial")):
                return None
            if name in expected_mask:
                profile.append(float(values[1]))
    return profile


def _frequency_record(event, selected, first, stop, context, signature):
    full_run = selected[first:stop]
    chain, code, reason = _support_chain(selected, first, stop, context)
    if chain is None:
        return None, code, reason
    expected = signature["stable_primary_frequency"]
    masks = _expected_frequency_masks(expected)
    if masks is None:
        return None, "EXPECTED_FREQUENCY", "unmute redraw has no canonical stable frequency"
    readings = [_reading(sample, "primary_frequency") for sample in chain]
    if any(reading.get("state") != "readable" or reading.get("value") != expected
           for reading in readings[:2] + readings[-2:]):
        return None, "STABLE_FREQUENCY_SUPPORT", "unmute redraw lacks two same-value readable frequency supports on each side"
    claim = [sample for sample in full_run if _claimable_for_current(sample, "primary_frequency")]
    if not claim:
        return None, "NO_PRODUCT_CLAIM", "unmute redraw has no product-unresolved frequency member"
    if any(right["video_frame_index"] != left["video_frame_index"] + 1
           for left, right in zip(claim, claim[1:])):
        return None, "NONCONTIGUOUS_PRODUCT_CLAIM", "unmute redraw product members are not one contiguous run"
    profiles = [_frequency_profile(sample, masks) for sample in chain]
    if any(profile is None for profile in profiles):
        return None, "FREQUENCY_PROFILE", "frequency redraw lacks canonical masks, clear holes, decimal, or segment profile"
    metrics, code, reason = _component_progress(
        profiles, FREQUENCY_COMPONENT_SEPARATION_MIN, FREQUENCY_PROGRESS_MIN,
        FREQUENCY_PROGRESS_MAX, FREQUENCY_MAXIMUM_BACKWARD_STEP,
        FREQUENCY_MAXIMUM_TOTAL_BACKWARD_MOTION)
    if metrics is None:
        return None, code, reason
    return {
        "event_id": event["event_id"], "classifier_id": FREQUENCY_CLASSIFIER_ID,
        "classifier_spec_sha256": FREQUENCY_CLASSIFIER_SPEC_SHA256,
        "status": "QUALIFIED_CAPTURE_TRANSITION", "raw_affected_fields": ["primary_frequency"],
        "video_frame_indices": [sample["video_frame_index"] for sample in claim],
        "first": _point(claim[0]), "last": _point(claim[-1]),
        "full_field_run_indices": [sample["video_frame_index"] for sample in full_run],
        "full_field_run_first": _point(full_run[0]), "full_field_run_last": _point(full_run[-1]),
        "left_support": [_point(sample) for sample in chain[:2]],
        "right_support": [_point(sample) for sample in chain[-2:]],
        "event_signature": signature, "expected_digit_masks": masks, **metrics,
        "maximum_support_chain_span_ns": MAXIMUM_SUPPORT_CHAIN_SPAN_NS,
        "verified_maximum_source_interval_ns": context["verified_maximum_source_interval_ns"],
        "capture_id": context["capture_id"],
        "selection_manifest_sha256": context["selection_manifest_sha256"],
        "reader_method_version": context["reader_method_version"],
        "reader_sha256": context["reader_sha256"],
        "redraw_probe_method_version": context["redraw_probe_method_version"],
        "redraw_probe_sha256": context["redraw_probe_sha256"],
        "basis": "A maximal raw stable-frequency refusal preserves canonical glyph geometry while every lit stroke advances componentwise from the dim to bright support profile; only product-unresolved members are claimed and raw frames remain unresolved.",
    }, None, None


def _reject(event, field, run, code, reason):
    return {"event_id": event.get("event_id"), "field": field, "code": code,
            "first": _point(run[0]), "last": _point(run[-1]), "reason": reason}


def classify_mute_redraw_runs(samples, events, context, *, classifier_ids=None):
    """Return badge/frequency candidates and stable refusals; never mutate input."""
    result = {"classifications": [], "rejected_runs": [], "errors": []}
    try:
        requested = set((BADGE_CLASSIFIER_ID, FREQUENCY_CLASSIFIER_ID)
                        if classifier_ids is None else classifier_ids)
        if not requested <= {BADGE_CLASSIFIER_ID, FREQUENCY_CLASSIFIER_ID}:
            raise ValueError("unknown mute redraw classifier requested")
        context = _context(context)
        originals = _unique_originals(samples)
        if not isinstance(events, list):
            raise ValueError("mute redraw events are not a list")
        for event in events:
            if (not isinstance(event, dict) or not isinstance(event.get("event_id"), str)
                    or type(event.get("start_ns")) is not int or type(event.get("end_ns")) is not int
                    or event["start_ns"] >= event["end_ns"]):
                raise ValueError("mute redraw event is malformed")
            selected = [sample for sample in originals
                        if type(sample.get("capture_ns")) is int
                        and event["start_ns"] <= sample["capture_ns"] < event["end_ns"]]

            if BADGE_CLASSIFIER_ID in requested:
                badge_signature = _mute_event_signature(event, False, True)
                for first, stop in _runs(selected, "muted_badge", "partial muted badge"):
                    run = selected[first:stop]
                    if badge_signature is None:
                        result["rejected_runs"].append(_reject(
                            event, "muted_badge", run, "EVENT_SCOPE",
                            "badge redraw is not the frozen mute-on-only event shape"))
                        continue
                    record, code, reason = _badge_record(
                        event, selected, first, stop, context, badge_signature)
                    if record is None:
                        result["rejected_runs"].append(_reject(event, "muted_badge", run, code, reason))
                    else:
                        result["classifications"].append(record)

            if FREQUENCY_CLASSIFIER_ID in requested:
                frequency_signature = _mute_event_signature(event, True, False)
                for first, stop in _runs(selected, "primary_frequency",
                                         "inconsistent illuminated frequency segment levels"):
                    run = selected[first:stop]
                    if frequency_signature is None:
                        result["rejected_runs"].append(_reject(
                            event, "primary_frequency", run, "EVENT_SCOPE",
                            "frequency redraw is not the frozen unmute-only event shape"))
                        continue
                    record, code, reason = _frequency_record(
                        event, selected, first, stop, context, frequency_signature)
                    if record is None:
                        result["rejected_runs"].append(_reject(
                            event, "primary_frequency", run, code, reason))
                    else:
                        result["classifications"].append(record)
    except (KeyError, TypeError, ValueError) as exc:
        result["classifications"] = []
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    return result
