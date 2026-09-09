"""Observe configured radar persistence sequences using retained literal readings.

This adapter never reads pixels or substitutes elapsed host time for DUT expiry.
The input defines the required stages; originals establish which stages appeared.
An old card still present at the end is reported as retained content, not a
response-time violation. Unseen or unreadable stages remain measurement gaps.
"""
from __future__ import annotations

from copy import deepcopy

try:
    from .encounter_expectation import _normalize, _observed, EncounterEvidenceError
except ImportError:
    from encounter_expectation import _normalize, _observed, EncounterEvidenceError


def _card(row):
    return {key: row[key] for key in ("band", "frequency", "direction", "bars")}


def _continues(previous, current):
    # The card renderer treats same-band shifts up to 5MHz as one bogey.
    return (previous["band"] == current["band"] and
            abs(int(previous["frequency"].replace(".", "")) -
                int(current["frequency"].replace(".", ""))) <= 5)


def _authored_duration(event):
    offsets = event.get("input_key", {}).get("authored_offsets_seconds", [])
    end = event.get("input_key", {}).get("next_authored_offset_seconds")
    return (end - offsets[0] if offsets and end is not None else
            (event["end_ns"] - event["start_ns"]) / 1e9)


def _value(span, field):
    try:
        value = _observed(field, span.get("observed", {}).get(field, {}))
        if field == "secondary" and any(any(v is None for v in card.values()) for card in value):
            return None, False
        return value, True
    except (EncounterEvidenceError, KeyError, TypeError, ValueError):
        return None, False


def _point(span, anchor, last=False):
    point = deepcopy(span["last" if last else "first"])
    point["after_complete_host_input_ms"] = (point["capture_ns"] - anchor) / 1e6
    return point


def _stage_for_release(span, previous):
    values = {name: _value(span, name) for name in
              ("primary_frequency", "counter_glyph", "main_bars", "secondary", "muted_badge")}
    if not all(ok for _, ok in values.values()):
        return "unresolved"
    frequency, counter, bars, cards, muted = (values[name][0] for name in values)
    # The current counter must leave its live digit, independently distinguishing
    # a persisted rendering from the old live frame still arriving at the panel.
    if counter in ("A", "l", "L") and bars == 0 and cards == [] and muted is False:
        if frequency == previous["frequency"]:
            return "retained_primary"
        if frequency == "--.---":
            return "cleared"
    if frequency == previous["frequency"]:
        return "previous_live_or_transition"
    return "contrary_content"


def _stage_for_live(span, event, retired):
    target = event.get("target") or {}
    values = {}
    for name in ("primary_frequency", "counter_glyph", "main_bars", "active_bands", "main_arrows", "muted_badge", "secondary"):
        value, known = _value(span, name)
        if not known:
            return "unresolved"
        values[name] = value
    if any(values[name] not in [_normalize(name, value) for value in
                               target.get("fields", {}).get(name, {}).get("allowed", [])]
           for name in values if name != "secondary"):
        return "other_display_content"
    phases = target.get("joint_states", [])
    if not any(all(values[name] == _normalize(name, value) for name, value in phase.items()) for phase in phases):
        return "contrary_content"
    required = target.get("secondary_policy", {}).get("required", [])
    cards = values["secondary"]
    if any(card not in cards for card in required):
        return "contrary_content"
    extras = [card for card in cards if card not in required]
    historical = target.get("secondary_policy", {}).get("previously_seen", [])
    if any(card not in historical for card in extras):
        return "contrary_content"
    if extras:
        return ("retained_card_with_live" if not retired or any(card in retired for card in extras)
                else "other_retired_card")
    return "live_target"


