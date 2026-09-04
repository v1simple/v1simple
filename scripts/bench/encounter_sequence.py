"""Describe recorded V1 display responses without inventing a response deadline.

This module sees literal readings and validated inputs, never pixels. Every change
of authored packet content defines an event. Repeated identical packets remain one
event, including the second accepted mute display needed by the existing contract.
Capture timestamps are observation markers, not exposure bounds or DUT receipts.
"""
from __future__ import annotations

from collections import Counter
from copy import deepcopy

try:
    from .encounter_expectation import FIELDS, compare_sample, encounter_expectation_at
except ImportError:
    from encounter_expectation import FIELDS, compare_sample, encounter_expectation_at


_DEFINITE = {"DIFFERENCE", "PREVIOUS_INPUT_STATE", "TRANSITION_DIFFERENCE"}
_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image")
_BASIS = (
    "Literal recorded-frame observations against validated V1 packet-state events. "
    "No response deadline is applied. Host-send-to-capture intervals are not DUT "
    "latency: DUT receipt, exposure integration and absolute clock error are unestablished. "
    "Unsampled gaps and unreadable frames cannot establish absence of a brief response."
)


def _require(condition, reason):
    if not condition:
        raise ValueError(reason)


def _point(sample):
    return {key: sample[key] for key in _POINT_KEYS if key in sample}


def _literal(sample):
    fields = sample.get("observed", {}).get("fields", {})
    result = {}
    for name in FIELDS:
        reading = fields.get(name)
        if not isinstance(reading, dict):
            reading = {"state": "unreadable", "reason": "no reader observation"}
        result[name] = {key: value for key, value in reading.items() if key in ("state", "value", "reason")}
    return result


def _policy(expected):
    return {key: expected.get(key) for key in ("fields", "joint_states", "secondary_policy")}


def _validate(timeline, source_records, samples):
    states = timeline.get("states")
    _require(isinstance(states, list) and bool(states), "validated encounter states are missing")
    previous = None
    for state in states:
        _require(all(type(state.get(key)) is int for key in (
            "stimulus_sequence", "stimulus_requested_ns", "all_accepted_ns")),
            "incomplete encounter input timing")
        _require(state["stimulus_requested_ns"] <= state["all_accepted_ns"],
                 "input accepted before its request")
        _require(isinstance(state.get("packet_signature"), list) and bool(state["packet_signature"]),
                 "encounter packet signature is missing")
        _require(previous is None or (state["stimulus_sequence"] > previous["stimulus_sequence"]
                 and state["stimulus_requested_ns"] > previous["stimulus_requested_ns"]),
                 "encounter input is out of order")
        previous = state
    _require(timeline.get("stimuli") and timeline.get("accepted"), "validated input timeline is incomplete")
    _require([(s["stimulus_sequence"], s["stimulus_requested_ns"], s["all_accepted_ns"]) for s in states]
             == [(s.get("stimulus_sequence"), s.get("stimulus_requested_ns"), s.get("all_accepted_ns"))
                 for s in timeline["stimuli"]], "encounter states disagree with validated stimulus timing")
    accepted = timeline["accepted"]
    _require(all(all(type(packet.get(k)) is int for k in (
        "display_requested_ns", "display_attempted_ns", "display_accepted_ns")) and
        packet["display_requested_ns"] <= packet["display_attempted_ns"] <= packet["display_accepted_ns"]
        for packet in accepted), "invalid accepted host-send bounds")
    _require(all((b["display_accepted_ns"], b.get("global_tx_sequence", 0)) >
                 (a["display_accepted_ns"], a.get("global_tx_sequence", 0))
                 for a, b in zip(accepted, accepted[1:])), "accepted input is out of order")
    for state in states:
        packets = [p for p in accepted if p.get("stimulus_sequence") == state["stimulus_sequence"]]
        _require([p.get("payload_hex") for p in packets] == state["packet_signature"]
                 and max((p["display_accepted_ns"] for p in packets), default=None) == state["all_accepted_ns"],
                 "accepted packets do not complete the encounter state")
    _require(bool(source_records), "source camera records are missing")
    written = [r for r in source_records if r.get("status") == "written"]
    _require(bool(written) and all(type(r.get("host_capture_ns")) is int
             and type(r.get("frame_seq")) is int and type(r.get("duration_ns")) is int
             and r["duration_ns"] > 0 for r in written), "invalid source camera timing")
    _require(all(b["host_capture_ns"] > a["host_capture_ns"] and b["frame_seq"] > a["frame_seq"]
                 for a, b in zip(written, written[1:])), "source camera records are out of order")
    originals = {}
    for sample in samples:
        if "video_frame_index" not in sample:
            continue
        index = sample["video_frame_index"]
        _require(type(index) is int and 0 <= index < len(written), "sample has no matching source image")
        row = written[index]
        _require(sample.get("capture_ns") == row["host_capture_ns"]
                 and sample.get("source_frame_seq") == row["frame_seq"],
                 "sample timing disagrees with its original source image")
        if index in originals:
            _require(originals[index].get("observed") == sample.get("observed"),
                     "duplicate source image has conflicting reader observations")
        originals.setdefault(index, sample)
    return written, [sample for _, sample in sorted(originals.items())]


