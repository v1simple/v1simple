"""Translate exact-window encounter observations into product-judgment input.

The adapter does not choose frames and does not inspect pixels.  Its caller must
provide every original recorded frame in the union of the input-only product
windows.  This module proves that membership against the source sidecar, compares
each literal reader observation with the sequence target and its predecessor, and
retains the complete original observation beside the compact status.
"""

from __future__ import annotations

from copy import deepcopy
from typing import Any

try:
    from .encounter_expectation import FIELDS, compare_sample
except ImportError:
    from encounter_expectation import FIELDS, compare_sample


SCHEMA_VERSION = 1
WINDOW_LEAD_NS = 10_000_000
WINDOW_AFTER_ANCHOR_NS = 312_000_000
# The existing secondary support-chain bound. These originals can only close
# a qualified unresolved verification boundary, never extend the product hold.
AUXILIARY_CLOSURE_CONTEXT_NS = 80_000_000

_DEFINITE = {"DIFFERENCE"}
_UNCERTAIN = {"UNRESOLVED", "CONDITIONAL"}


class ProductAdapterEvidenceError(ValueError):
    """The supplied sequence or source-window originals are not exact."""


def _require(condition: bool, reason: str) -> None:
    if not condition:
        raise ProductAdapterEvidenceError(reason)


def _integer(value: Any, name: str, minimum: int = 0) -> int:
    _require(type(value) is int and value >= minimum, f"invalid {name}")
    return value


def _target_supported(target: Any) -> bool:
    """The product scope is supported exactly when all seven fields resolve."""
    if not isinstance(target, dict) or not isinstance(target.get("fields"), dict):
        return False
    return all(isinstance(target["fields"].get(name), dict)
               and "unresolved" not in target["fields"][name]
               and isinstance(target["fields"][name].get("allowed"), list)
               and bool(target["fields"][name]["allowed"])
               for name in FIELDS)


def _retained_semantics(event: dict, event_id: str,
                        target: dict) -> tuple[str, dict | None]:
    """Validate semantics derived before any product-range filtering."""
    mode = event.get("mode")
    _require("previous_target" in event,
             f"sequence event {event_id} has no retained previous target")
    previous_target = event.get("previous_target")
    _require(mode in {"BASELINE", "UNCHANGED", "CHANGED"},
             f"sequence event {event_id} has no retained mode")
    _require(previous_target is None or isinstance(previous_target, dict),
             f"sequence event {event_id} has invalid previous target")
    expected = ("BASELINE" if previous_target is None else
                "UNCHANGED" if target == previous_target else "CHANGED")
    _require(mode == expected,
             f"sequence event {event_id} retained mode disagrees with previous target")
    return mode, previous_target


def _phase_ids(target: dict) -> list[str]:
    phases = target.get("joint_states")
    _require(isinstance(phases, list), "sequence target has invalid joint states")
    return [f"phase-{index}" for index in range(1, len(phases) + 1)]


def _full_match(comparison: dict | None, permitted_phase_ids: list[str]) -> bool:
    if not isinstance(comparison, dict):
        return False
    checks = comparison.get("checks")
    joint = comparison.get("joint_state")
    return (isinstance(checks, dict)
            and all(isinstance(checks.get(name), dict)
                    and checks[name].get("status") == "MATCH" for name in FIELDS)
            and isinstance(joint, dict)
            and joint.get("status") == "MATCH"
            and joint.get("state_id") in permitted_phase_ids)


def _definitely_different(comparison: dict | None) -> bool:
    if not isinstance(comparison, dict):
        return False
    checks = comparison.get("checks", {})
    joint = comparison.get("joint_state", {})
    return (any(isinstance(checks.get(name), dict)
                and checks[name].get("status") in _DEFINITE for name in FIELDS)
            or isinstance(joint, dict) and joint.get("status") in _DEFINITE)


def _direct_fields(comparisons: list[dict | None], statuses: set[str]) -> list[str]:
    affected = []
    for name in FIELDS:
        if any(isinstance(comparison, dict)
               and isinstance(comparison.get("checks", {}).get(name), dict)
               and comparison["checks"][name].get("status") in statuses
               for comparison in comparisons):
            affected.append(name)
    return affected


