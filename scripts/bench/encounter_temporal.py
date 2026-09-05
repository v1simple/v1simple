"""Classify narrowly supported camera-transition sequences without rewriting pixels.

The frame reader remains expectation-blind and every raw refusal remains in the
result.  This module may attach a named sequence-level interpretation only when
source-consecutive readable endpoints and field-specific diagnostics close the
ambiguous run.  Product policy decides whether a qualified classifier is allowed.
"""
from __future__ import annotations

from copy import deepcopy
import re


SECONDARY_CLASSIFIER_ID = "secondary-closed-meter-corroboration-v1"
SECONDARY_CONTEXT_CLASSIFIER_ID = "v1-secondary-closed-context-v3"
SECONDARY_OPTICAL_CLASSIFIER_ID = "v1-secondary-text-optical-bridge-v1"
SECONDARY_CLASSIFIER_SPEC_SHA256 = "31621b3356c3646094cf93bf95360f693ba91dd429a0c207e1ee7101c25153b0"
ARROW_CLASSIFIER_ID = "v1-arrow-phase-edge-v4"
ARROW_ACQUISITION_CLASSIFIER_ID = "v1-arrow-target-acquisition-v1"
BAR_CLASSIFIER_ID = "v1-main-bar-adjacent-redraw-v2"
BADGE_CLASSIFIER_ID = "v1-muted-badge-rising-fill-v2"
FREQUENCY_CLASSIFIER_ID = "v1-unmute-stable-frequency-sweep-v2"
FREQUENCY_CONTEXT_CLASSIFIER_ID = "v1-stable-frequency-closed-context-v3"
CLASSIFIER_IDS = (
    SECONDARY_CLASSIFIER_ID,
    SECONDARY_CONTEXT_CLASSIFIER_ID,
    SECONDARY_OPTICAL_CLASSIFIER_ID,
    ARROW_CLASSIFIER_ID,
    ARROW_ACQUISITION_CLASSIFIER_ID,
    BAR_CLASSIFIER_ID,
    BADGE_CLASSIFIER_ID,
    FREQUENCY_CLASSIFIER_ID,
    FREQUENCY_CONTEXT_CLASSIFIER_ID,
)
SECONDARY_MAX_ADJACENT_CAPTURE_GAP_NS = 10_000_000
SECONDARY_MAX_ENDPOINT_SPAN_NS = 50_000_000
_CARD_BANDS = {"X", "K", "Ka"}
_CARD_DIRECTIONS = {"front", "side", "rear"}
_CARD_FREQUENCY = re.compile(r"[0-9]{2}\.[0-9]{3}")
_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image")


def _point(sample):
    return {key: deepcopy(sample[key]) for key in _POINT_KEYS if key in sample}


def _consecutive(left, right):
    return (type(left.get("video_frame_index")) is int and
            type(left.get("source_frame_seq")) is int and
            right.get("video_frame_index") == left["video_frame_index"] + 1 and
            right.get("source_frame_seq") == left["source_frame_seq"] + 1 and
            type(left.get("capture_ns")) is int and type(right.get("capture_ns")) is int and
            0 < right["capture_ns"] - left["capture_ns"] <= SECONDARY_MAX_ADJACENT_CAPTURE_GAP_NS)


def _reading(sample, field):
    observed = sample.get("observed")
    fields = observed.get("fields") if isinstance(observed, dict) else None
    value = fields.get(field) if isinstance(fields, dict) else None
    return value if isinstance(value, dict) else {}


def _unique_originals(samples):
    originals = {}
    for sample in samples:
        index = sample.get("video_frame_index")
        if type(index) is not int:
            continue
        prior = originals.get(index)
        if prior is not None and prior.get("observed") != sample.get("observed"):
            raise ValueError("duplicate source image has conflicting temporal observations")
        originals.setdefault(index, sample)
    return [sample for _, sample in sorted(originals.items())]


def _ambiguous_runs(samples, field):
    """Yield maximal ambiguous/read-refusal runs with immediate list neighbors."""
    index = 0
    while index < len(samples):
        reading = _reading(samples[index], field)
        if reading.get("state") not in ("ambiguous", "unreadable"):
            index += 1
            continue
        end = index + 1
        while end < len(samples) and _consecutive(samples[end - 1], samples[end]) and \
                _reading(samples[end], field).get("state") in ("ambiguous", "unreadable"):
            end += 1
        yield index, end
        index = end