def event_windows(timeline, source_records):
    """Input-only event bounds; unchanged repeats do not create artificial responses.

    An unscoped table/display request stops the current event because its semantics
    are no longer solely the authored state. No replay period or nominal FPS is
    assumed for the final event: the source-camera extent supplies its outer bound.
    """
    written = [r for r in source_records if r.get("status") == "written"]
    _require(bool(written), "no source camera images")
    end = written[-1]["host_capture_ns"] + written[-1]["duration_ns"]
    groups = []
    for state in timeline["states"]:
        if groups and groups[-1]["states"][0]["packet_signature"] == state["packet_signature"]:
            groups[-1]["states"].append(state)
        else:
            groups.append({"states": [state], "start_ns": state["stimulus_requested_ns"]})
    for index, group in enumerate(groups):
        next_start = groups[index + 1]["start_ns"] if index + 1 < len(groups) else end
        unscoped = [p["display_requested_ns"] for p in timeline["accepted"]
                    if p.get("stimulus_sequence") is None and p.get("packet_id") in (0x31, 0x43)
                    and group["start_ns"] <= p["display_requested_ns"] < next_start]
        group["end_ns"] = min(next_start, end, *unscoped) if unscoped else min(next_start, end)
        group["end_reason"] = ("unscoped_input_requested" if unscoped and min(unscoped) == group["end_ns"]
                               else "next_changed_input_requested" if next_start < end else "camera_recording_ended")
        group["event_id"] = f"event-{index + 1:04d}"
    return groups


def _target(group, timeline, configuration):
    candidates = []
    members = {s["stimulus_sequence"] for s in group["states"]}
    for state in group["states"]:
        if state["all_accepted_ns"] >= group["end_ns"]:
            continue
        expected = encounter_expectation_at(timeline, state["all_accepted_ns"], configuration)
        if expected["input"]["ready"] and expected["input"].get("stimulus_sequence") in members:
            candidates.append((state, expected))
    if not candidates:
        return None, None
    last, expected = candidates[-1]
    matching = next(state for state, candidate in candidates if _policy(candidate) == _policy(expected))
    basis = {"target_stimulus_sequence": last["stimulus_sequence"],
             "target_sampled_at_ns": last["all_accepted_ns"],
             "first_complete_target_input_ns": matching["all_accepted_ns"],
             "first_complete_target_stimulus_sequence": matching["stimulus_sequence"]}
    return deepcopy(expected), basis


def _coverage(start, end, written, samples, source_records):
    available = {i: r for i, r in enumerate(written) if start <= r["host_capture_ns"] < end}
    selected = {s["video_frame_index"]: s for s in samples if start <= s["capture_ns"] < end}
    read = {i: s for i, s in selected.items() if "observed" in s}
    times = [start, *(s["capture_ns"] for s in read.values()), end] if end > start else []
    drops = [r for r in source_records if r.get("status") != "written"
             and type(r.get("host_capture_ns")) is int and start <= r["host_capture_ns"] < end]
    return {"start_ns": start, "end_ns": end, "available_recorded_frames": len(available),
            "selected_recorded_frames": len(selected), "read_recorded_frames": len(read),
            "unread_recorded_frames": len(set(available) - set(read)),
            "unrecorded_source_frames": len(drops),
            "complete_recorded_frame_coverage": bool(available) and set(available) == set(read),
            "first_read_capture_ns": next(iter(read.values()))["capture_ns"] if read else None,
            "last_read_capture_ns": next(reversed(read.values()))["capture_ns"] if read else None,
            "maximum_gap_between_read_markers_ms": max((b - a for a, b in zip(times, times[1:])), default=0) / 1e6,
            "basis": "All recorded images read does not establish continuous visibility between camera exposures."}


