"""Corroborate one bounded secondary OCR band-token omission optically.

Reader7 remains the sole owner of raw single-frame values.  This classifier
can emit separate legal-presentation evidence only when one raw OCR refusal is
inside a source-consecutive five-frame bracket of the exact same current card
state and its fixed full-text RGB profile remains inside the support envelope.
"""
from __future__ import annotations

import base64
import binascii
from copy import deepcopy
import hashlib
from itertools import combinations
import re

import numpy as np


CLASSIFIER_ID = "v1-secondary-text-optical-bridge-v1"
CLASSIFIER_SPEC_SHA256 = "f8bbbb1cb962ed404143d7e2a3a0ef05e4c68eeb1adc5c069b284b82ac40060a"
DEADLINE_OBSERVATION_SEMANTICS = "LEGAL_PRESENTATION_TRANSITION"

PROFILE_READER_METHOD_VERSION = 7
PROFILE_READER_SHA256 = "f4efd6a1df4daefb3e7271a2e229e80f7378ba8b7a824dea1593aa0581f20b7c"
PROFILE_SECONDARY_PROBE_METHOD_VERSION = 1
PROFILE_SECONDARY_PROBE_SHA256 = "e4fed63ff3ad90e569f4f9992b35ef1275d2c6c54fd35ec56bdc4159a60f5f4b"

MAXIMUM_RECORDING_SOURCE_INTERVAL_NS = 1_000_000_000
MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS = 10_000_000
MAXIMUM_SUPPORT_CHAIN_SPAN_NS = 25_000_000
STABLE_SUPPORT_FRAMES_EACH_SIDE = 2
MINIMUM_OCR_CONFIDENCE = .95
MAXIMUM_SUPPORT_PAIR_RMS = 4.0
MAXIMUM_SUPPORT_COMPONENT_SPAN = 16
MAXIMUM_TARGET_SUPPORT_RMS = 4.0
MAXIMUM_TARGET_ENVELOPE_EXCURSION = 8

RAW_UNREADABLE_REASON = "secondary card text, direction, or bars are not fully readable"
_FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
           "main_bars", "secondary", "muted_badge")
_CARD_KEYS = ("band", "frequency", "direction", "bars")
_CARD_BANDS = {"X", "K", "Ka"}
_CARD_DIRECTIONS = {"front", "side", "rear"}
_FREQUENCY = re.compile(r"[0-9]{2}\.[0-9]{3}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image", "image_sha256")
_POINT_ID_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns")
_TEXT_BOXES = ((440, 377, 621, 413), (687, 377, 868, 413))
_PROFILE_BYTE_COUNT = 9 * 46 * 3
_PROFILE_SCHEMA = {
    "rows": 9,
    "columns": 46,
    "channels": ["red", "green", "blue"],
    "order": "row-major cells with RGB-interleaved uint8 components",
    "sample": "rounded arithmetic mean of registered RGB pixels",
    "normalization": "none",
}


def _point(sample):
    return {key: deepcopy(sample[key]) for key in _POINT_KEYS if key in sample}


def _reject(event, sample, code, reason):
    point = _point(sample)
    return {"event_id": event.get("event_id"), "field": "secondary", "code": code,
            "first": point, "last": deepcopy(point), "reason": reason}


def _reading(sample):
    observed = sample.get("observed")
    fields = observed.get("fields") if isinstance(observed, dict) else None
    reading = fields.get("secondary") if isinstance(fields, dict) else None
    return reading if isinstance(reading, dict) else {}


def _unique_originals(samples):
    if not isinstance(samples, list):
        raise ValueError("secondary optical samples are not a list")
    originals = {}
    for sample in samples:
        if not isinstance(sample, dict):
            raise ValueError("secondary optical sample is not an object")
        index = sample.get("video_frame_index")
        if type(index) is not int:
            continue
        prior = originals.get(index)
        if prior is not None and any(prior.get(key) != sample.get(key)
                                     for key in ("observed", "expected", "comparison")):
            raise ValueError("duplicate source image has conflicting secondary optical evidence")
        originals.setdefault(index, sample)
    return [sample for _, sample in sorted(originals.items())]


