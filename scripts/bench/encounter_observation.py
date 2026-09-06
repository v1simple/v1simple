"""Index literal event observations without inventing a response deadline.

This is a report adapter over encounter_sequence, not another image reader.
Matching, differing and unreadable observations remain separate. Capture-marker
brackets describe recorded observations; they are not physical exposure bounds.
"""
from __future__ import annotations

from copy import deepcopy

try:
    from .encounter_expectation import FIELDS
except ImportError:
    from encounter_expectation import FIELDS


_DIFFERENT = {"DIFFERENCE", "PREVIOUS_INPUT_STATE", "TRANSITION_DIFFERENCE"}
_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image", "image_sha256")


def _point(point):
    return {key: deepcopy(point[key]) for key in _POINT_KEYS if key in point}


def _field_status(span, name):
    judgment = span.get("judgment", {})
    if judgment.get("status") not in ("CORRECT", "NOT_CORRECT", "UNRESOLVED"):
        return "UNRESOLVED"
    return judgment.get("fields", {}).get(name, "UNRESOLVED")


def _witness(span, name, *, last=False):
    return {**_point(span["last" if last else "first"]),
            "observed": deepcopy(span.get("observed", {}).get(name)),
            "comparison_status": _field_status(span, name)}


def _interval(span, name):
    return {"first": _witness(span, name), "last": _witness(span, name, last=True),
            "frame_count": span["frame_count"]}


def _field_summary(event, spans, name):
    matching, different, unknown, departures = [], [], [], []
    first_target = None
    previous = last_definite = last_contrary = None
    baseline = event.get("preceding_observation")
    if baseline is not None:
        previous = {**_point(baseline), "observed": deepcopy(baseline.get("observed", {}).get(name)),
                    "comparison_status": "NOT_EVALUATED"}
    bracket = None
    for span in spans:
        status = _field_status(span, name)
        observation = _interval(span, name)
        if status == "MATCH":
            matching.append(observation)
            if first_target is None:
                first_target = observation["first"]
                lower = last_contrary
                bracket = {
                    "preceding_observation": deepcopy(previous),
                    "last_definite_difference": deepcopy(lower),
                    "first_target": deepcopy(first_target),
                    "marker_separation_ms": ((first_target["capture_ns"] - lower["capture_ns"]) / 1e6
                                              if lower else None),
                    "intervening_unresolved_frames": sum(
                        item["frame_count"] for item in unknown
                        if lower is None or item["first"]["capture_ns"] > lower["capture_ns"]),
                    "recorded_frame_gaps": [deepcopy(gap) for gap in event.get("gaps", [])
                        if (lower is None or gap["after"]["capture_ns"] > lower["capture_ns"])
                        and gap["before"]["capture_ns"] < first_target["capture_ns"]],
                    "basis": "Last differing and first matching recorded markers only; unknown images and "
                             "unsampled gaps remain unknown. Physical settling and DUT latency are not established.",
                }
            last_definite = observation["last"]
        elif status in _DIFFERENT:
            different.append(observation)
            last_contrary = observation["last"]
            last_definite = observation["last"]
            if first_target is not None:
                departures.append(observation)
        else:
            unknown.append(observation)
        previous = observation["last"]

    # Count repeated contrary literals after the first target without declaring
    # those samples a settled physical state or filling the gaps between them.
    repeated = []
    for interval in departures:
        literal = interval["first"]["observed"]
        group = next((item for item in repeated if item["observed"] == literal), None)
        if group is None:
            group = {"observed": deepcopy(literal), "frame_count": 0, "span_count": 0,
                     "first": deepcopy(interval["first"])}
            repeated.append(group)
        group["frame_count"] += interval["frame_count"]
        group["span_count"] += 1
        group["last"] = deepcopy(interval["last"])

    # A preceding-event witness is useful at the start of a bracket but is not
    # an observation of this event's ending state when nothing was read here.
    if not spans:
        previous = None
    suffix = [item for item in unknown
              if last_definite is None or item["first"]["capture_ns"] > last_definite["capture_ns"]]
    return {
        "target_observed": first_target is not None,
        "first_target_observation": first_target,
        "first_target_capture_bracket": bracket,
        "counts": {"matching_frames": sum(item["frame_count"] for item in matching),
                   "different_frames": sum(item["frame_count"] for item in different),
                   "unresolved_frames": sum(item["frame_count"] for item in unknown)},
        "difference_intervals": different,
        "unresolved_intervals": unknown,
        "latest_contrary_observation": last_contrary,
        "post_target_departures": departures,
        "repeated_contrary_literals_after_target": [item for item in repeated if item["frame_count"] > 1],
        "end_state": {
            "last_observation": previous,
            "last_definite_observation": last_definite,
            "last_definite_matches_target": (last_definite["comparison_status"] == "MATCH"
                                             if last_definite else None),
            "unresolved_suffix": suffix,
            "last_capture_to_event_end_ms": ((event["end_ns"] - previous["capture_ns"]) / 1e6
                                              if previous else None),
        },
    }