def _card_shape(card, expected_slot):
    if (not isinstance(card, dict) or card.get("slot") != expected_slot or
            card.get("band") not in _CARD_BANDS or
            card.get("direction") not in _CARD_DIRECTIONS or
            not isinstance(card.get("frequency"), str) or
            _CARD_FREQUENCY.fullmatch(card["frequency"]) is None):
        return None
    return {"band": card["band"], "frequency": card["frequency"],
            "direction": card["direction"]}


def _readable_secondary(reading):
    cards, value = reading.get("cards"), reading.get("value")
    if (reading.get("state") != "readable" or not isinstance(cards, list) or
            not 1 <= len(cards) <= 2 or not isinstance(value, list) or len(value) != len(cards)):
        return None
    result = []
    for slot, (card, literal) in enumerate(zip(cards, value)):
        identity = _card_shape(card, slot)
        if (identity is None or card.get("bars_state") != "readable" or
                type(card.get("bars")) is not int or not 0 <= card["bars"] <= 6 or
                not isinstance(literal, dict)):
            return None
        exact = {**identity, "bars": card["bars"]}
        if literal != exact:
            return None
        result.append(exact)
    return result


def _partial_secondary(reading):
    cards = reading.get("cards")
    if (reading.get("state") not in ("ambiguous", "unreadable") or
            not isinstance(cards, list) or not 1 <= len(cards) <= 2):
        return None
    result, ambiguous = [], False
    for slot, card in enumerate(cards):
        identity = _card_shape(card, slot)
        if identity is None:
            return None
        if card.get("bars_state") == "readable" and type(card.get("bars")) is int:
            if not 0 <= card["bars"] <= 6:
                return None
            compatible = [card["bars"]]
        elif card.get("bars_state") == "ambiguous" and card.get("bars") is None:
            compatible = card.get("compatible_bars")
            ambiguous = True
        else:
            return None
        if (not isinstance(compatible, list) or not compatible or
                any(type(value) is not int or not 0 <= value <= 6 for value in compatible)):
            return None
        result.append({**identity, "compatible_bars": set(compatible)})
    return result if ambiguous else None


def _secondary(left, run, right):
    left_reading, right_reading = _reading(left, "secondary"), _reading(right, "secondary")
    left_value, right_value = _readable_secondary(left_reading), _readable_secondary(right_reading)
    if left_value is None or right_value is None or left_value != right_value:
        return None, "secondary endpoints are not the same readable card state"
    if right["capture_ns"] - left["capture_ns"] > SECONDARY_MAX_ENDPOINT_SPAN_NS:
        return None, "secondary endpoint span exceeds one display update period"
    endpoints = left_value
    intersection = [None] * len(endpoints)
    for sample in run:
        partial = _partial_secondary(_reading(sample, "secondary"))
        if partial is None or len(partial) != len(endpoints):
            return None, "ambiguous secondary frame lacks complete identity and bar compatibility"
        for slot, (actual, endpoint) in enumerate(zip(partial, endpoints)):
            if any(actual[key] != endpoint.get(key) for key in ("band", "frequency", "direction")):
                return None, "secondary card identity or direction changes inside the ambiguous run"
            values = actual["compatible_bars"]
            intersection[slot] = values if intersection[slot] is None else intersection[slot] & values
    if any(values != {endpoint.get("bars")} for values, endpoint in zip(intersection, endpoints)):
        return None, "secondary compatible-count intersection does not uniquely close on the endpoints"
    return deepcopy(endpoints), None


def _requested_classifier_ids(value):
    if (not isinstance(value, (list, tuple))
            or any(not isinstance(item, str) or not item for item in value)
            or len(value) != len(set(value))):
        raise ValueError("temporal classifier request is malformed")
    unknown = set(value) - set(CLASSIFIER_IDS)
    if unknown:
        raise ValueError("unknown temporal classifier requested: " + ", ".join(sorted(unknown)))
    return set(value)