def _context(value):
    if not isinstance(value, dict):
        raise ValueError("secondary optical classifier context is missing")
    required = ("capture_id", "selection_manifest_sha256",
                "verified_maximum_source_interval_ns", "reader_method_version",
                "reader_sha256", "secondary_probe_method_version",
                "secondary_probe_sha256")
    if any(key not in value for key in required):
        raise ValueError("secondary optical classifier context is incomplete")
    if (_SHA256.fullmatch(str(value["capture_id"])) is None
            or _SHA256.fullmatch(str(value["selection_manifest_sha256"])) is None
            or type(value["verified_maximum_source_interval_ns"]) is not int
            or not 0 < value["verified_maximum_source_interval_ns"]
            <= MAXIMUM_RECORDING_SOURCE_INTERVAL_NS
            or value["reader_method_version"] != PROFILE_READER_METHOD_VERSION
            or value["reader_sha256"] != PROFILE_READER_SHA256
            or value["secondary_probe_method_version"]
            != PROFILE_SECONDARY_PROBE_METHOD_VERSION
            or value["secondary_probe_sha256"] != PROFILE_SECONDARY_PROBE_SHA256):
        raise ValueError("secondary optical context does not match the frozen reader/probe contract")
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
    if not isinstance(allowed, list) or len(allowed) != 1 or "unresolved" in spec:
        return None
    return deepcopy(allowed[0])


def _canonical_card(value):
    if (not isinstance(value, dict) or set(value) != set(_CARD_KEYS)
            or value.get("band") not in _CARD_BANDS
            or not isinstance(value.get("frequency"), str)
            or _FREQUENCY.fullmatch(value["frequency"]) is None
            or value.get("direction") not in _CARD_DIRECTIONS
            or type(value.get("bars")) is not int or not 0 <= value["bars"] <= 6):
        return None
    return {key: deepcopy(value[key]) for key in _CARD_KEYS}


def _canonical_value(value):
    if not isinstance(value, list) or not 1 <= len(value) <= 2:
        return None
    cards = [_canonical_card(card) for card in value]
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


def _direction_meter_exact(card, expected, slot):
    if (not isinstance(card, dict) or card.get("slot") != slot
            or card.get("direction") != expected["direction"]
            or card.get("bars") != expected["bars"]
            or card.get("bars_state") != "readable"
            or card.get("compatible_bars") != [expected["bars"]]
            or card.get("text_visible") is not True):
        return False
    direction = card.get("direction_reading")
    if (not isinstance(direction, dict) or direction.get("state") != "readable"
            or direction.get("value") != expected["direction"]
            or direction.get("reason") is not None
            or direction.get("shape_matches") != [expected["direction"]]):
        return False
    bars = card.get("bar_reading")
    states = _meter_states(bars)
    definite = ["on" if index < expected["bars"] else "off" for index in range(6)]
    return (isinstance(bars, dict) and bars.get("state") == "readable"
            and bars.get("value") == expected["bars"] and bars.get("reason") is None
            and states == definite and _compatible_counts(states) == [expected["bars"]]
            and bars.get("compatible_counts") == [expected["bars"]])


def _accepted_identity(card, expected):
    if (card.get("band") != expected["band"]
            or card.get("frequency") != expected["frequency"]):
        return False
    candidates = card.get("ocr_candidates")
    if not isinstance(candidates, list) or not candidates:
        return False
    normalized = []
    for candidate in candidates:
        if (not isinstance(candidate, (list, tuple)) or len(candidate) != 2
                or not all(isinstance(item, str) for item in candidate)):
            return False
        normalized.append(tuple(candidate))
    return set(normalized) == {(expected["band"], expected["frequency"])}


def _exact_card(card, expected, slot):
    return (_direction_meter_exact(card, expected, slot)
            and _accepted_identity(card, expected))


def _readable_secondary(sample):
    reading = _reading(sample)
    value = _canonical_value(reading.get("value"))
    cards = reading.get("cards")
    if (reading.get("state") != "readable" or reading.get("reason") is not None
            or value is None or not isinstance(cards, list) or len(cards) != len(value)):
        return None
    if any(not _exact_card(card, expected, slot)
           for slot, (card, expected) in enumerate(zip(cards, value))):
        return None
    return value


def _frequency_only_ocr(card):
    observation = card.get("ocr_observation") if isinstance(card, dict) else None
    rows = observation.get("rows") if isinstance(observation, dict) else None
    if (not isinstance(observation, dict) or observation.get("revision") != 3
            or not isinstance(rows, list) or len(rows) != 1):
        return None
    candidates = rows[0].get("candidates") if isinstance(rows[0], dict) else None
    if not isinstance(candidates, list) or len(candidates) != 1:
        return None
    candidate = candidates[0]
    confidence = candidate.get("confidence") if isinstance(candidate, dict) else None
    text = candidate.get("text") if isinstance(candidate, dict) else None
    if (isinstance(confidence, bool) or not isinstance(confidence, (int, float))
            or not np.isfinite(confidence) or confidence < MINIMUM_OCR_CONFIDENCE
            or not isinstance(text, str)):
        return None
    normalized = re.sub(r"\s", "", text)
    if _FREQUENCY.fullmatch(normalized) is None:
        return None
    return {"text": text, "normalized_frequency": normalized,
            "confidence": float(confidence)}


