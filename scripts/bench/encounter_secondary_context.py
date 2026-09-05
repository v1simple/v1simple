"""Conservative closed-context candidates for secondary-card meter refusals.

The ordinary reader remains the sole owner of every single-frame value.  This
module emits a separate legal-presentation record only when a maximal raw
secondary refusal preserves exact card identity and a boundary-adjacent meter
partial between two source-consecutive, stable pairs of the same current card
state.  Raw samples are never rewritten.
"""
from __future__ import annotations

from copy import deepcopy
import re


CLASSIFIER_ID = "v1-secondary-closed-context-v3"
CLASSIFIER_SPEC_SHA256 = "464542ae31277827eb5919c42f65c7f359235ab764388f5c66332b2c4a9b907c"
DEADLINE_OBSERVATION_SEMANTICS = "LEGAL_PRESENTATION_TRANSITION"
VERIFICATION_CLOSURE_SEMANTICS = "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"

PROFILE_READER_METHOD_VERSION = 7
PROFILE_READER_SHA256 = "f4efd6a1df4daefb3e7271a2e229e80f7378ba8b7a824dea1593aa0581f20b7c"

MAXIMUM_RECORDING_SOURCE_INTERVAL_NS = 1_000_000_000
MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS = 10_000_000
AUTHORED_DISPLAY_UPDATE_NS = 50_000_000
MAXIMUM_SUPPORT_CHAIN_SPAN_NS = 80_000_000
MAXIMUM_INTERLEAVED_READABLE_FRAMES = 2
STABLE_SUPPORT_FRAMES_EACH_SIDE = 2

RAW_UNREADABLE_REASON = "secondary card text, direction, or bars are not fully readable"
RAW_PARTIAL_METER_REASON = "partial, faint or noncontiguous secondary strength bars"

_FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
           "main_bars", "secondary", "muted_badge")
_CARD_KEYS = ("band", "frequency", "direction", "bars")
_CARD_BANDS = {"X", "K", "Ka"}
_CARD_DIRECTIONS = {"front", "side", "rear"}
_FREQUENCY = re.compile(r"[0-9]{2}\.[0-9]{3}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image", "image_sha256")


def _point(sample):
    return {key: deepcopy(sample[key]) for key in _POINT_KEYS if key in sample}


def _reject(event, run, code, reason):
    return {"event_id": event.get("event_id"), "field": "secondary", "code": code,
            "first": _point(run[0]), "last": _point(run[-1]), "reason": reason}


def _reading(sample):
    observed = sample.get("observed")
    fields = observed.get("fields") if isinstance(observed, dict) else None
    value = fields.get("secondary") if isinstance(fields, dict) else None
    return value if isinstance(value, dict) else {}


def _unique_originals(samples):
    if not isinstance(samples, list):
        raise ValueError("secondary context samples are not a list")
    originals = {}
    for sample in samples:
        if not isinstance(sample, dict):
            raise ValueError("secondary context sample is not an object")
        index = sample.get("video_frame_index")
        if type(index) is not int:
            continue
        prior = originals.get(index)
        if prior is not None and any(prior.get(key) != sample.get(key)
                                     for key in ("observed", "expected", "comparison")):
            raise ValueError("duplicate source image has conflicting secondary evidence")
        originals.setdefault(index, sample)
    return [sample for _, sample in sorted(originals.items())]


def _context(value):
    if not isinstance(value, dict):
        raise ValueError("secondary context classifier context is missing")
    required = ("capture_id", "selection_manifest_sha256",
                "verified_maximum_source_interval_ns", "reader_method_version",
                "reader_sha256")
    if any(key not in value for key in required):
        raise ValueError("secondary context classifier context is incomplete")
    if (_SHA256.fullmatch(str(value["capture_id"])) is None
            or _SHA256.fullmatch(str(value["selection_manifest_sha256"])) is None
            or type(value["verified_maximum_source_interval_ns"]) is not int
            or not 0 < value["verified_maximum_source_interval_ns"]
            <= MAXIMUM_RECORDING_SOURCE_INTERVAL_NS
            or value["reader_method_version"] != PROFILE_READER_METHOD_VERSION
            or value["reader_sha256"] != PROFILE_READER_SHA256):
        raise ValueError("secondary context does not match the frozen reader/capture contract")
    return value


def _consecutive(left, right, maximum_gap_ns):
    return (type(left.get("video_frame_index")) is int
            and type(left.get("source_frame_seq")) is int
            and right.get("video_frame_index") == left["video_frame_index"] + 1
            and right.get("source_frame_seq") == left["source_frame_seq"] + 1
            and type(left.get("capture_ns")) is int
            and type(right.get("capture_ns")) is int
            and 0 < right["capture_ns"] - left["capture_ns"] <= maximum_gap_ns)