def _joint_definitely_impossible(current: dict | None, previous: dict | None) -> bool:
    def differs(comparison: dict | None) -> bool:
        joint = comparison.get("joint_state", {}) if isinstance(comparison, dict) else {}
        return isinstance(joint, dict) and joint.get("status") in _DEFINITE

    return differs(current) and (previous is None or differs(previous))


def _safe_compare(target: dict | None, original: dict) -> tuple[dict | None, str | None]:
    if target is None:
        return None, "comparison target is unavailable"
    observed = original.get("observed")
    if not isinstance(observed, dict):
        return None, "original frame has no literal reader observation"
    try:
        return compare_sample(target, observed, role="held"), None
    except (KeyError, TypeError, ValueError) as exc:
        return None, f"{type(exc).__name__}: {exc}"


def _comparison_summary(comparison: dict | None) -> dict | None:
    if not isinstance(comparison, dict):
        return None
    return {"status": comparison.get("status"),
            "checks": {name: check.get("status") for name, check in
                       comparison.get("checks", {}).items() if isinstance(check, dict)},
            "joint_state": {key: comparison.get("joint_state", {}).get(key)
                            for key in ("status", "state_id", "reason")
                            if key in comparison.get("joint_state", {})}}


def _adapt_observation(original: dict, target: dict, previous_target: dict | None,
                       mode: str, current_phase_ids: list[str],
                       previous_phase_ids: list[str]) -> dict:
    current, current_error = _safe_compare(target, original)
    previous, previous_error = _safe_compare(previous_target, original)

    previous_match = mode == "CHANGED" and _full_match(previous, previous_phase_ids)
    current_match = _full_match(current, current_phase_ids)
    # A readable field which differs from the current target but has no
    # contradiction with the previous target remains previous-state evidence
    # even when another field is unreadable.  Keeping it as PREVIOUS prevents a
    # field-specific temporal classifier from excusing that known mismatch.
    previous_compatible = (mode == "CHANGED" and _definitely_different(current)
                           and not _definitely_different(previous))
    if previous_match or previous_compatible:
        status = "PREVIOUS"
    elif current_match:
        status = "CURRENT"
    elif _definitely_different(current) and (
            previous is None or _definitely_different(previous)):
        status = "DEFINITE_OTHER"
    else:
        status = "UNRESOLVED"

    comparisons = [current, previous]
    if status == "DEFINITE_OTHER":
        affected = _direct_fields(comparisons, _DEFINITE)
    elif status == "UNRESOLVED":
        affected = (_direct_fields(comparisons, _UNCERTAIN)
                    if current is not None else list(FIELDS))
    else:
        affected = []

    impossible = status == "DEFINITE_OTHER" and _joint_definitely_impossible(current, previous)
    if impossible:
        affected.append("joint_state")

    compact = {
        "frame_id": original["frame_id"],
        "video_frame_index": original["video_frame_index"],
        "source_frame_seq": original["source_frame_seq"],
        "capture_ns": original["capture_ns"],
        "raw_status": status,
        "raw_affected_fields": affected,
        # The enclosing encounter result retains the full immutable source
        # observation once.  Product evidence links to it instead of copying
        # large OCR/pixel diagnostics into every judgment layer.
        "source_observation_ref": {key: deepcopy(original[key]) for key in
                                   ("frame_id", "video_frame_index", "source_frame_seq",
                                    "capture_ns", "image", "image_sha256") if key in original},
        "raw_comparison_status": {"current": _comparison_summary(current),
                                  "previous": _comparison_summary(previous)},
    }
    errors = [reason for reason in (current_error, previous_error if previous_target is not None else None)
              if reason is not None]
    if errors:
        compact["comparison_errors"] = errors
    if status == "CURRENT":
        compact["joint_state_id"] = current["joint_state"]["state_id"]
    if impossible:
        compact["joint_impossible"] = True
    return compact


def _validate_source_records(source_records: Any) -> list[dict]:
    _require(isinstance(source_records, list) and bool(source_records),
             "source camera records are missing")
    written = []
    for position, record in enumerate(source_records):
        _require(isinstance(record, dict) and isinstance(record.get("status"), str),
                 f"invalid source record {position}")
        if record["status"] != "written":
            continue
        _integer(record.get("host_capture_ns"), f"source record {position} capture timestamp")
        _integer(record.get("frame_seq"), f"source record {position} frame sequence", 1)
        _integer(record.get("duration_ns"), f"source record {position} duration", 1)
        if written:
            _require(record["host_capture_ns"] > written[-1]["host_capture_ns"]
                     and record["frame_seq"] > written[-1]["frame_seq"],
                     "written source records are not strictly ordered")
        written.append(record)
    _require(bool(written), "source camera records contain no written frames")
    return written