def _arrow_intervals(event, samples):
    """Retain reader-observed partial/faint color without naming its cause."""
    intervals, active = [], {}
    for sample in samples or []:
        if not event["start_ns"] <= sample.get("capture_ns", -1) < event["end_ns"]:
            continue
        reading = sample.get("observed", {}).get("fields", {}).get("main_arrows", {})
        states = reading.get("direction_states", {})
        for direction in ("front", "side", "rear"):
            state = states.get(direction, {})
            if state.get("state") not in ("partial", "faint"):
                active.pop(direction, None)
                continue
            witness = {**_point(sample), "reader_state": state["state"],
                       "rgb_median": deepcopy(state.get("rgb_median")),
                       "visible_directions": deepcopy(reading.get("visible_directions", []))}
            prior = active.get(direction)
            if (prior and prior["last"]["reader_state"] == state["state"]
                    and sample["video_frame_index"] == prior["last"]["video_frame_index"] + 1
                    and sample["source_frame_seq"] == prior["last"]["source_frame_seq"] + 1):
                prior["last"] = witness
                prior["frame_count"] += 1
            else:
                item = {"direction": direction, "first": witness, "last": deepcopy(witness), "frame_count": 1}
                active[direction] = item
                intervals.append(item)
    return intervals


def summarize_event_observations(event, samples=None):
    """Summarize a validated sequence event, optionally retaining raw arrow color.

    ``event`` is one entry from interpret_sequence. Input-in-progress samples do
    not become target differences. A first match never proves the remaining hold
    correct: later different readings, unknown suffixes and coverage all survive.
    ``samples`` may supply the original reader dictionaries; it is never decoded
    or changed here and expected inputs never affect the literal image readings.
    """
    anchor = (event.get("target_basis") or {}).get("first_complete_target_input_ns")
    spans = [span for span in event.get("observation_spans", [])
             if anchor is not None and span["first"]["capture_ns"] >= anchor
             and span.get("judgment", {}).get("status") != "INPUT_IN_PROGRESS"]
    return {"event_id": event["event_id"], "input_anchor_ns": anchor,
            "target_observed": event.get("first_correct") is not None,
            "first_target_observation": deepcopy(event.get("first_correct")),
            "target_already_correct_in_preceding_observation": event.get(
                "target_already_correct_in_preceding_observation"),
            "fields": {name: _field_summary(event, spans, name) for name in FIELDS},
            "arrow_partial_or_faint_intervals": _arrow_intervals(event, samples),
            "coverage": deepcopy(event.get("coverage", {})),
            "basis": "Literal comparisons after complete host input, without a response deadline. "
                     "First matching content does not establish a correct hold; later differences, "
                     "unknowns and sampling gaps remain explicit."}