def _resolved_allowed(target, field):
    fields = target.get("fields") if isinstance(target, dict) else None
    spec = fields.get(field) if isinstance(fields, dict) else None
    allowed = spec.get("allowed") if isinstance(spec, dict) else None
    if (not isinstance(allowed, list) or len(allowed) != 1
            or "unresolved" in spec):
        return None
    return deepcopy(allowed[0])


def _canonical_card(value, slot):
    if (not isinstance(value, dict) or set(value) != set(_CARD_KEYS)
            or value.get("band") not in _CARD_BANDS
            or not isinstance(value.get("frequency"), str)
            or _FREQUENCY.fullmatch(value["frequency"]) is None
            or value.get("direction") not in _CARD_DIRECTIONS
            or type(value.get("bars")) is not int
            or not 0 <= value["bars"] <= 6):
        return None
    return {key: deepcopy(value[key]) for key in _CARD_KEYS}


def _canonical_value(value):
    if not isinstance(value, list) or not 1 <= len(value) <= 2:
        return None
    cards = [_canonical_card(card, slot) for slot, card in enumerate(value)]
    return cards if all(card is not None for card in cards) else None


def _meter_states(bar_reading):
    bars = bar_reading.get("bars") if isinstance(bar_reading, dict) else None
    if not isinstance(bars, list) or len(bars) != 6:
        return None
    states = []
    for bar in bars:
        state = bar.get("state") if isinstance(bar, dict) else None
        if state not in {"on", "off", "partial"}:
            return None
        states.append(state)
    return states


def _compatible_counts(states):
    return [count for count in range(7) if all(
        state == "partial" or (state == "on" and index < count)
        or (state == "off" and index >= count)
        for index, state in enumerate(states))]


def _identity_evidence(card, expected, slot):
    if (not isinstance(card, dict) or card.get("slot") != slot
            or any(card.get(key) != expected[key]
                   for key in ("band", "frequency", "direction"))
            or card.get("text_visible") is not True):
        return False
    candidates = card.get("ocr_candidates")
    normalized = []
    if not isinstance(candidates, list) or not candidates:
        return False
    for candidate in candidates:
        if (not isinstance(candidate, (list, tuple)) or len(candidate) != 2
                or not all(isinstance(item, str) for item in candidate)):
            return False
        normalized.append(tuple(candidate))
    if set(normalized) != {(expected["band"], expected["frequency"])}:
        return False
    direction = card.get("direction_reading")
    return (isinstance(direction, dict)
            and direction.get("state") == "readable"
            and direction.get("value") == expected["direction"]
            and direction.get("reason") is None
            and direction.get("shape_matches") == [expected["direction"]])


def _card_evidence(card, expected, slot, *, allow_partial):
    if not _identity_evidence(card, expected, slot):
        return None, "card text or direction evidence does not preserve the support identity"
    bar_reading = card.get("bar_reading")
    states = _meter_states(bar_reading)
    if states is None:
        return None, "card meter does not retain six explicit cell states"
    compatible = _compatible_counts(states)
    if (bar_reading.get("compatible_counts") != compatible
            or card.get("compatible_bars") != compatible
            or card.get("bars_state") != bar_reading.get("state")):
        return None, "card meter compatible-count evidence is internally inconsistent"

    count = expected["bars"]
    definite = ["on" if index < count else "off" for index in range(6)]
    if bar_reading.get("state") == "readable":
        if (card.get("bars") != count or bar_reading.get("value") != count
                or bar_reading.get("reason") is not None or states != definite
                or compatible != [count]):
            return None, "readable card meter does not equal the support count"
        return {"slot": slot, "state": "readable", "bars": count,
                "compatible_bars": compatible, "cell_states": states}, None

    partials = [index for index, state in enumerate(states) if state == "partial"]
    if (not allow_partial or bar_reading.get("state") != "ambiguous"
            or card.get("bars") is not None or bar_reading.get("value") is not None
            or bar_reading.get("reason") != RAW_PARTIAL_METER_REASON
            or not partials or count not in compatible or not compatible):
        return None, "ambiguous card meter does not remain compatible with the support count"
    return {"slot": slot, "state": "partial", "bars": None,
            "partial_cells": partials, "compatible_bars": compatible,
            "cell_states": states}, None