def _candidate_shape(sample):
    reading = _reading(sample)
    cards = reading.get("cards")
    if (reading.get("state") != "unreadable" or reading.get("value") is not None
            or reading.get("reason") != RAW_UNREADABLE_REASON
            or reading.get("ocr_available") is not True
            or not isinstance(cards, list) or not 1 <= len(cards) <= 2):
        return None
    deficient = []
    for slot, card in enumerate(cards):
        if not isinstance(card, dict) or card.get("slot") != slot:
            return None
        missing = card.get("band") is None or card.get("frequency") is None
        if missing:
            if (card.get("band") is not None or card.get("frequency") is not None
                    or card.get("ocr_candidates") != [] or card.get("text_visible") is not True
                    or card.get("direction") not in _CARD_DIRECTIONS
                    or card.get("bars_state") != "readable"
                    or type(card.get("bars")) is not int or not 0 <= card["bars"] <= 6):
                return None
            raw = _frequency_only_ocr(card)
            if raw is None:
                return None
            deficient.append((slot, raw))
        elif (card.get("band") not in _CARD_BANDS
              or not isinstance(card.get("frequency"), str)
              or _FREQUENCY.fullmatch(card["frequency"]) is None):
            return None
    return deficient[0] if len(deficient) == 1 else None


def _comparison_matches(sample, *, target):
    comparison = sample.get("comparison")
    checks = comparison.get("checks") if isinstance(comparison, dict) else None
    joint = comparison.get("joint_state") if isinstance(comparison, dict) else None
    if not isinstance(checks, dict) or set(checks) != set(_FIELDS) or not isinstance(joint, dict):
        return False
    statuses = {field: checks[field].get("status")
                if isinstance(checks[field], dict) else None for field in _FIELDS}
    if target:
        return (statuses["secondary"] == "UNRESOLVED"
                and all(statuses[field] == "MATCH" for field in _FIELDS if field != "secondary")
                and joint.get("status") == "MATCH")
    return all(status == "MATCH" for status in statuses.values()) and joint.get("status") == "MATCH"


def _decode_profiles(sample):
    observed = sample.get("observed")
    probe = observed.get("secondary_profiles") if isinstance(observed, dict) else None
    if (not isinstance(probe, dict)
            or set(probe) != {"schema_version", "method_version", "profile_schema", "cards"}
            or probe.get("schema_version") != 1
            or probe.get("method_version") != PROFILE_SECONDARY_PROBE_METHOD_VERSION
            or probe.get("profile_schema") != _PROFILE_SCHEMA):
        raise ValueError("secondary optical profile schema is malformed or unavailable")
    cards = probe.get("cards")
    if not isinstance(cards, list) or len(cards) != len(_TEXT_BOXES):
        raise ValueError("secondary optical profile card inventory is malformed")
    decoded = []
    hashes = []
    for slot, (card, box) in enumerate(zip(cards, _TEXT_BOXES)):
        if (not isinstance(card, dict)
                or set(card) != {"slot", "reference_bounds", "profile_b64", "profile_sha256"}
                or card.get("slot") != slot or card.get("reference_bounds") != list(box)
                or not isinstance(card.get("profile_b64"), str)
                or _SHA256.fullmatch(str(card.get("profile_sha256"))) is None):
            raise ValueError("secondary optical profile card binding is malformed")
        try:
            raw = base64.b64decode(card["profile_b64"], validate=True)
        except (ValueError, binascii.Error) as exc:
            raise ValueError("secondary optical profile is not canonical base64") from exc
        if (len(raw) != _PROFILE_BYTE_COUNT
                or base64.b64encode(raw).decode("ascii") != card["profile_b64"]
                or hashlib.sha256(raw).hexdigest() != card["profile_sha256"]):
            raise ValueError("secondary optical profile bytes do not match their binding")
        decoded.append(np.frombuffer(raw, dtype=np.uint8).astype(float))
        hashes.append(card["profile_sha256"])
    return decoded, hashes