def _validate_originals(originals: Any, written: list[dict]) -> dict[int, dict]:
    _require(isinstance(originals, list), "source-window originals are not a list")
    by_index: dict[int, dict] = {}
    prior_index = -1
    for position, original in enumerate(originals):
        _require(isinstance(original, dict), f"invalid source-window original {position}")
        index = _integer(original.get("video_frame_index"),
                         f"source-window original {position} video frame index")
        _require(index < len(written), f"source-window original {position} has no source frame")
        _require(index > prior_index, "source-window originals are duplicated or out of order")
        _require(isinstance(original.get("frame_id"), str) and bool(original["frame_id"]),
                 f"source-window original {position} has no stable frame id")
        row = written[index]
        _require(original.get("capture_ns") == row["host_capture_ns"]
                 and original.get("source_frame_seq") == row["frame_seq"],
                 f"source-window original {position} disagrees with its source frame")
        by_index[index] = original
        prior_index = index
    return by_index


def _event_shape(event: Any, position: int) -> tuple[
        str, dict, dict, int, int, str, str, dict | None]:
    _require(isinstance(event, dict), f"invalid sequence event {position}")
    event_id = event.get("event_id")
    _require(isinstance(event_id, str) and bool(event_id), f"sequence event {position} has no id")
    target = event.get("target")
    _require(isinstance(target, dict), f"sequence event {event_id} has no target")
    basis = event.get("target_basis")
    _require(isinstance(basis, dict), f"sequence event {event_id} has no target basis")
    anchor = _integer(basis.get("first_complete_target_input_ns"),
                      f"sequence event {event_id} target anchor", WINDOW_LEAD_NS)
    end = _integer(event.get("end_ns"), f"sequence event {event_id} end")
    _require(end > anchor, f"sequence event {event_id} ends before its target")
    end_reason = event.get("end_reason")
    _require(isinstance(end_reason, str) and bool(end_reason),
             f"sequence event {event_id} has no end reason")
    mode, previous_target = _retained_semantics(event, event_id, target)
    return event_id, target, basis, anchor, end, end_reason, mode, previous_target


def _coverage(start: int, end: int, indices: list[int], originals: dict[int, dict],
              written: list[dict], source_records: list[dict]) -> dict:
    selected = [index for index in indices if index in originals]
    read = [index for index in selected if "observed" in originals[index]]
    captures = [written[index]["host_capture_ns"] for index in indices]
    gaps = [right - left for left, right in zip(captures, captures[1:])]
    drops = [record for record in source_records if record.get("status") != "written"
             and type(record.get("host_capture_ns")) is int
             and start <= record["host_capture_ns"] < end]
    discontinuities = []
    for left, right in zip(indices, indices[1:]):
        if right != left + 1 or written[right]["frame_seq"] != written[left]["frame_seq"] + 1:
            discontinuities.append({
                "before_video_frame_index": left,
                "after_video_frame_index": right,
                "before_source_frame_seq": written[left]["frame_seq"],
                "after_source_frame_seq": written[right]["frame_seq"],
            })
    return {
        "available_recorded_frames": len(indices),
        "selected_recorded_frames": len(selected),
        "read_recorded_frames": len(read),
        "unread_recorded_frames": len(indices) - len(read),
        "unrecorded_source_frames": len(drops),
        "maximum_source_marker_gap_ns": max(gaps, default=0),
        "complete_recorded_frame_coverage": bool(indices) and len(read) == len(indices),
        "first_source_boundary_gap_ns": captures[0] - start if captures else end - start,
        "last_source_boundary_gap_ns": end - captures[-1] if captures else end - start,
        "unread_video_frame_indices": [index for index in indices if index not in read],
        "unrecorded_source_frame_sequences": [record.get("frame_seq") for record in drops],
        "source_sequence_discontinuities": discontinuities,
    }