def _readable_secondary(sample):
    reading = _reading(sample)
    value = _canonical_value(reading.get("value"))
    cards = reading.get("cards")
    if (reading.get("state") != "readable" or reading.get("reason") is not None
            or value is None or not isinstance(cards, list) or len(cards) != len(value)):
        return None
    for slot, (card, expected) in enumerate(zip(cards, value)):
        evidence, _ = _card_evidence(card, expected, slot, allow_partial=False)
        if evidence is None:
            return None
    return value


def _partial_secondary(sample, expected_value):
    reading = _reading(sample)
    cards = reading.get("cards")
    if (reading.get("state") != "unreadable" or reading.get("value") is not None
            or reading.get("reason") != RAW_UNREADABLE_REASON
            or reading.get("ocr_available") is not True
            or not isinstance(cards, list) or len(cards) != len(expected_value)):
        return None, "secondary refusal does not retain the frozen reader shape"
    evidence = []
    for slot, (card, expected) in enumerate(zip(cards, expected_value)):
        current, reason = _card_evidence(card, expected, slot, allow_partial=True)
        if current is None:
            return None, reason
        evidence.append(current)
    projected = [{key: card.get(key) for key in _CARD_KEYS} for card in cards]
    if reading.get("partial_cards") != projected:
        return None, "secondary partial-card summary disagrees with card evidence"
    if not any(card["state"] == "partial" for card in evidence):
        return None, "secondary refusal contains no compatible partial meter"
    return evidence, None


def _comparison_matches(sample, *, refusal):
    comparison = sample.get("comparison")
    checks = comparison.get("checks") if isinstance(comparison, dict) else None
    joint = comparison.get("joint_state") if isinstance(comparison, dict) else None
    if not isinstance(checks, dict) or set(checks) != set(_FIELDS) or not isinstance(joint, dict):
        return False
    statuses = {field: checks[field].get("status")
                if isinstance(checks[field], dict) else None for field in _FIELDS}
    if refusal:
        return (statuses["secondary"] == "UNRESOLVED"
                and all(statuses[field] in {"MATCH", "UNRESOLVED", "CONDITIONAL"}
                        for field in _FIELDS if field != "secondary")
                and joint.get("status") in {"MATCH", "UNRESOLVED"})
    return (statuses["secondary"] == "MATCH"
            and all(status in {"MATCH", "UNRESOLVED", "CONDITIONAL"}
                    for status in statuses.values())
            and joint.get("status") in {"MATCH", "UNRESOLVED"})


def _is_refusal(sample):
    reading = _reading(sample)
    return (reading.get("state") == "unreadable"
            and reading.get("reason") == RAW_UNREADABLE_REASON)


def _episodes(selected, first_current_position=None):
    """Yield raw-defined refusal contexts with at most two readable bridge frames."""
    index = 0
    while index < len(selected):
        if not _is_refusal(selected[index]):
            index += 1
            continue
        first = index
        last_refusal = index
        readable_bridge = 0
        cursor = index + 1
        while cursor < len(selected):
            if cursor == first_current_position:
                break
            if _is_refusal(selected[cursor]):
                last_refusal = cursor
                readable_bridge = 0
                cursor += 1
                continue
            if _reading(selected[cursor]).get("state") == "readable":
                readable_bridge += 1
                if readable_bridge <= MAXIMUM_INTERLEAVED_READABLE_FRAMES:
                    cursor += 1
                    continue
            break
        stop = last_refusal + 1
        yield first, stop
        index = stop


def _refusal_runs(selected, first, stop):
    index = first
    while index < stop:
        if not _is_refusal(selected[index]):
            index += 1
            continue
        end = index + 1
        while end < stop and _is_refusal(selected[end]):
            end += 1
        yield selected[index:end]
        index = end


def _support_chain(selected, first, stop, context):
    episode = selected[first:stop]
    refusals = [sample for sample in episode if _is_refusal(sample)]
    if not refusals:
        return None, "EMPTY_EPISODE", "secondary context contains no refusal frame"
    if (first < STABLE_SUPPORT_FRAMES_EACH_SIDE
            or stop + STABLE_SUPPORT_FRAMES_EACH_SIDE > len(selected)):
        return None, "UNCLOSED_RUN", "secondary refusal context lacks two immediate readable anchors on each side"
    chain = selected[first - 2:stop + 2]
    maximum_gap = min(context["verified_maximum_source_interval_ns"],
                      MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS)
    if not all(_consecutive(left, right, maximum_gap)
               for left, right in zip(chain, chain[1:])):
        return None, "SOURCE_GAP", "secondary support context crosses an unobserved source position"
    if (refusals[-1]["capture_ns"] - refusals[0]["capture_ns"]
            > AUTHORED_DISPLAY_UPDATE_NS + maximum_gap):
        return None, "CONTEXT_SPAN", "secondary refusal context exceeds one display update plus the effective source gap"
    if chain[-1]["capture_ns"] - chain[0]["capture_ns"] > MAXIMUM_SUPPORT_CHAIN_SPAN_NS:
        return None, "SUPPORT_SPAN", "secondary support context exceeds 80 ms"
    return chain, None, None