def _profile_metrics(chain, deficient_slot):
    decoded = []
    hashes = []
    for sample in chain:
        profiles, profile_hashes = _decode_profiles(sample)
        decoded.append(profiles[deficient_slot])
        hashes.append(profile_hashes[deficient_slot])
    supports = [decoded[0], decoded[1], decoded[3], decoded[4]]
    support_pair_rms = max(float(np.sqrt(np.mean(np.square(left - right))))
                           for left, right in combinations(supports, 2))
    support_stack = np.stack(supports)
    low, high = np.min(support_stack, axis=0), np.max(support_stack, axis=0)
    support_component_span = int(np.max(high - low))
    target = decoded[2]
    target_support_rms = max(float(np.sqrt(np.mean(np.square(target - support))))
                             for support in supports)
    excursion = np.maximum.reduce((low - target, target - high, np.zeros_like(target)))
    maximum_excursion = int(np.max(excursion))
    violation_count = int(np.sum(excursion > MAXIMUM_TARGET_ENVELOPE_EXCURSION))
    metrics = {
        "maximum_support_pair_rms": round(support_pair_rms, 6),
        "maximum_support_component_span": support_component_span,
        "maximum_target_support_rms": round(target_support_rms, 6),
        "maximum_target_envelope_excursion": maximum_excursion,
        "target_envelope_violation_count": violation_count,
    }
    accepted = (support_pair_rms <= MAXIMUM_SUPPORT_PAIR_RMS
                and support_component_span <= MAXIMUM_SUPPORT_COMPONENT_SPAN
                and target_support_rms <= MAXIMUM_TARGET_SUPPORT_RMS
                and violation_count == 0)
    return metrics, hashes, accepted


def _current_presentation_established(event, selected, target):
    first_correct = event.get("first_correct")
    if not isinstance(first_correct, dict):
        return None
    established = [sample for sample in selected
                   if all(key in first_correct and sample.get(key) == first_correct[key]
                          for key in _POINT_ID_KEYS)]
    if (len(established) != 1 or not _comparison_matches(established[0], target=False)
            or established[0].get("capture_ns", target["capture_ns"] + 1) > target["capture_ns"]):
        return None
    return _point(established[0])


def _target_cards(sample, support_value, deficient_slot, raw_ocr):
    reading = _reading(sample)
    cards = reading.get("cards")
    if not isinstance(cards, list) or len(cards) != len(support_value):
        return False
    for slot, (card, expected) in enumerate(zip(cards, support_value)):
        if not _direction_meter_exact(card, expected, slot):
            return False
        if slot == deficient_slot:
            if (card.get("band") is not None or card.get("frequency") is not None
                    or card.get("ocr_candidates") != []
                    or raw_ocr["normalized_frequency"] != expected["frequency"]):
                return False
        elif not _accepted_identity(card, expected):
            return False
    projected = [{key: card.get(key) for key in _CARD_KEYS} for card in cards]
    return reading.get("partial_cards") == projected