def _status(sample, target, current, target_basis):
    if "observed" not in sample:
        return {"status": "UNREAD", "reason": "selected source image was not read", "fields": {},
                "unresolved_fields": list(FIELDS)}
    if target is None:
        return {"status": "INPUT_UNRESOLVED", "reason": "no complete supported target input before the event ended",
                "fields": {}, "unresolved_fields": list(FIELDS)}
    comparison = compare_sample(target, sample["observed"], role="transition")
    fields = {name: check["status"] for name, check in comparison["checks"].items()}
    definite = [name for name, status in fields.items() if status in _DEFINITE]
    unresolved = [name for name, status in fields.items() if status in ("UNRESOLVED", "CONDITIONAL")]
    joint = comparison["joint_state"]["status"]
    result = {"fields": fields, "joint_state": joint, "not_correct_fields": definite,
              "unresolved_fields": unresolved}
    if not current["input"]["ready"]:
        result.update(status="INPUT_UNRESOLVED", reason=current["input"].get("unresolved"))
    elif sample["capture_ns"] < target_basis["first_complete_target_input_ns"]:
        result.update(status="INPUT_IN_PROGRESS", reason="the complete target input state was not yet accepted")
    elif definite or joint in _DEFINITE:
        result["status"] = "NOT_CORRECT"
    elif unresolved or joint != "MATCH":
        result["status"] = "UNRESOLVED"
    else:
        result["status"] = "CORRECT"
    return result


def _timing(group, basis, first, last_not_correct, timeline, coverage):
    first_state = group["states"][0]
    if not first or not basis:
        return {"status": "unavailable", "reason": "no all-required correct frame was observed after complete input"}
    packets = [p for p in timeline["accepted"]
               if p.get("stimulus_sequence") == basis["first_complete_target_stimulus_sequence"]]
    completing = max(packets, key=lambda p: p["display_accepted_ns"])
    initial = min((p for p in timeline["accepted"]
                   if p.get("stimulus_sequence") == first_state["stimulus_sequence"]),
                  key=lambda p: p["display_requested_ns"])
    capture = first["capture_ns"]
    return {"status": "observed_capture_marker", "first_correct": first,
            "a_differing_frame_preceded_correct": last_not_correct is not None,
            "last_definite_not_correct": last_not_correct,
            "stimulus_requested_ns": first_state["stimulus_requested_ns"],
            "first_notification_requested_ns": initial["display_requested_ns"],
            "completing_notification_send_bounds_ns": [completing["display_attempted_ns"],
                                                       completing["display_accepted_ns"]],
            "request_to_first_correct_capture_ms": (capture - first_state["stimulus_requested_ns"]) / 1e6,
            "host_send_to_first_correct_capture_ms": [(capture - completing["display_accepted_ns"]) / 1e6,
                                                      (capture - completing["display_attempted_ns"]) / 1e6],
            "complete_recorded_frame_prefix": coverage["complete_recorded_frame_coverage"],
            "unrecorded_source_frames_in_prefix": coverage["unrecorded_source_frames"],
            "physical_appearance_interval": None,
            "physical_appearance_reason": "Exposure integration, DUT receipt, and absolute clock error are unestablished; "
                "earlier correctness between captures or through an unreadable frame cannot be excluded."}