def _measure(event, kind, required, classify, permitted_after=()):
    anchor = event.get("observation", {}).get("input_anchor_ns")
    case = {"event_id": event["event_id"], "kind": kind,
            "required_stages": required, "observed_stages": [], "stages": {},
            "findings": [], "complete_recorded_frame_coverage": False,
            "missing_stages": list(required), "stage_order_observed": False, "unresolved_ending": True}
    if type(anchor) is not int:
        case["reason"] = "Complete accepted input is unavailable."
        return case
    case["input_anchor_ns"] = anchor
    coverage = event.get("coverage", {})
    case["complete_recorded_frame_coverage"] = (coverage.get("complete_recorded_frame_coverage") is True
                                               and coverage.get("unrecorded_source_frames") == 0)
    acquired = False
    ordered = []
    last_stage = None
    for span in event.get("observation_spans", []):
        if span["first"]["capture_ns"] < anchor:
            continue
        stage = classify(span)
        state = case["stages"].setdefault(stage, {"frames": 0, "first": _point(span, anchor)})
        state["frames"] += span["frame_count"]
        state["last"] = _point(span, anchor, last=True)
        if stage in required and stage not in case["observed_stages"]:
            case["observed_stages"].append(stage)
        if len(ordered) < len(required) and stage == required[len(ordered)]:
            ordered.append(stage)
        if acquired and stage not in (required[-1], "unresolved", *permitted_after):
            case["findings"].append({"kind": "content_after_final_stage", "stage": stage,
                "first": _point(span, anchor), "last": _point(span, anchor, last=True),
                "observed": deepcopy(span["observed"]),
                "reason": "Different content appeared after the required final display stage was observed."})
        acquired |= ordered == required
        last_stage = stage
    case["missing_stages"] = [stage for stage in required if stage not in case["observed_stages"]]
    case["stage_order_observed"] = ordered == required
    # A partially unreadable ending is not silently replaced with an earlier
    # readable frame. The original unknown suffix remains a measurement limit.
    if case["complete_recorded_frame_coverage"] and last_stage not in (None, "unresolved", required[-1], *permitted_after):
        state = case["stages"][last_stage]
        case["findings"].append({"kind": "ending_content", "stage": last_stage,
            "first": state["first"], "last": state["last"],
            "reason": "The authored input ended with this observed content instead of its final required stage. "
                      "This is a content observation, not a DUT expiry-time or response-time failure."})
    case["unresolved_ending"] = last_stage == "unresolved"
    return case