def adapt_sequence_events(sequence: dict, source_window_originals: list[dict],
                          source_records: list[dict]) -> dict:
    """Build compact VISIBLE_EVENT_PRESENTATION inputs without selecting frames.

    ``source_window_originals`` must contain every written source image in the
    union of ``[anchor - 10 ms, min(anchor + 392 ms, event end))`` and no other
    images. The final 80 ms is separate auxiliary closure evidence. Event
    clipping remains visible through the unchanged 312 ms product window.
    """
    result = {"schema_version": SCHEMA_VERSION, "kind": "encounter_product_adapter",
              "events": [], "errors": []}
    frozen_sequence = deepcopy(sequence)
    # These inputs are read-only throughout. Avoid duplicating every retained
    # OCR and pixel diagnostic in memory; the side-effect test protects this.
    frozen_originals = source_window_originals
    frozen_records = source_records
    try:
        _require(isinstance(frozen_sequence, dict)
                 and frozen_sequence.get("schema_version") == 1,
                 "invalid encounter sequence result")
        sequence_errors = frozen_sequence.get("errors")
        _require(isinstance(sequence_errors, list) and not sequence_errors,
                 "encounter sequence contains unresolved interpretation errors")
        sequence_events = frozen_sequence.get("events")
        _require(isinstance(sequence_events, list) and bool(sequence_events),
                 "encounter sequence contains no events")
        written = _validate_source_records(frozen_records)
        originals = _validate_originals(frozen_originals, written)

        shaped = [_event_shape(event, position) for position, event in enumerate(sequence_events)]
        event_ids = [item[0] for item in shaped]
        _require(len(event_ids) == len(set(event_ids)), "encounter sequence has duplicate event ids")
        expected_union = set()
        for _, _, _, anchor, event_end, _, _, _ in shaped:
            start, end = anchor - WINDOW_LEAD_NS, min(
                anchor + WINDOW_AFTER_ANCHOR_NS + AUXILIARY_CLOSURE_CONTEXT_NS, event_end)
            expected_union.update(index for index, row in enumerate(written)
                                  if start <= row["host_capture_ns"] < end)
        _require(set(originals) == expected_union,
                 "source-window originals are not the exact written-frame union")

        for ((event_id, target, basis, anchor, event_end, end_reason, mode,
              previous_target), source_event) in zip(shaped, sequence_events):
            current_phases = _phase_ids(target)
            prior_phases = _phase_ids(previous_target) if previous_target is not None else []
            start = anchor - WINDOW_LEAD_NS
            effective_end = min(anchor + WINDOW_AFTER_ANCHOR_NS, event_end)
            indices = [index for index, row in enumerate(written)
                       if start <= row["host_capture_ns"] < effective_end]
            observations = [_adapt_observation(originals[index], target, previous_target,
                                               mode, current_phases, prior_phases)
                            for index in indices]
            closure_start = anchor + WINDOW_AFTER_ANCHOR_NS
            closure_end = max(closure_start, min(
                closure_start + AUXILIARY_CLOSURE_CONTEXT_NS, event_end))
            closure_indices = [index for index, row in enumerate(written)
                               if closure_start <= row["host_capture_ns"] < closure_end]
            result["events"].append({
                "event_id": event_id,
                "mode": mode,
                "supported": _target_supported(target),
                "target_basis": deepcopy(basis),
                "end_ns": event_end,
                "end_reason": end_reason,
                "selection_window": {"start_ns": start,
                                     "end_ns": anchor + WINDOW_AFTER_ANCHOR_NS},
                "required_joint_state_ids": current_phases,
                "coverage": _coverage(start, effective_end, indices, originals,
                                      written, frozen_records),
                "observations": observations,
                "closure_context": {
                    "selection_window": {"start_ns": closure_start, "end_ns": closure_end},
                    "observations": [_adapt_observation(
                        originals[index], target, previous_target, mode,
                        current_phases, prior_phases) for index in closure_indices],
                    "coverage": _coverage(closure_start, closure_end, closure_indices,
                                          originals, written, frozen_records),
                },
                "source_target": deepcopy(target),
                "source_previous_target": deepcopy(previous_target),
                "source_sequence_event": deepcopy(source_event),
            })
    except (KeyError, TypeError, ProductAdapterEvidenceError) as exc:
        result["events"] = []
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    return result