def interpret_sequence(samples, timeline, source_records, configuration=None, ranges=None):
    """Return useful event observations; malformed inputs yield explicit errors.

    ``timeline`` must come from build_encounter_timeline and source records from
    the hash-verified capture. ``ranges`` optionally contains absolute monotonic
    [start,end) boundaries selected before reading. No observations are repaired
    or carried through an unknown. The caller retains per-frame reader details.
    """
    result = {"schema_version": 1, "basis": _BASIS, "events": [], "errors": []}
    try:
        written, originals = _validate(timeline, source_records, samples)
        for start, end in ranges or []:
            _require(type(start) is int and type(end) is int and start < end,
                     "invalid declared observation range")
        groups = event_windows(timeline, source_records)
        previous_target = None
        for group in groups:
            start, end = group["start_ns"], group["end_ns"]
            if end <= start:
                continue
            target, basis = _target(group, timeline, configuration)
            if target is not None:
                target["previous_input"] = previous_target
            selected = [s for s in originals if start <= s["capture_ns"] < end]
            event = {"event_id": group["event_id"], "start_ns": start, "end_ns": end,
                     "end_reason": group["end_reason"],
                     "stimulus_sequences": [s["stimulus_sequence"] for s in group["states"]],
                     "target": _policy(target) if target else None, "target_basis": basis,
                     "wire_rows": deepcopy(group["states"][0].get("rows", [])),
                     "changed_fields": [name for name in FIELDS if target is not None and (
                         previous_target is None or target["fields"][name] != previous_target["fields"][name])],
                     "first_correct": None, "last_definite_not_correct_before_first": None,
                     "target_already_correct_in_preceding_observation": None,
                     "first_correct_by_field": {name: None for name in FIELDS},
                     "observation_spans": [], "gaps": [], "changes_after_correct": []}
            previous, last_not_correct, counts = None, None, Counter()
            before = [s for s in originals if s["capture_ns"] < start]
            if before:
                baseline = before[-1]
                event["preceding_observation"] = {**_point(baseline), "observed": _literal(baseline)}
                if target is not None and "observed" in baseline:
                    comparison = compare_sample(target, baseline["observed"], role="transition")
                    event["target_already_correct_in_preceding_observation"] = (
                        all(check["status"] == "MATCH" for check in comparison["checks"].values())
                        and comparison["joint_state"]["status"] == "MATCH")
                    if any(check["status"] in _DEFINITE for check in comparison["checks"].values()) \
                            or comparison["joint_state"]["status"] in _DEFINITE:
                        last_not_correct = _point(baseline)
            for sample in selected:
                current = encounter_expectation_at(timeline, sample["capture_ns"], configuration)
                judgment = _status(sample, target, current, basis)
                counts[judgment["status"]] += 1
                adjacent = previous is not None and sample["video_frame_index"] == previous["video_frame_index"] + 1 \
                    and sample["source_frame_seq"] == previous["source_frame_seq"] + 1
                if previous is not None and not adjacent:
                    event["gaps"].append({"before": _point(previous), "after": _point(sample),
                        "unread_recorded_frames": sample["video_frame_index"] - previous["video_frame_index"] - 1,
                        "unobserved_source_sequence_positions": sample["source_frame_seq"] - previous["source_frame_seq"] - 1})
                literal = _literal(sample)
                span = event["observation_spans"][-1] if event["observation_spans"] else None
                if adjacent and span and span["judgment"] == judgment and span["observed"] == literal:
                    span["last"] = _point(sample)
                    span["frame_count"] += 1
                else:
                    span = {"first": _point(sample), "last": _point(sample), "frame_count": 1,
                            "observed": literal, "judgment": judgment}
                    event["observation_spans"].append(span)
                    if event["first_correct"] is not None:
                        event["changes_after_correct"].append({"span_index": len(event["observation_spans"]) - 1,
                            "first": _point(sample), "status": judgment["status"],
                            "not_correct_fields": judgment.get("not_correct_fields", []),
                            "unresolved_fields": judgment.get("unresolved_fields", [])})
                supported = current["input"]["ready"] and basis is not None \
                    and sample["capture_ns"] >= basis["first_complete_target_input_ns"]
                if supported:
                    for name, status in judgment["fields"].items():
                        if status == "MATCH" and event["first_correct_by_field"][name] is None:
                            event["first_correct_by_field"][name] = _point(sample)
                if judgment["status"] == "CORRECT" and event["first_correct"] is None:
                    event["first_correct"] = _point(sample)
                    event["last_definite_not_correct_before_first"] = last_not_correct
                elif judgment["status"] == "NOT_CORRECT" and event["first_correct"] is None:
                    last_not_correct = _point(sample)
                previous = sample
            coverage = _coverage(start, end, written, originals, source_records)
            event["coverage"] = coverage
            event["declared_range_coverage"] = [_coverage(max(start, lo), min(end, hi), written, originals, source_records)
                                                 for lo, hi in ranges or [] if max(start, lo) < min(end, hi)]
            first = event["first_correct"]
            prefix = _coverage(start, first["capture_ns"] + 1, written, originals, source_records) if first else coverage
            event["timing"] = _timing(group, basis, first, event["last_definite_not_correct_before_first"], timeline, prefix)
            event["observation_counts"] = dict(counts)
            event["uncertainty_before_first_correct"] = [
                {"span_index": index, "first": span["first"], "last": span["last"],
                 "status": span["judgment"]["status"],
                 "unresolved_fields": span["judgment"].get("unresolved_fields", [])}
                for index, span in enumerate(event["observation_spans"])
                if (first is None or span["first"]["capture_ns"] < first["capture_ns"])
                and (span["judgment"]["status"] in ("UNRESOLVED", "UNREAD", "INPUT_UNRESOLVED", "INPUT_IN_PROGRESS")
                     or span["judgment"].get("unresolved_fields"))]
            event["outcome"] = ("CORRECT_OBSERVED" if first else "NOT_OBSERVED" if not selected else
                                "CORRECT_NOT_OBSERVED")
            regressions = [change for change in event["changes_after_correct"] if change["status"] == "NOT_CORRECT"]
            event["not_correct_after_correct"] = regressions
            event["summary"] = (
                f"All required content was first observed correct at {first.get('offset_seconds', first['capture_ns'] / 1e9):.6f}s; "
                f"{len(regressions)} later differing observation spans were retained."
                if first else "No frame in this event was analyzed." if not selected else
                "All required content was never observed correct in the analyzed frames; this does not establish a missed alert.")
            if event["target_already_correct_in_preceding_observation"]:
                event["summary"] += " The target also matched the preceding observation; an input-caused visual response is not established."
            if not coverage["complete_recorded_frame_coverage"] or coverage["unrecorded_source_frames"]:
                event["summary"] += " Event coverage has unobserved images or source gaps."
            result["events"].append(event)
            previous_target = target
    except (ValueError, KeyError, TypeError) as exc:
        result["events"] = []
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    return result