def _tagged_rejections(records, classifier_id):
    """Attach dispatcher provenance without mutating classifier-owned records."""
    tagged = []
    for record in records:
        if not isinstance(record, dict):
            raise ValueError(f"{classifier_id} emitted a malformed rejection")
        owner = record.get("classifier_id")
        if owner is not None and owner != classifier_id:
            raise ValueError(f"{classifier_id} emitted rejection for {owner}")
        tagged.append({**deepcopy(record), "classifier_id": classifier_id})
    return tagged


def classify_temporal(samples, sequence, arrow_context=None, *, classifier_ids):
    """Return field-specific temporal records; never mutate raw sample evidence."""
    result = {"schema_version": 1, "classifications": [], "rejected_runs": [], "errors": []}
    try:
        requested = _requested_classifier_ids(classifier_ids)
        originals = _unique_originals(samples)
        if SECONDARY_CLASSIFIER_ID in requested:
            for event in sequence.get("events", []):
                start, end = event.get("start_ns"), event.get("end_ns")
                if type(start) is not int or type(end) is not int or start >= end:
                    raise ValueError("temporal event bounds are invalid")
                selected = [sample for sample in originals
                            if type(sample.get("capture_ns")) is int and start <= sample["capture_ns"] < end]
                for first, stop in _ambiguous_runs(selected, "secondary"):
                    run = selected[first:stop]
                    reason = None
                    if first == 0 or stop == len(selected):
                        reason = "ambiguous secondary run has no two immediate endpoints"
                    else:
                        left, right = selected[first - 1], selected[stop]
                        chain = [left, *run, right]
                        if not all(_consecutive(a, b) for a, b in zip(chain, chain[1:])):
                            reason = "secondary run or its endpoints cross an unobserved source position"
                        else:
                            resolved, reason = _secondary(left, run, right)
                    if reason:
                        result["rejected_runs"].append({"event_id": event.get("event_id"),
                            "classifier_id": SECONDARY_CLASSIFIER_ID,
                            "field": "secondary", "first": _point(run[0]), "last": _point(run[-1]),
                            "reason": reason})
                        continue
                    result["classifications"].append({
                        "event_id": event.get("event_id"),
                        "classifier_id": SECONDARY_CLASSIFIER_ID,
                        "classifier_spec_sha256": SECONDARY_CLASSIFIER_SPEC_SHA256,
                        "status": "QUALIFIED_CAPTURE_TRANSITION",
                        "raw_affected_fields": ["secondary"],
                        "video_frame_indices": [sample["video_frame_index"] for sample in run],
                        "first": _point(run[0]), "last": _point(run[-1]),
                        "left_endpoint": _point(left), "right_endpoint": _point(right),
                        "resolved_value": resolved,
                        "basis": "Source-consecutive ambiguous meter frames preserve complete card identity; "
                                 "their definite cells intersect at the one readable bar count present on both endpoints. "
                                 "Raw frames remain unresolved."
                    })
        if SECONDARY_CONTEXT_CLASSIFIER_ID in requested and any(
               _reading(sample, "secondary").get("state") == "unreadable"
               for sample in originals):
            try:
                from .encounter_secondary_context import classify_secondary_context_runs
            except ImportError:
                from encounter_secondary_context import classify_secondary_context_runs
            secondary_context = classify_secondary_context_runs(
                originals, sequence.get("events", []), arrow_context)
            result["classifications"].extend(secondary_context["classifications"])
            result["rejected_runs"].extend(_tagged_rejections(
                secondary_context["rejected_runs"], SECONDARY_CONTEXT_CLASSIFIER_ID))
            result["errors"].extend(secondary_context["errors"])
        if SECONDARY_OPTICAL_CLASSIFIER_ID in requested and any(
               _reading(sample, "secondary").get("state") == "unreadable"
               for sample in originals):
            try:
                from .encounter_secondary_optical_bridge import classify_secondary_optical_bridge
            except ImportError:
                from encounter_secondary_optical_bridge import classify_secondary_optical_bridge
            secondary_optical = classify_secondary_optical_bridge(
                originals, sequence.get("events", []), arrow_context)
            result["classifications"].extend(secondary_optical["classifications"])
            result["rejected_runs"].extend(_tagged_rejections(
                secondary_optical["rejected_runs"], SECONDARY_OPTICAL_CLASSIFIER_ID))
            result["errors"].extend(secondary_optical["errors"])
        if ARROW_CLASSIFIER_ID in requested and any(
               _reading(sample, "main_arrows").get("state") == "ambiguous"
               for sample in originals):
            try:
                from .encounter_arrow_transition import classify_arrow_runs
            except ImportError:
                from encounter_arrow_transition import classify_arrow_runs
            arrow = classify_arrow_runs(originals, sequence.get("events", []), arrow_context)
            result["classifications"].extend(arrow["classifications"])
            result["rejected_runs"].extend(_tagged_rejections(
                arrow["rejected_runs"], ARROW_CLASSIFIER_ID))
            result["errors"].extend(arrow["errors"])
        if ARROW_ACQUISITION_CLASSIFIER_ID in requested and any(
               _reading(sample, "main_arrows").get("state") == "ambiguous"
               for sample in originals):
            try:
                from .encounter_arrow_acquisition import classify_arrow_acquisition_runs
            except ImportError:
                from encounter_arrow_acquisition import classify_arrow_acquisition_runs
            acquisition = classify_arrow_acquisition_runs(
                originals, sequence.get("events", []), arrow_context)
            result["classifications"].extend(acquisition["classifications"])
            result["rejected_runs"].extend(_tagged_rejections(
                acquisition["rejected_runs"], ARROW_ACQUISITION_CLASSIFIER_ID))
            result["errors"].extend(acquisition["errors"])
        if BAR_CLASSIFIER_ID in requested and any(
               _reading(sample, "main_bars").get("state") == "ambiguous"
               for sample in originals):
            try:
                from .encounter_bar_transition import classify_main_bar_runs
            except ImportError:
                from encounter_bar_transition import classify_main_bar_runs
            bars = classify_main_bar_runs(originals, sequence.get("events", []), arrow_context)
            result["classifications"].extend(bars["classifications"])
            result["rejected_runs"].extend(_tagged_rejections(
                bars["rejected_runs"], BAR_CLASSIFIER_ID))
            result["errors"].extend(bars["errors"])
        mute_fields = []
        if BADGE_CLASSIFIER_ID in requested:
            mute_fields.append("muted_badge")
        if FREQUENCY_CLASSIFIER_ID in requested:
            mute_fields.append("primary_frequency")
        if any(_reading(sample, field).get("state") == "ambiguous"
               for sample in originals for field in mute_fields):
            try:
                from .encounter_mute_redraw_transition import classify_mute_redraw_runs
            except ImportError:
                from encounter_mute_redraw_transition import classify_mute_redraw_runs
            mute_redraw = classify_mute_redraw_runs(
                originals, sequence.get("events", []), arrow_context,
                classifier_ids=sorted(requested & {BADGE_CLASSIFIER_ID, FREQUENCY_CLASSIFIER_ID}))
            result["classifications"].extend(
                record for record in mute_redraw["classifications"]
                if record.get("classifier_id") in requested)
            requested_fields = set(mute_fields)
            for record in mute_redraw["rejected_runs"]:
                owner = ({"muted_badge": BADGE_CLASSIFIER_ID,
                          "primary_frequency": FREQUENCY_CLASSIFIER_ID}
                         .get(record.get("field")))
                if owner in requested and record.get("field") in requested_fields:
                    result["rejected_runs"].extend(_tagged_rejections([record], owner))
            result["errors"].extend(mute_redraw["errors"])
        if FREQUENCY_CONTEXT_CLASSIFIER_ID in requested and any(
               _reading(sample, "primary_frequency").get("state") == "ambiguous"
               for sample in originals):
            try:
                from .encounter_frequency_context import classify_frequency_context_runs
            except ImportError:
                from encounter_frequency_context import classify_frequency_context_runs
            frequency_context = classify_frequency_context_runs(
                originals, sequence.get("events", []), arrow_context)
            result["classifications"].extend(frequency_context["classifications"])
            result["rejected_runs"].extend(_tagged_rejections(
                frequency_context["rejected_runs"], FREQUENCY_CONTEXT_CLASSIFIER_ID))
            result["errors"].extend(frequency_context["errors"])
    except (KeyError, TypeError, ValueError) as exc:
        result["classifications"] = []
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    return result