def _support_value(chain):
    supports = chain[:2] + chain[-2:]
    values = [_readable_secondary(sample) for sample in supports]
    if any(value is None for value in values) or any(value != values[0] for value in values[1:]):
        return None, "SUPPORT_VALUE", "secondary anchors are not one same exact readable card state"
    if any(not _comparison_matches(sample, refusal=False) for sample in supports):
        return None, "SUPPORT_COMPARISON", "secondary anchors are not exact current secondary comparisons free of definite noncurrent fields"
    return values[0], None, None


def _current_target_secondary(event, chain, support_value):
    if _resolved_allowed(event.get("target"), "secondary") != support_value:
        return "TARGET_MISMATCH", "support-derived secondary value does not equal the exact event target"
    if any(_resolved_allowed(sample.get("expected"), "secondary") != support_value
           for sample in chain):
        return "TARGET_MISMATCH", "support context does not share one exact current secondary expectation"
    return None, None


def _current_presentation_established(event, selected, refusals):
    first_correct = event.get("first_correct")
    established_ns = (first_correct.get("capture_ns")
                      if isinstance(first_correct, dict) else None)
    if type(established_ns) is not int:
        return None, "TARGET_ACQUISITION_CONTEXT", "event has no observed fully current presentation before the secondary refusal"
    point_keys = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
                  "offset_seconds", "image")
    established = [sample for sample in selected
                   if all(key in first_correct and sample.get(key) == first_correct[key]
                          for key in point_keys)]
    if (len(established) != 1
            or not _comparison_matches(established[0], refusal=False)
            or any(established[0]["comparison"]["checks"][field]["status"] != "MATCH"
                   for field in _FIELDS)
            or established[0]["comparison"]["joint_state"].get("status") != "MATCH"):
        return None, "TARGET_ACQUISITION_CONTEXT", "event first-correct marker is not bound to one literal fully current sample"
    if any(sample["capture_ns"] < established_ns for sample in refusals):
        return None, "TARGET_ACQUISITION_CONTEXT", "secondary refusal precedes the event's first observed fully current presentation"
    return _point(established[0]), None, None


