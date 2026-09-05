"""Falsifiable product judgment for externally observed V1 display events.

This layer receives already-decoded, literal frame comparisons.  It does not
read pixels, search for convenient frames, or infer a temporal classifier.  A
raw ``UNRESOLVED`` frame is excused only when the caller supplies an explicit
qualified-classification record whose classifier is listed by the selected
policy and whose source frames belong exactly to that event.  Disjoint
classifiers may jointly qualify a frame only when their accepted field claims
cover that frame's raw affected fields exactly.  At the sole deadline marker,
a qualified target-acquisition transition in any implicated field is already
enough to prove a late target.  That one-way failure evidence never excuses the
frame or contributes to a pass.

Optional ``closure_context`` retains input-selected originals after the ordinary
product window. Only an exact policy-bound secondary closure can use this tail
to bracket an unresolved verification boundary. The tail never enters ordinary
acquisition or verification observation processing.

Input event records use this compact boundary schema::

    {
      "event_id": "event-0001",
      "mode": "CHANGED" | "UNCHANGED" | "BASELINE",
      "supported": true,
      "target_basis": {"first_complete_target_input_ns": 1000000000},
      "end_ns": 1400000000,
      "selection_window": {"start_ns": 990000000, "end_ns": 1312000000},
      "required_joint_state_ids": ["image-1", "image-2"],
      "coverage": {
        "available_recorded_frames": 65,
        "selected_recorded_frames": 65,
        "read_recorded_frames": 65,
        "unrecorded_source_frames": 0,
        "maximum_source_marker_gap_ns": 5000000,
        "complete_recorded_frame_coverage": true
      },
      "observations": [{
        "frame_id": "0042", "video_frame_index": 41,
        "source_frame_seq": 106, "capture_ns": 1000000000,
        "raw_status": "CURRENT" | "PREVIOUS" |
                      "DEFINITE_OTHER" | "UNRESOLVED",
        "joint_state_id": "image-1",
        "joint_impossible": false,
        "raw_affected_fields": ["main_arrows"]
      }]
    }

All timestamps are host-monotonic capture markers.  They are not DUT receipt or
physical exposure bounds.  Callers retain their raw observations separately;
the returned ``raw_observations`` is a deep copy and no raw status is changed.
"""

from __future__ import annotations

from collections import Counter
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import re
from typing import Any


SCHEMA_VERSION = 1
DEFAULT_POLICY_ID = "v1-normal-x-k-ka-blink96-v3"
DEFAULT_POLICY_PATH = Path(__file__).with_name("visible_event_policies.json")

_EVENT_MODES = {"CHANGED", "UNCHANGED", "BASELINE"}
_RAW_STATUSES = {"CURRENT", "PREVIOUS", "DEFINITE_OTHER", "UNRESOLVED"}
_FIELDS = {
    "counter_glyph", "primary_frequency", "active_bands", "main_arrows",
    "main_bars", "secondary", "muted_badge", "joint_state",
}
_SHA256 = re.compile(r"[0-9a-f]{64}")
_DEADLINE_TRANSITION_SEMANTICS = {
    "LEGAL_PRESENTATION_TRANSITION", "TARGET_ACQUISITION_TRANSITION",
}
_VERIFICATION_CLOSURE_SEMANTICS = (
    "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY")
AUXILIARY_CLOSURE_CONTEXT_NS = 80_000_000


def _require(condition: bool, reason: str) -> None:
    if not condition:
        raise ValueError(reason)


def _integer(value: Any, name: str, minimum: int = 0) -> int:
    _require(type(value) is int and value >= minimum, f"invalid {name}")
    return value


def _strings(value: Any, name: str, *, allow_empty: bool = False) -> list[str]:
    _require(isinstance(value, list), f"invalid {name}")
    _require(allow_empty or bool(value), f"empty {name}")
    _require(all(isinstance(item, str) and bool(item.strip()) for item in value), f"invalid {name}")
    _require(len(value) == len(set(value)), f"duplicate {name}")
    return list(value)