def measure_persistence_behavior(events, configuration):
    """Return the bounded sequence verdict for a positive-persistence recording.

    ``events`` are the existing behavior report's validated inputs and literal
    observation spans. The verified configuration is mandatory. No new frame
    selection, optical classification or filling of unreadable intervals occurs.
    """
    seconds = configuration.get("alertPersistenceSeconds") if isinstance(configuration, dict) else None
    result = {"schema_version": 1, "kind": "radar_persistence_sequences", "cases": [],
              "configured_seconds": seconds, "result": "MEASUREMENT_INCOMPLETE",
              "scope": "Ordinary radar primary retention/clearing, live preemption and secondary-card retirement. "
                       "ALP, gray palette fidelity and exact DUT timer expiry are not measured.",
              "timing_basis": "Capture markers relative to complete host input acceptance; no response deadline. "
                              "Configured persistence defines the requested behavior, not a host-clock expiry verdict."}
    if type(seconds) is not int or not 1 <= seconds <= 5:
        result["reason"] = "This sequence evaluation requires verified positive persistence (1..5 seconds)."
        return result
    if configuration.get("stealthEnabled") is not False:
        result["reason"] = "The persistence sequence requires verified ordinary presentation (stealth disabled)."
        return result
    if any(row.get("band") not in ("X", "K", "Ka") for event in events for row in event.get("wire_rows", [])):
        result["reason"] = "Persistence sequence observation supports ordinary X/K/Ka radar only."
        return result
    previous = None
    for event in events:
        rows = event.get("wire_rows", [])
        prior_rows = previous.get("wire_rows", []) if previous else []
        prior_primary = next((row for row in prior_rows if row["priority"]), None)
        if previous is None and not rows:
            result["cases"].append(_measure(event, "initial_idle", ["cleared"],
                lambda span: _stage_for_release(span, {"frequency": None})))
        if prior_primary and not rows:
            # The short authored release intentionally probes preemption while a
            # primary is retained. Longer releases request both visible stages.
            long_release = _authored_duration(event) > seconds
            required = ["retained_primary", "cleared"] if long_release else ["retained_primary"]
            case = _measure(event, "primary_retirement", required,
                            lambda span: _stage_for_release(span, prior_primary),
                            permitted_after=() if long_release else ("cleared",))
            case["retired_alert"] = _card(prior_primary)
            result["cases"].append(case)
        if rows:
            retired = [_card(row) for row in prior_rows if not any(_continues(row, current) for current in rows)]
            if retired:
                long_release = _authored_duration(event) > seconds
                required = ["retained_card_with_live", "live_target"] if long_release else ["retained_card_with_live"]
                case = _measure(event, "secondary_retirement", required,
                                lambda span: _stage_for_live(span, event, retired),
                                permitted_after=() if long_release else ("live_target",))
                case["retired_alerts"] = retired
                result["cases"].append(case)
            elif previous and not prior_rows:
                prior_case = next((case for case in reversed(result["cases"])
                                   if case["event_id"] == previous["event_id"] and case["kind"] == "primary_retirement"), None)
                if prior_case and prior_case["required_stages"] == ["retained_primary"]:
                    case = _measure(event, "live_preemption", ["live_target"],
                                    lambda span: _stage_for_live(span, event, []))
                    case["retained_primary_before_input"] = (
                        "retained_primary" in prior_case["observed_stages"] and
                        "cleared" not in prior_case["stages"] and not prior_case["unresolved_ending"])
                    if not case["retained_primary_before_input"]:
                        case["missing_stages"].insert(0, "retained_primary_before_input")
                    result["cases"].append(case)
            if len(rows) > 1:
                result["cases"].append(_measure(event, "live_secondary_preserved", ["live_target"],
                                               lambda span: _stage_for_live(span, event, [])))
        previous = event
    cases = result["cases"]
    complete = [case for case in cases if not case.get("missing_stages", ["input"])
                and case.get("stage_order_observed") and case["complete_recorded_frame_coverage"]]
    findings = sum(len(case["findings"]) for case in cases)
    result["summary"] = {"cases": len(cases), "complete_sequences": len(complete), "findings": findings,
                         "unresolved_frames": sum(case["stages"].get("unresolved", {}).get("frames", 0) for case in cases)}
    result["result"] = ("DIFFERENCES_FOUND" if findings else
                        "NO_DIFFERENCES_OBSERVED" if cases and len(complete) == len(cases) else "MEASUREMENT_INCOMPLETE")
    return result


def persistence_result(events, errors, qualification, measurement):
    """Compose the sequence verdict with the existing complete recording checks.

    Positive-persistence idle has multiple correct successive presentations, so
    its ordinary fixed seven-field target is intentionally not a completion gate.
    Every live event still owes its ordinary complete target and blink phases.
    """
    if (errors or qualification.get("status") != "QUALIFIED" or not events or
            any(event.get("coverage", {}).get("complete_recorded_frame_coverage") is not True or
                event.get("coverage", {}).get("unrecorded_source_frames") != 0 for event in events)):
        return "MEASUREMENT_INCOMPLETE"
    if (any(event.get("findings") for event in events) or
            any(case.get("findings") for case in measurement.get("cases", []))):
        return "DIFFERENCES_FOUND"
    if measurement.get("result") != "NO_DIFFERENCES_OBSERVED":
        return "MEASUREMENT_INCOMPLETE"
    covered_idle = {case["event_id"] for case in measurement.get("cases", [])
                    if case["kind"] in ("initial_idle", "primary_retirement")}
    covered_live = {case["event_id"] for case in measurement.get("cases", [])
                    if case["kind"] in ("secondary_retirement", "live_preemption", "live_secondary_preserved")
                    and any(stage in case.get("stages", {}) for stage in ("retained_card_with_live", "live_target"))}
    for event in events:
        if not event.get("wire_rows"):
            if event["event_id"] not in covered_idle:
                return "MEASUREMENT_INCOMPLETE"
            continue
        if event.get("observation", {}).get("target_observed") is not True and event["event_id"] not in covered_live:
            return "MEASUREMENT_INCOMPLETE"
        phases = event.get("phase_observation", {})
        required = phases.get("required_phase_ids")
        if not required or set(required) != set(phases.get("observed_phase_ids", [])):
            return "MEASUREMENT_INCOMPLETE"
    return "NO_DIFFERENCES_OBSERVED"