def _record(event, selected, first, stop, context):
    episode = selected[first:stop]
    refusal_runs = list(_refusal_runs(selected, first, stop))
    refusals = [sample for run in refusal_runs for sample in run]
    chain, code, reason = _support_chain(selected, first, stop, context)
    if chain is None:
        return None, code, reason

    support_value, code, reason = _support_value(chain)
    if support_value is None:
        return None, code, reason

    code, reason = _current_target_secondary(event, chain, support_value)
    if code is not None:
        return None, code, reason
    established, code, reason = _current_presentation_established(
        event, selected, refusals)
    if established is None:
        return None, code, reason
    interior_readable = [sample for sample in episode if not _is_refusal(sample)]
    if any(_readable_secondary(sample) != support_value
           or not _comparison_matches(sample, refusal=False)
           for sample in interior_readable):
        return None, "INTERIOR_CONTEXT", "interleaved readable frame does not preserve the exact current secondary context"
    if any(not _comparison_matches(sample, refusal=True) for sample in refusals):
        return None, "PRODUCT_FIELD_SCOPE", "secondary refusal has a definite noncurrent field or invalid comparison context"

    meter_evidence = []
    for sample in refusals:
        cards, reason = _partial_secondary(sample, support_value)
        if cards is None:
            return None, "CARD_REDRAW_CONTEXT", reason
        meter_evidence.append({"video_frame_index": sample["video_frame_index"],
                               "cards": cards})

    intersections = []
    for slot, expected in enumerate(support_value):
        compatible = [set(frame["cards"][slot]["compatible_bars"])
                      for frame in meter_evidence]
        intersection = set.intersection(*compatible)
        if intersection != {expected["bars"]}:
            return None, "METER_COMPATIBILITY", "secondary context compatible-count intersection does not uniquely close on the support count"
        intersections.append([expected["bars"]])

    maximum_gap = min(context["verified_maximum_source_interval_ns"],
                      MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS)
    full_context_indices = [sample["video_frame_index"] for sample in chain]
    refusal_indices = [sample["video_frame_index"] for sample in refusals]
    readable_indices = [sample["video_frame_index"] for sample in interior_readable]
    records = []
    for run in refusal_runs:
        indices = [sample["video_frame_index"] for sample in run]
        if any(right != left + 1 for left, right in zip(indices, indices[1:])):
            return None, "NONCONTIGUOUS_PRODUCT_CLAIM", "secondary product claim is not contiguous"
        records.append({
            "event_id": event["event_id"],
            "classifier_id": CLASSIFIER_ID,
            "classifier_spec_sha256": CLASSIFIER_SPEC_SHA256,
            "status": "QUALIFIED_CAPTURE_TRANSITION",
            "deadline_observation_semantics": DEADLINE_OBSERVATION_SEMANTICS,
            "verification_closure_semantics": VERIFICATION_CLOSURE_SEMANTICS,
            "auxiliary_closure_context_ns": MAXIMUM_SUPPORT_CHAIN_SPAN_NS,
            "raw_affected_fields": ["secondary"],
            "video_frame_indices": indices,
            "first": _point(run[0]),
            "last": _point(run[-1]),
            "full_context_indices": full_context_indices,
            "context_frame_indices": full_context_indices,
            "context_refusal_indices": refusal_indices,
            "interleaved_readable_indices": readable_indices,
            "context_first": _point(refusals[0]),
            "context_last": _point(refusals[-1]),
            "left_support": [_point(sample) for sample in chain[:2]],
            "right_support": [_point(sample) for sample in chain[-2:]],
            "current_presentation_established": deepcopy(established),
            "support_derived_secondary": deepcopy(support_value),
            "resolved_value": deepcopy(support_value),
            "partial_meter_evidence": deepcopy(meter_evidence),
            "compatible_bar_intersections": deepcopy(intersections),
            "maximum_interleaved_readable_frames": MAXIMUM_INTERLEAVED_READABLE_FRAMES,
            "maximum_context_refusal_span_ns": AUTHORED_DISPLAY_UPDATE_NS + maximum_gap,
            "maximum_support_chain_span_ns": MAXIMUM_SUPPORT_CHAIN_SPAN_NS,
            "maximum_support_chain_interval_ns": maximum_gap,
            "verified_maximum_source_interval_ns": context["verified_maximum_source_interval_ns"],
            "capture_id": context["capture_id"],
            "selection_manifest_sha256": context["selection_manifest_sha256"],
            "reader_method_version": context["reader_method_version"],
            "reader_sha256": context["reader_sha256"],
            "event_signature": {
                "mode": event.get("mode"),
                "changed_fields": deepcopy(event.get("changed_fields")),
                "current_secondary": deepcopy(support_value),
            },
            "basis": "A bounded secondary context preserves exact current card identity through interleaved readable frames, and all raw refusals jointly close their compatible meter counts on that one state between source-consecutive stable anchors; raw frames remain unresolved.",
        })
    return records, None, None


def classify_secondary_context_runs(samples, events, context):
    """Return closed secondary-context candidates and explicit refusals."""
    result = {"classifications": [], "rejected_runs": [], "errors": []}
    try:
        context = _context(context)
        originals = _unique_originals(samples)
        if not isinstance(events, list):
            raise ValueError("secondary context events are not a list")
        for event in events:
            if (not isinstance(event, dict) or not isinstance(event.get("event_id"), str)
                    or type(event.get("start_ns")) is not int
                    or type(event.get("end_ns")) is not int
                    or event["start_ns"] >= event["end_ns"]):
                raise ValueError("secondary context event is malformed")
            selected = [sample for sample in originals
                        if type(sample.get("capture_ns")) is int
                        and event["start_ns"] <= sample["capture_ns"] < event["end_ns"]]
            established, _, _ = _current_presentation_established(
                event, selected, [])
            first_current_position = (next(
                (position for position, sample in enumerate(selected)
                 if sample.get("video_frame_index") ==
                 established.get("video_frame_index")), None)
                if established is not None else None)
            for first, stop in _episodes(selected, first_current_position):
                episode = selected[first:stop]
                records, code, reason = _record(event, selected, first, stop, context)
                if records is None:
                    result["rejected_runs"].append(_reject(event, episode, code, reason))
                else:
                    result["classifications"].extend(records)
    except (KeyError, TypeError, ValueError) as exc:
        result["classifications"] = []
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    return result