def _canonical_sha256(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def load_policy(policy_id: str = DEFAULT_POLICY_ID, path: Path = DEFAULT_POLICY_PATH) -> dict[str, Any]:
    """Load one tracked normative policy; there is no arbitrary timing override."""
    _require(isinstance(policy_id, str) and bool(policy_id), "invalid policy id")
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError("visible-event policy file is unreadable") from exc
    _require(isinstance(document, dict) and document.get("schema_version") == 1
             and document.get("kind") == "visible_event_policies", "invalid visible-event policy document")
    policies = document.get("policies")
    _require(isinstance(policies, dict) and isinstance(policies.get(policy_id), dict),
             "unknown visible-event policy")
    policy = deepcopy(policies[policy_id])
    _require(policy.get("contract_id") == "VISIBLE_EVENT_PRESENTATION"
             and policy.get("contract_version") in {1, 2, 3}, "unsupported visible-event contract")
    contract_version = policy["contract_version"]
    appearance = _integer(policy.get("appearance_deadline_ns"), "appearance deadline", 1)
    verification = _integer(policy.get("verification_duration_ns"), "verification duration", 1)
    marker_gap = _integer(policy.get("maximum_source_marker_gap_ns"), "source-marker gap", 1)
    guard = _integer(policy.get("range_guard_ns"), "range guard", 1)
    hold = _integer(policy.get("minimum_post_completion_hold_ns"), "minimum event hold", 1)
    _require(guard == marker_gap, "range guard must equal the qualified source-marker gap")
    if contract_version == 1:
        _require(hold == appearance + verification + guard,
                 "minimum event hold disagrees with timing bounds")
    else:
        _require(policy.get("appearance_decision_rule") ==
                 "first_source_marker_at_or_after_nominal_deadline",
                 "unsupported appearance-decision rule")
        observation_bracket = _integer(
            policy.get("maximum_appearance_observation_bracket_ns"),
            "appearance observation bracket", 1)
        _require(observation_bracket <= marker_gap,
                 "appearance observation bracket exceeds the qualified source-marker gap")
        _require(hold == appearance + observation_bracket + verification + guard,
                 "minimum event hold disagrees with observable timing bounds")
    if contract_version == 3:
        _require(policy.get("pre_deadline_observations") == "diagnostic_only"
                 and policy.get("verification_anchor") ==
                     "first_source_marker_at_or_after_nominal_deadline",
                 "unsupported functional observation window")
    _require(policy.get("clock") == "host_monotonic_capture_marker"
             and policy.get("input_anchor") == "first_complete_target_input_all_accepted_ns",
             "unsupported visible-event clock or anchor")
    _require(policy.get("temporal_classification") == "explicit_qualified_map_only",
             "unsupported temporal-classification policy")
    classifiers = policy.get("qualified_temporal_classifier_ids", [])
    policy["qualified_temporal_classifier_ids"] = _strings(
        classifiers, "qualified temporal classifier ids", allow_empty=True)
    specs = policy.get("qualified_temporal_classifiers")
    _require(isinstance(specs, dict) and set(specs) == set(policy["qualified_temporal_classifier_ids"]),
             "qualified temporal classifier specs disagree with their ids")
    for classifier_id, spec in specs.items():
        _require(isinstance(spec, dict)
                 and isinstance(spec.get("classifier_spec_sha256"), str)
                 and _SHA256.fullmatch(spec["classifier_spec_sha256"]) is not None,
                 f"invalid classifier spec for {classifier_id}")
        fields = _strings(spec.get("raw_affected_fields"), f"classifier fields for {classifier_id}")
        _require(all(field in _FIELDS for field in fields), f"unknown classifier field for {classifier_id}")
        spec["raw_affected_fields"] = fields
        if contract_version >= 2:
            _require(spec.get("deadline_observation_semantics") in
                     _DEADLINE_TRANSITION_SEMANTICS,
                     f"invalid deadline transition semantics for {classifier_id}")
        closure_semantics = spec.get("verification_closure_semantics")
        if closure_semantics is not None:
            _require(closure_semantics == _VERIFICATION_CLOSURE_SEMANTICS,
                     f"invalid verification closure semantics for {classifier_id}")
        if "auxiliary_closure_context_ns" in spec:
            _require(closure_semantics == _VERIFICATION_CLOSURE_SEMANTICS
                     and fields == ["secondary"]
                     and spec["auxiliary_closure_context_ns"] == AUXILIARY_CLOSURE_CONTEXT_NS,
                     f"invalid auxiliary closure context for {classifier_id}")
    _require(isinstance(policy.get("scope"), dict) and isinstance(policy.get("basis"), list),
             "visible-event policy lacks scope or basis")
    return policy


def _contract(policy_id: str, policy: dict[str, Any]) -> dict[str, Any]:
    result = {
        "id": policy["contract_id"],
        "version": policy["contract_version"],
        "profile_id": policy_id,
        "policy_sha256": _canonical_sha256(policy),
        "clock": policy["clock"],
        "anchor": policy["input_anchor"],
        "appearance_deadline_ns": policy["appearance_deadline_ns"],
        "verification_duration_ns": policy["verification_duration_ns"],
        "maximum_source_marker_gap_ns": policy["maximum_source_marker_gap_ns"],
        "range_guard_ns": policy["range_guard_ns"],
        "minimum_post_completion_hold_ns": policy["minimum_post_completion_hold_ns"],
        "qualified_temporal_classifier_ids": list(policy["qualified_temporal_classifier_ids"]),
        "qualified_temporal_classifiers": deepcopy(policy["qualified_temporal_classifiers"]),
        "scope": deepcopy(policy["scope"]),
        "basis": deepcopy(policy["basis"]),
    }
    if policy["contract_version"] >= 2:
        result.update(
            appearance_decision_rule=policy["appearance_decision_rule"],
            maximum_appearance_observation_bracket_ns=(
                policy["maximum_appearance_observation_bracket_ns"]),
        )
    if policy["contract_version"] == 3:
        result["pre_deadline_observations"] = policy["pre_deadline_observations"]
        result["verification_anchor"] = policy["verification_anchor"]
    return result


def _base_result(policy_id: str, policy: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "visible_event_presentation",
        "contract": _contract(policy_id, policy),
        "execution": {"status": "COMPLETE", "fatal_integrity_errors": [],
                      "temporal_classification_errors": []},
        "submitted_temporal_classifications": [],
        "events": [],
        "counts": {"required_events": 0, "passed": 0, "failed": 0, "inconclusive": 0},
        "result": "INCONCLUSIVE",
        "reason_code": "NO_REQUIRED_EVENTS",
    }


def _inconclusive_event(raw: Any, event_id: str, code: str, reason: str) -> dict[str, Any]:
    observations = raw.get("observations", []) if isinstance(raw, dict) else []
    return {
        "event_id": event_id,
        "mode": raw.get("mode") if isinstance(raw, dict) else None,
        "raw_observations": deepcopy(observations) if isinstance(observations, list) else [],
        "derived_observations": [],
        "temporal_classifications": [],
        "result": "INCONCLUSIVE",
        "reason_code": code,
        "reasons": [code],
        "reason": reason,
    }


def _classification_shape(record: Any) -> str | None:
    if not isinstance(record, dict):
        return "classification record is not an object"
    if not isinstance(record.get("event_id"), str) or not record["event_id"]:
        return "classification has no event id"
    if not isinstance(record.get("classifier_id"), str) or not record["classifier_id"]:
        return "classification has no classifier id"
    if not isinstance(record.get("classifier_spec_sha256"), str) \
            or _SHA256.fullmatch(record["classifier_spec_sha256"]) is None:
        return "classification has no valid classifier spec hash"
    if record.get("status") != "QUALIFIED_CAPTURE_TRANSITION":
        return "classification status is not QUALIFIED_CAPTURE_TRANSITION"
    indices = record.get("video_frame_indices")
    if not isinstance(indices, list) or not indices or any(type(value) is not int or value < 0 for value in indices):
        return "classification has invalid video frame indices"
    if indices != sorted(set(indices)) or any(right != left + 1 for left, right in zip(indices, indices[1:])):
        return "classification video frame indices are not one contiguous unique run"
    fields = record.get("raw_affected_fields")
    if not isinstance(fields, list) or not fields or any(field not in _FIELDS for field in fields):
        return "classification has invalid raw affected fields"
    if len(fields) != len(set(fields)):
        return "classification has duplicate raw affected fields"
    closure_semantics = record.get("verification_closure_semantics")
    if closure_semantics is not None:
        if closure_semantics != _VERIFICATION_CLOSURE_SEMANTICS:
            return "classification has invalid verification closure semantics"
        context = record.get("context_frame_indices")
        if (not isinstance(context, list) or not context
                or any(type(value) is not int or value < 0 for value in context)
                or context != sorted(set(context))
                or any(right != left + 1 for left, right in zip(context, context[1:]))
                or not set(indices).issubset(context)):
            return "classification has invalid verification closure context"
    return None


def _classification_map(records: Any, events: list[Any],
                        allowed: dict[str, dict]) -> tuple[
                            dict[tuple[str, int], list[dict]],
                            dict[tuple[str, int], list[dict]],
                            list[dict],
                        ]:
    """Validate explicit qualifications without deriving any classification."""
    errors: list[dict] = []
    qualified: dict[tuple[str, int], list[dict]] = {}
    if records is None:
        records = []
    if not isinstance(records, list):
        return {}, {}, [{"code": "INVALID_TEMPORAL_CLASSIFICATION_MAP",
                         "reason": "temporal classifications must be a list"}]
    event_observations: dict[str, dict[int, dict]] = {}
    for event in events:
        if not isinstance(event, dict) or not isinstance(event.get("event_id"), str):
            continue
        observations = event.get("observations")
        if not isinstance(observations, list):
            continue
        auxiliary = event.get("closure_context", {})
        if isinstance(auxiliary, dict) and isinstance(auxiliary.get("observations", []), list):
            observations = [*observations, *auxiliary.get("observations", [])]
        by_index = {item.get("video_frame_index"): item for item in observations
                    if isinstance(item, dict) and type(item.get("video_frame_index")) is int}
        event_observations[event["event_id"]] = by_index
    claims: dict[tuple[str, int, str], list[int]] = {}
    candidates: list[tuple[int, dict]] = []
    for record_index, record in enumerate(records):
        reason = _classification_shape(record)
        if reason:
            errors.append({"code": "INVALID_TEMPORAL_CLASSIFICATION", "record_index": record_index,
                           "reason": reason})
            continue
        if record["classifier_id"] not in allowed:
            errors.append({"code": "CLASSIFIER_NOT_POLICY_LISTED", "record_index": record_index,
                           "event_id": record["event_id"], "classifier_id": record["classifier_id"]})
            continue
        spec = allowed[record["classifier_id"]]
        if record.get("classifier_spec_sha256") != spec["classifier_spec_sha256"] \
                or set(record["raw_affected_fields"]) != set(spec["raw_affected_fields"]) \
                or ("deadline_observation_semantics" in record
                    and record["deadline_observation_semantics"] !=
                    spec.get("deadline_observation_semantics")) \
                or (("verification_closure_semantics" in record
                     or "verification_closure_semantics" in spec)
                    and record.get("verification_closure_semantics") !=
                    spec.get("verification_closure_semantics")) \
                or record.get("auxiliary_closure_context_ns") != spec.get(
                    "auxiliary_closure_context_ns"):
            errors.append({"code": "TEMPORAL_CLASSIFIER_SPEC_MISMATCH", "record_index": record_index,
                           "event_id": record["event_id"], "classifier_id": record["classifier_id"]})
            continue
        members = event_observations.get(record["event_id"])
        if members is None or any(index not in members for index in record["video_frame_indices"]):
            errors.append({"code": "TEMPORAL_CLASSIFICATION_EVENT_MEMBERSHIP", "record_index": record_index,
                           "event_id": record["event_id"], "reason": "source frame is not an exact event member"})
            continue
        selected = [members[index] for index in record["video_frame_indices"]]
        affected = [item.get("raw_affected_fields") for item in selected]
        claimed_fields = set(record["raw_affected_fields"])
        if any(item.get("raw_status") != "UNRESOLVED" for item in selected) \
                or not all(isinstance(fields, list) for fields in affected) \
                or any(not claimed_fields.issubset(set(fields)) for fields in affected):
            errors.append({"code": "TEMPORAL_CLASSIFICATION_RAW_MISMATCH", "record_index": record_index,
                           "event_id": record["event_id"],
                           "reason": "qualification claims fields absent from a raw unresolved frame"})
            continue
        candidates.append((record_index, record))
        for index in record["video_frame_indices"]:
            for field in record["raw_affected_fields"]:
                claims.setdefault((record["event_id"], index, field), []).append(record_index)
    duplicate_claims = {key for key, owners in claims.items() if len(owners) > 1}
    rejected_records = {record_index for key in duplicate_claims for record_index in claims[key]}
    accepted_by_frame: dict[tuple[str, int], list[dict]] = {}
    for record_index, record in candidates:
        if record_index in rejected_records:
            errors.append({"code": "DUPLICATE_TEMPORAL_CLASSIFICATION_FIELD_CLAIM",
                           "record_index": record_index,
                           "event_id": record["event_id"]})
            continue
        for index in record["video_frame_indices"]:
            accepted_by_frame.setdefault((record["event_id"], index), []).append(record)
    target_acquisition: dict[tuple[str, int], list[dict]] = {}
    for key, accepted in accepted_by_frame.items():
        raw_fields = set(event_observations[key[0]][key[1]].get("raw_affected_fields", []))
        accepted_fields = {field for record in accepted for field in record["raw_affected_fields"]}
        if accepted_fields == raw_fields:
            qualified[key] = deepcopy(accepted)
        target_records = [record for record in accepted
                          if allowed[record["classifier_id"]].get(
                              "deadline_observation_semantics") ==
                          "TARGET_ACQUISITION_TRANSITION"]
        if target_records:
            target_acquisition[key] = deepcopy(target_records)
    return qualified, target_acquisition, errors


def _append_once(values: list[str], value: str) -> None:
    if value not in values:
        values.append(value)


def _verification_closure_at(
    position: int,
    observations: list[dict],
    event_id: str,
    verification_end_ns: int,
    qualified: dict[tuple[str, int], list[dict]],
    policy: dict[str, Any],
    selection_end_ns: int,
) -> dict[str, Any] | None:
    """Prove one unresolved verification boundary from a closed raw-current bracket."""
    boundary = observations[position]
    if (boundary["raw_status"] != "UNRESOLVED"
            or not verification_end_ns <= boundary["capture_ns"]
            <= verification_end_ns + policy["maximum_source_marker_gap_ns"]):
        return None

    def closure_record(at: int) -> dict | None:
        item = observations[at]
        records = qualified.get((event_id, item["video_frame_index"]), [])
        if len(records) != 1:
            return None
        record = records[0]
        spec = policy["qualified_temporal_classifiers"].get(record["classifier_id"], {})
        if (record.get("verification_closure_semantics") !=
                _VERIFICATION_CLOSURE_SEMANTICS
                or spec.get("verification_closure_semantics") !=
                _VERIFICATION_CLOSURE_SEMANTICS):
            return None
        return record

    record = closure_record(position)
    if record is None:
        return None
    start = position
    while start > 0 and observations[start - 1]["raw_status"] == "UNRESOLVED" \
            and closure_record(start - 1) is not None:
        start -= 1
    stop = position + 1
    while stop < len(observations) and observations[stop]["raw_status"] == "UNRESOLVED" \
            and closure_record(stop) is not None:
        stop += 1
    if start == 0 or stop >= len(observations):
        return None
    left, right = observations[start - 1], observations[stop]
    if right["capture_ns"] >= selection_end_ns:
        spec = policy["qualified_temporal_classifiers"].get(record["classifier_id"], {})
        if (record.get("auxiliary_closure_context_ns") != AUXILIARY_CLOSURE_CONTEXT_NS
                or spec.get("auxiliary_closure_context_ns") != AUXILIARY_CLOSURE_CONTEXT_NS
                or right["capture_ns"] >= selection_end_ns + AUXILIARY_CLOSURE_CONTEXT_NS
                or right["capture_ns"] - left["capture_ns"] > AUXILIARY_CLOSURE_CONTEXT_NS):
            return None
    if (left["raw_status"] != "CURRENT" or right["raw_status"] != "CURRENT"
            or not left["capture_ns"] < verification_end_ns
            or any(right_item["video_frame_index"] != left_item["video_frame_index"] + 1
                   or right_item["source_frame_seq"] != left_item["source_frame_seq"] + 1
                   or right_item["capture_ns"] - left_item["capture_ns"]
                   > policy["maximum_source_marker_gap_ns"]
                   for left_item, right_item in zip(
                       observations[start - 1:stop], observations[start:stop + 1]))):
        return None
    run_indices = [item["video_frame_index"] for item in observations[start:stop]]
    records = [closure_record(at) for at in range(start, stop)]
    if (any(item is None for item in records)
            or any(item != record for item in records)
            or record.get("video_frame_indices") != run_indices):
        return None
    bracket_indices = [left["video_frame_index"], *run_indices,
                       right["video_frame_index"]]
    retained_context = [
        *(point.get("video_frame_index") for point in record.get("left_support", [])
          if isinstance(point, dict)),
        *record.get("context_frame_indices", []),
        *(point.get("video_frame_index") for point in record.get("right_support", [])
          if isinstance(point, dict)),
    ]
    if not set(bracket_indices).issubset(retained_context):
        return None
    for key, expected in (("first", observations[start]),
                          ("last", observations[stop - 1])):
        point = record.get(key)
        if not isinstance(point, dict) or any(
                point.get(field) != expected.get(field) for field in (
                    "frame_id", "video_frame_index", "source_frame_seq", "capture_ns")):
            return None
    return {
        "semantics": _VERIFICATION_CLOSURE_SEMANTICS,
        "verification_boundary": deepcopy(boundary),
        "left_raw_current": deepcopy(left),
        "right_raw_current": deepcopy(right),
        "classified_video_frame_indices": run_indices,
        "classifier_id": record["classifier_id"],
        "classifier_spec_sha256": record["classifier_spec_sha256"],
    }


def _event_evidence(raw: dict[str, Any], policy: dict[str, Any]) -> tuple[dict[str, Any], list[dict], list[dict], list[str]]:
    """Validate one event and return normalized timing, observations and blockers."""
    event_id = raw.get("event_id")
    _require(isinstance(event_id, str) and bool(event_id), "event has no stable id")
    _require(raw.get("mode") in _EVENT_MODES, "event has invalid mode")
    _require(type(raw.get("supported")) is bool, "event support decision is missing")
    basis = raw.get("target_basis")
    _require(isinstance(basis, dict), "event target basis is missing")
    anchor = _integer(basis.get("first_complete_target_input_ns"), "target completion timestamp")
    end = _integer(raw.get("end_ns"), "event end timestamp")
    _require(end > anchor, "event ends before target completion")
    _require(isinstance(raw.get("end_reason"), str) and bool(raw["end_reason"]),
             "event end reason is missing")
    gap = policy["maximum_source_marker_gap_ns"]
    selection_start = anchor - gap
    selection_end = anchor + policy["minimum_post_completion_hold_ns"]
    declared = raw.get("selection_window")
    _require(isinstance(declared, dict)
             and declared.get("start_ns") == selection_start
             and declared.get("end_ns") == selection_end,
             "event selection window disagrees with the frozen policy")
    phases = _strings(raw.get("required_joint_state_ids"), "required joint-state ids")
    observations = raw.get("observations")
    _require(isinstance(observations, list) and bool(observations), "event has no original observations")
    closure_context = raw.get("closure_context", {})
    _require(isinstance(closure_context, dict), "invalid auxiliary closure context")
    auxiliary = closure_context.get("observations", [])
    _require(isinstance(auxiliary, list), "invalid auxiliary closure observations")
    closure_end = max(selection_end, min(selection_end + AUXILIARY_CLOSURE_CONTEXT_NS, end))
    if closure_context:
        _require(closure_context.get("selection_window") == {
                    "start_ns": selection_end, "end_ns": closure_end},
                 "auxiliary closure context disagrees with fixed bound or next input")
    normalized: list[dict] = []
    prior_capture = None
    prior_video = None
    prior_source = None
    for position, item in enumerate([*observations, *auxiliary]):
        _require(isinstance(item, dict), "event observation is not an object")
        point = deepcopy(item)
        _require(isinstance(point.get("frame_id"), str) and bool(point["frame_id"]),
                 "observation has no frame id")
        video = _integer(point.get("video_frame_index"), "video frame index")
        source = _integer(point.get("source_frame_seq"), "source frame sequence", 1)
        capture = _integer(point.get("capture_ns"), "capture timestamp")
        lo, hi = ((selection_start, selection_end) if position < len(observations)
                  else (selection_end, closure_end))
        _require(lo <= capture < hi, "observation lies outside frozen selection window")
        _require(capture < end, "observation occurs after the event was superseded")
        _require(point.get("raw_status") in _RAW_STATUSES, "observation has invalid raw status")
        if point["raw_status"] == "CURRENT":
            _require(point.get("joint_state_id") in phases, "current observation lacks a legal joint-state id")
        if "joint_impossible" in point:
            _require(type(point["joint_impossible"]) is bool, "invalid impossible-joint flag")
        fields = point.get("raw_affected_fields", [])
        _require(isinstance(fields, list) and all(field in _FIELDS for field in fields)
                 and len(fields) == len(set(fields)), "invalid observation affected fields")
        if prior_capture is not None:
            _require(capture > prior_capture and video > prior_video and source > prior_source,
                     "event observations are not strictly ordered originals")
        normalized.append(point)
        prior_capture, prior_video, prior_source = capture, video, source
    auxiliary = normalized[len(observations):]
    normalized = normalized[:len(observations)]
    coverage = raw.get("coverage")
    _require(isinstance(coverage, dict), "event coverage is missing")
    for key in ("available_recorded_frames", "selected_recorded_frames", "read_recorded_frames",
                "unrecorded_source_frames", "maximum_source_marker_gap_ns"):
        _integer(coverage.get(key), f"coverage {key}")
    _require(type(coverage.get("complete_recorded_frame_coverage")) is bool,
             "coverage completeness is missing")
    _require(len({item["frame_id"] for item in normalized}) == len(normalized),
             "duplicate event frame id")
    timing = {
        "selection_start_ns": selection_start,
        "anchor_ns": anchor,
        "deadline_ns": anchor + policy["appearance_deadline_ns"],
        "worst_case_verification_end_ns": anchor + policy["appearance_deadline_ns"]
                                                + policy.get(
                                                    "maximum_appearance_observation_bracket_ns", 0)
                                                + policy["verification_duration_ns"],
        "selection_end_ns": selection_end,
        "actual_event_end_ns": end,
        "end_reason": raw["end_reason"],
    }
    blockers: list[str] = []
    if not raw["supported"]:
        _append_once(blockers, "UNSUPPORTED_TARGET")
    if end < selection_end:
        _append_once(blockers, "EPISODE_CLIPPED")
    count = len(normalized)
    if not coverage["complete_recorded_frame_coverage"] \
            or coverage["available_recorded_frames"] != count \
            or coverage["selected_recorded_frames"] != count \
            or coverage["read_recorded_frames"] != count \
            or coverage["unrecorded_source_frames"]:
        _append_once(blockers, "INCOMPLETE_SOURCE_COVERAGE")
    actual_gaps = [right["capture_ns"] - left["capture_ns"] for left, right in zip(normalized, normalized[1:])]
    actual_max = max(actual_gaps, default=0)
    if coverage["maximum_source_marker_gap_ns"] > gap or actual_max > gap \
            or any(right["video_frame_index"] != left["video_frame_index"] + 1
                   or right["source_frame_seq"] != left["source_frame_seq"] + 1
                   for left, right in zip(normalized, normalized[1:])):
        _append_once(blockers, "SOURCE_MARKER_GAP")
    if normalized[0]["capture_ns"] - selection_start > gap \
            or selection_end - normalized[-1]["capture_ns"] > gap:
        _append_once(blockers, "SOURCE_BOUNDARY_GAP")
    return timing, normalized, auxiliary, blockers


def _judge_event(raw: dict[str, Any], policy: dict[str, Any],
                 qualified: dict[tuple[str, int], list[dict]],
                 target_acquisition: dict[tuple[str, int], list[dict]]) -> dict[str, Any]:
    event_id = raw.get("event_id") if isinstance(raw.get("event_id"), str) else "invalid-event"
    try:
        timing, observations, auxiliary, blockers = _event_evidence(raw, policy)
    except (KeyError, TypeError, ValueError) as exc:
        return _inconclusive_event(raw, event_id, "INVALID_EVENT_EVIDENCE", str(exc))
    result = {
        "event_id": event_id,
        "mode": raw["mode"],
        "target_basis": deepcopy(raw["target_basis"]),
        "window": timing,
        "closure_context": deepcopy(raw.get("closure_context", {})),
        "coverage": deepcopy(raw["coverage"]),
        "required_joint_state_ids": list(raw["required_joint_state_ids"]),
        "observed_joint_state_ids": [],
        "first_current_correct": None,
        "response_acquisition": None,
        "verification_end_ns": None,
        "verification_closure_proof": None,
        "closing_current_correct": None,
        "raw_status_counts": dict(Counter(item["raw_status"] for item in observations)),
        "raw_observations": deepcopy(observations),
        "derived_observations": [],
        "temporal_classifications": [],
        "result": "INCONCLUSIVE",
        "reason_code": "ANALYSIS_INCOMPLETE",
        "reasons": [],
    }
    if "UNSUPPORTED_TARGET" in blockers:
        result.update(result="INCONCLUSIVE", reason_code="UNSUPPORTED_TARGET", reasons=blockers)
        return result
    anchor, deadline = timing["anchor_ns"], timing["deadline_ns"]
    observable_contract = policy["contract_version"] >= 2
    functional_contract = policy["contract_version"] == 3
    deadline_index = next((index for index, item in enumerate(observations)
                           if item["capture_ns"] >= deadline), None)
    deadline_marker = observations[deadline_index] if deadline_index is not None else None
    preceding_deadline_marker = (
        observations[deadline_index - 1]
        if deadline_index is not None and deadline_index > 0 else None)
    deadline_bracket_ns = (
        deadline_marker["capture_ns"] - preceding_deadline_marker["capture_ns"]
        if deadline_marker is not None and preceding_deadline_marker is not None else None)
    deadline_bracket_valid = (
        observable_contract
        and deadline_marker is not None
        and preceding_deadline_marker is not None
        and preceding_deadline_marker["capture_ns"] < deadline
        and deadline_bracket_ns <= policy["maximum_appearance_observation_bracket_ns"])
    if observable_contract:
        def marker_summary(item: dict | None) -> dict | None:
            if item is None:
                return None
            return {key: deepcopy(item[key]) for key in (
                "frame_id", "video_frame_index", "source_frame_seq", "capture_ns", "raw_status")}

        result["response_acquisition"] = {
            "decision_rule": policy["appearance_decision_rule"],
            "status": "PENDING",
            "nominal_deadline_ns": deadline,
            "maximum_capture_marker_bracket_ns": (
                policy["maximum_appearance_observation_bracket_ns"]),
            "deadline_capture_marker_bracket": {
                "start": marker_summary(preceding_deadline_marker),
                "end": marker_summary(deadline_marker),
                "width_ns": deadline_bracket_ns,
                "within_limit": deadline_bracket_valid,
            },
            "first_current_capture_ns": None,
            "first_current_offset_from_nominal_deadline_ns": None,
        }
    before = [item for item in observations if item["capture_ns"] < anchor]
    if not functional_contract and raw["mode"] in {"CHANGED", "UNCHANGED"}:
        preceding = before[-1] if before else None
        expected = "PREVIOUS" if raw["mode"] == "CHANGED" else "CURRENT"
        if raw["mode"] == "CHANGED" and any(item["raw_status"] == "CURRENT" for item in before):
            _append_once(blockers, "TARGET_PREEXISTED")
        if preceding is None or anchor - preceding["capture_ns"] > policy["maximum_source_marker_gap_ns"] \
                or preceding["raw_status"] != expected:
            _append_once(blockers, "NO_QUALIFIED_PRECEDING_STATE")

    if functional_contract:
        if deadline_marker is not None:
            result["verification_end_ns"] = deadline_marker["capture_ns"] + policy["verification_duration_ns"]
        pre_deadline = [item for item in observations if anchor <= item["capture_ns"] < deadline]
        result["acquisition_observations"] = {
            "meaning": "Diagnostic optical observations before the functional deadline; no atomic redraw or firmware-cause claim.",
            "raw_status_counts": dict(Counter(item["raw_status"] for item in pre_deadline)),
            "first_current_correct": deepcopy(next((item for item in pre_deadline
                                                    if item["raw_status"] == "CURRENT"), None)),
        }
    state = "WAITING"
    first: dict | None = None
    closing: dict | None = None
    seen_phases: list[str] = []
    failure: tuple[str, dict] | None = None
    unknown_before_deadline = False
    acquisition_accepted = False
    acquisition_at_observation_marker = False
    for observation_position, item in enumerate(observations):
        capture = item["capture_ns"]
        if capture < anchor:
            continue
        raw_status = item["raw_status"]
        explicit = qualified.get((event_id, item["video_frame_index"]))
        target_evidence = target_acquisition.get((event_id, item["video_frame_index"]))
        legal_explicit = (explicit if explicit and all(
            policy["qualified_temporal_classifiers"][record["classifier_id"]].get(
                "deadline_observation_semantics",
                "LEGAL_PRESENTATION_TRANSITION" if not observable_contract else None)
            == "LEGAL_PRESENTATION_TRANSITION" for record in explicit) else None)
        derived = ("QUALIFIED_CAPTURE_TRANSITION"
                   if raw_status == "UNRESOLVED" and legal_explicit else raw_status)
        result["derived_observations"].append({
            "frame_id": item["frame_id"], "video_frame_index": item["video_frame_index"],
            "source_frame_seq": item["source_frame_seq"], "capture_ns": capture,
            "raw_status": raw_status, "derived_status": derived,
        })
        for classification in legal_explicit or []:
            if classification not in result["temporal_classifications"]:
                result["temporal_classifications"].append(deepcopy(classification))
        if observable_contract and item is deadline_marker and raw_status == "UNRESOLVED" \
                and target_evidence:
            for classification in target_evidence:
                if classification not in result["temporal_classifications"]:
                    result["temporal_classifications"].append(deepcopy(classification))
            result["response_acquisition"]["status"] = (
                "FIRST_POST_DEADLINE_CAPTURE_TARGET_IN_TRANSITION")
            failure = ("TARGET_IN_TRANSITION_AT_DEADLINE_CAPTURE", item)
            break
        if functional_contract and capture < deadline:
            # Keep every raw image and derived reading. A physical redraw before
            # its allowed response deadline is not a functional failure.
            continue
        if (functional_contract and item is deadline_marker and raw_status == "UNRESOLVED"
                and legal_explicit):
            # Both optical endpoint phases are already legal target content.
            # This starts verification without inventing a raw CURRENT image.
            first = item
            state = "VERIFYING"
            result["verification_end_ns"] = capture + policy["verification_duration_ns"]
            result["response_acquisition"]["qualified_deadline_transition"] = deepcopy(item)
            acquisition_accepted = deadline_bracket_valid
            acquisition_at_observation_marker = True
            result["response_acquisition"]["status"] = "QUALIFIED_LEGAL_PHASE_AT_DEADLINE_CAPTURE"
            continue
        after_verification = (
            functional_contract and result["verification_end_ns"] is not None
            and capture >= result["verification_end_ns"]
        ) or (not functional_contract and first is not None
              and capture > first["capture_ns"] + policy["verification_duration_ns"])
        if after_verification:
            if closing is not None:
                continue
            if functional_contract and raw_status == "UNRESOLVED" and legal_explicit:
                closure_proof = _verification_closure_at(
                    observation_position, [*observations, *auxiliary], event_id,
                    result["verification_end_ns"], qualified, policy,
                    timing["selection_end_ns"])
                if closure_proof is not None:
                    result["verification_closure_proof"] = closure_proof
                    closing = closure_proof["right_raw_current"]
                    if closing["joint_state_id"] not in seen_phases:
                        seen_phases.append(closing["joint_state_id"])
                    continue
            if (functional_contract and capture - result["verification_end_ns"]
                    > policy["maximum_source_marker_gap_ns"]):
                _append_once(blockers, "VERIFICATION_DURATION_NOT_OBSERVED")
                continue
            if raw_status == "DEFINITE_OTHER":
                code = "IMPOSSIBLE_JOINT_STATE" if item.get("joint_impossible") else "UNEXPECTED_VISIBLE_STATE"
                failure = (code, item)
                break
            if raw_status == "PREVIOUS":
                failure = ("REGRESSION_AFTER_CURRENT", item)
                break
            if raw_status == "CURRENT":
                closing = item
                if item["joint_state_id"] not in seen_phases:
                    seen_phases.append(item["joint_state_id"])
            elif raw_status == "UNRESOLVED" and legal_explicit is None:
                _append_once(blockers, "UNCLASSIFIED_VISIBLE_INTERVAL")
            continue
        if raw_status == "DEFINITE_OTHER":
            baseline_waiting_allowance = (
                raw["mode"] == "BASELINE" and state == "WAITING"
                and ((not observable_contract and capture <= deadline) or (
                    observable_contract and deadline_marker is not None
                    and capture < deadline_marker["capture_ns"])))
            if baseline_waiting_allowance:
                # The first event has no qualified prior target. A readable
                # non-current image before the sole deadline observation proves
                # only that the new target has not yet been observed.
                continue
            if observable_contract and item is deadline_marker:
                result["response_acquisition"]["status"] = (
                    "FIRST_POST_DEADLINE_CAPTURE_NOT_CURRENT")
            code = "IMPOSSIBLE_JOINT_STATE" if item.get("joint_impossible") else "UNEXPECTED_VISIBLE_STATE"
            failure = (code, item)
            break
        if raw_status == "PREVIOUS":
            if state == "VERIFYING":
                failure = ("REGRESSION_AFTER_CURRENT", item)
                break
            if raw["mode"] == "BASELINE" and observable_contract \
                    and deadline_marker is not None and capture < deadline_marker["capture_ns"]:
                continue
            if capture > deadline or (observable_contract and capture == deadline):
                if observable_contract and item is deadline_marker:
                    result["response_acquisition"]["status"] = (
                        "FIRST_POST_DEADLINE_CAPTURE_NOT_CURRENT")
                failure = ("PREVIOUS_STATE_AFTER_DEADLINE", item)
                break
        elif raw_status == "CURRENT":
            if functional_contract and first is not None and result["first_current_correct"] is None:
                result["first_current_correct"] = deepcopy(item)
                result["response_acquisition"]["first_current_capture_ns"] = capture
                result["response_acquisition"]["first_current_offset_from_nominal_deadline_ns"] = capture - deadline
            if item["joint_state_id"] not in seen_phases:
                seen_phases.append(item["joint_state_id"])
            if state == "WAITING":
                first = item
                result["first_current_correct"] = deepcopy(item)
                if not functional_contract:
                    result["verification_end_ns"] = capture + policy["verification_duration_ns"]
                if observable_contract:
                    result["response_acquisition"]["first_current_capture_ns"] = capture
                    result["response_acquisition"][
                        "first_current_offset_from_nominal_deadline_ns"] = capture - deadline
                    if capture <= deadline:
                        acquisition_accepted = True
                        result["response_acquisition"]["status"] = (
                            "CURRENT_ON_OR_BEFORE_NOMINAL_DEADLINE")
                    elif item is deadline_marker and deadline_bracket_valid:
                        acquisition_accepted = True
                        acquisition_at_observation_marker = True
                        result["response_acquisition"]["status"] = (
                            "CURRENT_AT_FIRST_POST_DEADLINE_CAPTURE")
                    elif item is deadline_marker:
                        _append_once(blockers, "DEADLINE_OBSERVATION_BRACKET_NOT_ESTABLISHED")
                        result["response_acquisition"]["status"] = (
                            "DEADLINE_CAPTURE_BRACKET_NOT_ESTABLISHED")
                    else:
                        _append_once(blockers, "DEADLINE_OBSERVATION_UNRESOLVED")
                        result["response_acquisition"]["status"] = (
                            "CURRENT_ONLY_AFTER_FIRST_POST_DEADLINE_CAPTURE")
                    state = "VERIFYING"
                elif capture > deadline:
                    if unknown_before_deadline or any(code in blockers for code in (
                            "INCOMPLETE_SOURCE_COVERAGE", "SOURCE_MARKER_GAP", "SOURCE_BOUNDARY_GAP")):
                        if "DEADLINE_NOT_ESTABLISHED" not in blockers:
                            blockers.insert(0, "DEADLINE_NOT_ESTABLISHED")
                    else:
                        failure = ("TARGET_LATE", item)
                    break
                else:
                    acquisition_accepted = True
                    state = "VERIFYING"
        elif raw_status == "UNRESOLVED":
            if raw["mode"] == "BASELINE" and state == "WAITING" \
                    and observable_contract and deadline_marker is not None \
                    and capture < deadline_marker["capture_ns"]:
                continue
            if observable_contract and state == "WAITING" and item is deadline_marker:
                explicit_semantics = {
                    policy["qualified_temporal_classifiers"][record["classifier_id"]][
                        "deadline_observation_semantics"]
                    for record in explicit or []
                }
                if "TARGET_ACQUISITION_TRANSITION" in explicit_semantics:
                    result["response_acquisition"]["status"] = (
                        "FIRST_POST_DEADLINE_CAPTURE_TARGET_IN_TRANSITION")
                    failure = ("TARGET_IN_TRANSITION_AT_DEADLINE_CAPTURE", item)
                    break
                _append_once(blockers, "DEADLINE_OBSERVATION_UNRESOLVED")
                result["response_acquisition"]["status"] = (
                    "FIRST_POST_DEADLINE_CAPTURE_UNRESOLVED")
            if legal_explicit is None:
                _append_once(blockers, "UNCLASSIFIED_VISIBLE_INTERVAL")
                if capture <= deadline:
                    unknown_before_deadline = True

    if observable_contract and deadline_marker is None:
        _append_once(blockers, "DEADLINE_OBSERVATION_MISSING")
        result["response_acquisition"]["status"] = "NO_SOURCE_MARKER_AT_OR_AFTER_DEADLINE"
    elif observable_contract and not deadline_bracket_valid and not acquisition_accepted:
        _append_once(blockers, "DEADLINE_OBSERVATION_BRACKET_NOT_ESTABLISHED")
        if result["response_acquisition"]["status"] == "PENDING":
            result["response_acquisition"]["status"] = "DEADLINE_CAPTURE_BRACKET_NOT_ESTABLISHED"

    if first is not None and closing is None:
        verify_end = (result["verification_end_ns"] if functional_contract else
                      first["capture_ns"] + policy["verification_duration_ns"])
        closing = next((item for item in observations
                        if item["capture_ns"] >= verify_end and item["raw_status"] == "CURRENT"), None)
        if closing is not None and closing["joint_state_id"] not in seen_phases:
            seen_phases.append(closing["joint_state_id"])
    result["observed_joint_state_ids"] = seen_phases
    result["closing_current_correct"] = deepcopy(closing)

    if failure is not None:
        code, point = failure
        result.update(result="FAIL", reason_code=code, reasons=[code], first_decisive_marker=deepcopy(point))
        return result
    if first is None:
        if blockers:
            result.update(result="INCONCLUSIVE", reason_code=blockers[0], reasons=blockers)
        else:
            result.update(result="FAIL", reason_code="TARGET_NOT_OBSERVED_BY_DEADLINE",
                          reasons=["TARGET_NOT_OBSERVED_BY_DEADLINE"])
        return result
    if observable_contract and not acquisition_accepted:
        result.update(result="INCONCLUSIVE",
                      reason_code=blockers[0] if blockers else "DEADLINE_OBSERVATION_UNRESOLVED",
                      reasons=blockers or ["DEADLINE_OBSERVATION_UNRESOLVED"])
        return result
    if not observable_contract and first["capture_ns"] > deadline:
        result.update(result="INCONCLUSIVE", reason_code=blockers[0] if blockers else "DEADLINE_NOT_ESTABLISHED",
                      reasons=blockers or ["DEADLINE_NOT_ESTABLISHED"])
        return result
    if (result["verification_closure_proof"] is None
            and (closing is None or closing["capture_ns"] - result["verification_end_ns"]
                 > policy["maximum_source_marker_gap_ns"])):
        _append_once(blockers, "VERIFICATION_DURATION_NOT_OBSERVED")
    missing_phases = [phase for phase in raw["required_joint_state_ids"] if phase not in seen_phases]
    if missing_phases:
        _append_once(blockers, "LEGAL_BLINK_PHASE_NOT_OBSERVED")
    if blockers:
        result.update(result="INCONCLUSIVE", reason_code=blockers[0], reasons=blockers)
    else:
        pass_reason = ("CURRENT_AT_FIRST_POST_DEADLINE_CAPTURE_AND_VERIFIED"
                       if acquisition_at_observation_marker
                       else "PRESENTED_BY_DEADLINE_AND_VERIFIED")
        if functional_contract and result["response_acquisition"].get("qualified_deadline_transition"):
            pass_reason = "LEGAL_PHASE_AT_DEADLINE_AND_VERIFIED"
        result.update(result="PASS", reason_code=pass_reason, reasons=[pass_reason])
    return result


def judge_visible_event_presentation(
    events: list[dict[str, Any]],
    *,
    temporal_classifications: list[dict[str, Any]] | None = None,
    fatal_integrity_errors: list[str] | None = None,
    policy_id: str = DEFAULT_POLICY_ID,
    policy_path: Path = DEFAULT_POLICY_PATH,
) -> dict[str, Any]:
    """Evaluate the named product contract without modifying any input object.

    A fatal identity/hash/timing/configuration/qualification error prevents every
    product claim.  With trusted evidence, a supported definite failure wins over
    local gaps or unknowns; otherwise every required event must pass.
    """
    try:
        policy = load_policy(policy_id, policy_path)
    except (TypeError, ValueError) as exc:
        # Keep a stable schema even when the normative policy itself is unavailable.
        fallback = {
            "contract_id": "VISIBLE_EVENT_PRESENTATION", "contract_version": 2,
            "clock": "host_monotonic_capture_marker",
            "input_anchor": "first_complete_target_input_all_accepted_ns",
            "appearance_deadline_ns": 100_000_000, "verification_duration_ns": 192_000_000,
            "maximum_source_marker_gap_ns": 10_000_000, "range_guard_ns": 10_000_000,
            "appearance_decision_rule": "first_source_marker_at_or_after_nominal_deadline",
            "maximum_appearance_observation_bracket_ns": 10_000_000,
            "minimum_post_completion_hold_ns": 312_000_000,
            "qualified_temporal_classifier_ids": [], "qualified_temporal_classifiers": {},
            "scope": {}, "basis": [],
        }
        result = _base_result(policy_id, fallback)
        result["execution"] = {"status": "INCOMPLETE", "fatal_integrity_errors": [str(exc)],
                               "temporal_classification_errors": []}
        result["reason_code"] = "FATAL_EVIDENCE_INTEGRITY"
        return result
    result = _base_result(policy_id, policy)
    frozen_events = deepcopy(events)
    frozen_classifications = deepcopy(temporal_classifications)
    result["submitted_temporal_classifications"] = (
        deepcopy(frozen_classifications) if isinstance(frozen_classifications, list) else [])
    errors = deepcopy(fatal_integrity_errors or [])
    if not isinstance(errors, list) or any(not isinstance(reason, str) or not reason for reason in errors):
        errors = ["fatal integrity errors are malformed"]
    if not isinstance(events, list):
        errors.append("events are not a list")
        frozen_events = []
    qualified, target_acquisition, classification_errors = _classification_map(
        frozen_classifications, frozen_events, policy["qualified_temporal_classifiers"])
    result["execution"]["fatal_integrity_errors"] = errors
    result["execution"]["temporal_classification_errors"] = classification_errors
    if classification_errors:
        result["execution"]["status"] = "INCOMPLETE"
    if errors:
        result["execution"]["status"] = "INCOMPLETE"
        result["result"] = "INCONCLUSIVE"
        result["reason_code"] = "FATAL_EVIDENCE_INTEGRITY"
        return result
    judged = [_judge_event(raw, policy, qualified, target_acquisition) if isinstance(raw, dict)
              else _inconclusive_event(raw, f"invalid-event-{index + 1}", "INVALID_EVENT_EVIDENCE",
                                       "event is not an object")
              for index, raw in enumerate(frozen_events)]
    result["events"] = judged
    counts = Counter(event["result"] for event in judged)
    result["counts"] = {"required_events": len(judged), "passed": counts["PASS"],
                        "failed": counts["FAIL"], "inconclusive": counts["INCONCLUSIVE"]}
    if not judged:
        result["result"], result["reason_code"] = "INCONCLUSIVE", "NO_REQUIRED_EVENTS"
    elif counts["FAIL"]:
        first = next(event for event in judged if event["result"] == "FAIL")
        result["result"], result["reason_code"] = "FAIL", first["reason_code"]
    elif counts["INCONCLUSIVE"] or classification_errors:
        first = next((event for event in judged if event["result"] == "INCONCLUSIVE"), None)
        result["result"] = "INCONCLUSIVE"
        result["reason_code"] = first["reason_code"] if first else "INVALID_TEMPORAL_CLASSIFICATION"
    else:
        result["result"], result["reason_code"] = "PASS", "ALL_REQUIRED_EVENTS_PASSED"
    return result