def _record(event, selected, index, context, candidate):
    target = selected[index]
    if index < STABLE_SUPPORT_FRAMES_EACH_SIDE or index + STABLE_SUPPORT_FRAMES_EACH_SIDE >= len(selected):
        return None, "UNCLOSED_BRACKET", "secondary OCR refusal lacks two immediate readable supports on each side"
    chain = selected[index - 2:index + 3]
    maximum_gap = min(context["verified_maximum_source_interval_ns"],
                      MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS)
    if not all(_consecutive(left, right, maximum_gap) for left, right in zip(chain, chain[1:])):
        return None, "SOURCE_GAP", "secondary optical bracket crosses an unobserved source position"
    if chain[-1]["capture_ns"] - chain[0]["capture_ns"] > MAXIMUM_SUPPORT_CHAIN_SPAN_NS:
        return None, "SUPPORT_SPAN", "secondary optical bracket exceeds 25 ms"
    support_samples = chain[:2] + chain[3:]
    support_values = [_readable_secondary(sample) for sample in support_samples]
    if any(value is None for value in support_values) or any(
            value != support_values[0] for value in support_values[1:]):
        return None, "SUPPORT_VALUE", "secondary optical supports are not one exact readable card state"
    support_value = support_values[0]
    if any(not _comparison_matches(sample, target=False) for sample in support_samples):
        return None, "SUPPORT_COMPARISON", "secondary optical supports are not literal all-field current matches"
    if not _comparison_matches(target, target=True):
        return None, "PRODUCT_FIELD_SCOPE", "secondary OCR refusal is not the sole unresolved product field"
    if (_resolved_allowed(event.get("target"), "secondary") != support_value
            or any(_resolved_allowed(sample.get("expected"), "secondary") != support_value
                   for sample in chain)):
        return None, "TARGET_MISMATCH", "support-derived secondary state is not the one exact current target"
    established = _current_presentation_established(event, selected, target)
    if established is None:
        return None, "TARGET_ACQUISITION_CONTEXT", "exact current presentation was not established before the OCR refusal"
    deficient_slot, raw_ocr = candidate
    if not _target_cards(target, support_value, deficient_slot, raw_ocr):
        return None, "CARD_EVIDENCE", "target direction, meter, sibling card, or raw OCR evidence differs from its supports"
    try:
        metrics, profile_hashes, accepted = _profile_metrics(chain, deficient_slot)
    except ValueError as exc:
        return None, "OPTICAL_PROFILE", str(exc)
    if not accepted:
        return None, "OPTICAL_DIFFERENCE", "target full-text pixels leave the stable support profile bounds"

    return {
        "event_id": event["event_id"],
        "classifier_id": CLASSIFIER_ID,
        "classifier_spec_sha256": CLASSIFIER_SPEC_SHA256,
        "status": "QUALIFIED_CAPTURE_TRANSITION",
        "deadline_observation_semantics": DEADLINE_OBSERVATION_SEMANTICS,
        "raw_affected_fields": ["secondary"],
        "video_frame_indices": [target["video_frame_index"]],
        "first": _point(target),
        "last": _point(target),
        "left_support": [_point(sample) for sample in chain[:2]],
        "right_support": [_point(sample) for sample in chain[3:]],
        "current_presentation_established": established,
        "deficient_slot": deficient_slot,
        "raw_frequency_only_ocr": deepcopy(raw_ocr),
        "support_derived_secondary": deepcopy(support_value),
        "resolved_value": deepcopy(support_value),
        "profile_schema": deepcopy(_PROFILE_SCHEMA),
        "profile_reference_bounds": list(_TEXT_BOXES[deficient_slot]),
        "profile_sha256s": profile_hashes,
        "profile_metrics": metrics,
        "profile_limits": {
            "maximum_support_pair_rms": MAXIMUM_SUPPORT_PAIR_RMS,
            "maximum_support_component_span": MAXIMUM_SUPPORT_COMPONENT_SPAN,
            "maximum_target_support_rms": MAXIMUM_TARGET_SUPPORT_RMS,
            "maximum_target_envelope_excursion": MAXIMUM_TARGET_ENVELOPE_EXCURSION,
        },
        "maximum_support_chain_span_ns": MAXIMUM_SUPPORT_CHAIN_SPAN_NS,
        "maximum_support_chain_interval_ns": maximum_gap,
        "verified_maximum_source_interval_ns": context["verified_maximum_source_interval_ns"],
        "capture_id": context["capture_id"],
        "selection_manifest_sha256": context["selection_manifest_sha256"],
        "reader_method_version": context["reader_method_version"],
        "reader_sha256": context["reader_sha256"],
        "secondary_probe_method_version": context["secondary_probe_method_version"],
        "secondary_probe_sha256": context["secondary_probe_sha256"],
        "event_signature": {
            "mode": event.get("mode"),
            "changed_fields": deepcopy(event.get("changed_fields")),
            "current_secondary": deepcopy(support_value),
        },
        "basis": "One raw frequency-only OCR refusal retains exact direction and meter evidence, while its fixed full-text RGB profile remains inside a source-consecutive four-frame exact-current support envelope after current presentation was established; raw evidence remains unresolved.",
    }, None, None


def classify_secondary_optical_bridge(samples, events, context):
    """Return qualified five-frame OCR bridge records and explicit refusals."""
    result = {"schema_version": 1, "classifications": [], "rejected_runs": [], "errors": []}
    try:
        context = _context(context)
        originals = _unique_originals(samples)
        if not isinstance(events, list):
            raise ValueError("secondary optical events are not a list")
        for event in events:
            if (not isinstance(event, dict) or not isinstance(event.get("event_id"), str)
                    or type(event.get("start_ns")) is not int or type(event.get("end_ns")) is not int
                    or event["start_ns"] >= event["end_ns"]):
                raise ValueError("secondary optical event is malformed")
            selected = [sample for sample in originals
                        if type(sample.get("capture_ns")) is int
                        and event["start_ns"] <= sample["capture_ns"] < event["end_ns"]]
            for index, sample in enumerate(selected):
                candidate = _candidate_shape(sample)
                if candidate is None:
                    continue
                record, code, reason = _record(event, selected, index, context, candidate)
                if record is None:
                    result["rejected_runs"].append(_reject(event, sample, code, reason))
                else:
                    result["classifications"].append(record)
    except (KeyError, TypeError, ValueError) as exc:
        result["classifications"] = []
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    return result
