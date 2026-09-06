"""Measure visible joint blink phases from existing literal observations."""
from __future__ import annotations

from copy import deepcopy
import math
import re

try:
    from .encounter_expectation import _normalize, compare_sample
except ImportError:
    from encounter_expectation import _normalize, compare_sample


_PHASE_FIELDS = ("counter_glyph", "active_bands", "main_arrows")


def _cadence(contract):
    rule = contract.get("rules", {}).get("shared_blink", {})
    if rule.get("status") != "VERIFIED":
        return None
    values = {int(value) for location in rule.get("locations", [])
              if location.get("path") == "src/display.h"
              for value in re.findall(r"BLINK_INTERVAL_MS\s*=\s*([0-9]+)\s*;", location.get("excerpt") or "")}
    return values.pop() if len(values) == 1 and next(iter(values)) > 0 else None


def _point(value):
    return {key: deepcopy(value[key]) for key in (
        "frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
        "offset_seconds", "image", "image_sha256") if key in value}


def _adjacent(left, right):
    return (right["video_frame_index"] == left["video_frame_index"] + 1
            and right["source_frame_seq"] == left["source_frame_seq"] + 1
            and right["capture_ns"] > left["capture_ns"])


def measure_event_phases(event, contract):
    """Index phase coverage and long fully readable holds of one blink phase.

    The source's full cycle supplies a conservative observation opportunity,
    never an input-to-display deadline. Unknown joint fields or source gaps
    break a run. Other unreadable display fields do not erase readable phases.
    """
    result = {"required_phase_ids": [], "observed_phase_ids": [], "phase_counts": {},
              "first_observations": {}, "last_observations": {}, "alternations": [],
              "contiguous_phase_spans": [], "findings": [], "unresolved_frames": 0,
              "status": "PHASES_NOT_ESTABLISHED", "source_toggle_ms": _cadence(contract),
              "basis": "Joint phase observations after complete host input. Source cadence describes "
                       "blink function; it imposes no acquisition deadline or DUT-only latency claim."}
    target = event.get("target") or {}
    phases, original_ids = [], {}
    try:
        for index, phase in enumerate(target.get("joint_states", []), 1):
            if set(phase) != set(_PHASE_FIELDS):
                return result
            normalized = {name: _normalize(name, phase[name]) for name in _PHASE_FIELDS}
            if normalized not in phases:
                phases.append(normalized)
            original_ids[f"phase-{index}"] = f"phase-{phases.index(normalized) + 1}"
    except (KeyError, TypeError, ValueError):
        return result
    ids = [f"phase-{index + 1}" for index in range(len(phases))]
    result["required_phase_ids"] = ids
    result["phase_counts"] = dict.fromkeys(ids, 0)
    anchor = (event.get("target_basis") or {}).get("first_complete_target_input_ns")
    if not ids or type(anchor) is not int:
        return result
    active, previous = None, None
    for span in event.get("observation_spans", []):
        first, last, count = span["first"], span["last"], span["frame_count"]
        if first["capture_ns"] < anchor:
            continue
        phase_id = None
        if span.get("judgment", {}).get("status") in ("CORRECT", "NOT_CORRECT", "UNRESOLVED"):
            try:
                joint = compare_sample(target, span["observed"])["joint_state"]
                if joint.get("status") == "MATCH":
                    phase_id = original_ids[joint["state_id"]]
            except (KeyError, TypeError, ValueError):
                pass
        contiguous = (type(count) is int and count > 0
                      and last["video_frame_index"] - first["video_frame_index"] == count - 1
                      and last["source_frame_seq"] - first["source_frame_seq"] == count - 1
                      and last["capture_ns"] >= first["capture_ns"])
        if phase_id is None or not contiguous:
            result["unresolved_frames"] += count
            active, previous = None, None
            continue
        result["phase_counts"][phase_id] += count
        result["first_observations"].setdefault(phase_id, _point(first))
        result["last_observations"][phase_id] = _point(last)
        adjacent = previous is not None and _adjacent(previous["last"], first)
        if adjacent and previous["phase_id"] != phase_id:
            result["alternations"].append({"from_phase_id": previous["phase_id"], "to_phase_id": phase_id,
                                            "before": _point(previous["last"]), "after": _point(first)})
        if active is not None and adjacent and active["phase_id"] == phase_id:
            active["last"] = _point(last)
            active["frame_count"] += count
        else:
            active = {"phase_id": phase_id, "first": _point(first), "last": _point(last), "frame_count": count}
            result["contiguous_phase_spans"].append(active)
        active["duration_ms"] = (active["last"]["capture_ns"] - active["first"]["capture_ns"]) / 1e6
        previous = {"phase_id": phase_id, "last": last}
    result["observed_phase_ids"] = [phase_id for phase_id in ids if result["phase_counts"][phase_id]]
    result["alternation_count"] = len(result["alternations"])
    if len(ids) == 1:
        result["status"] = "STEADY_INPUT"
        return result
    toggle = result["source_toggle_ms"]
    gap = event.get("coverage", {}).get("maximum_gap_between_read_markers_ms")
    dense = (toggle is not None and type(gap) in (int, float)
             and math.isfinite(gap) and 0 <= gap < toggle)
    result["dense_capture_markers"] = dense
    result["source_cycle_ms"] = 2 * toggle if toggle is not None else None
    if dense:
        for span in result["contiguous_phase_spans"]:
            if span["duration_ms"] <= 2 * toggle:
                continue
            result["findings"].append({
                "field": "joint_state", "kind": "blink_phase_held",
                "expected": {"joint_states": deepcopy(phases), "source_toggle_ms": toggle},
                "observed": {"phase_id": span["phase_id"],
                             "phase": deepcopy(phases[ids.index(span["phase_id"])]),
                             "frame_count": span["frame_count"], "readable_span_ms": span["duration_ms"]},
                "first": deepcopy(span["first"]), "last": deepcopy(span["last"]),
                "reason": f"One permitted blink phase remained readable across {span['frame_count']} consecutive "
                          f"captured frames spanning {span['duration_ms']:.3f} ms. The recorded source toggles "
                          f"distinct phases every {toggle} ms; no alternate phase was read inside this fully "
                          "readable interval. Exact optical transition timing and the underlying cause are unproved.",
                "rule_ids": ["shared_blink", "physical_dispatch"],
            })
    result["status"] = ("DIFFERENCES_FOUND" if result["findings"] else
                        "SOURCE_UNVERIFIED" if toggle is None else
                        "PHASES_OBSERVED" if result["observed_phase_ids"] == ids else "PHASES_NOT_ESTABLISHED")
    return result
