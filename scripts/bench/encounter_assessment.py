"""Answer separate encounter questions without replacing the strict aggregate.

Held checks, observed event targets and transition observations have different
obligations. This summary preserves every required reading and supplies no global
product pass, settling allowance, response deadline or continuity certification.
"""
from __future__ import annotations

from collections import Counter
from copy import deepcopy

try:
    from .encounter_expectation import FIELDS
except ImportError:
    from encounter_expectation import FIELDS


_POINT_KEYS = ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
               "offset_seconds", "image")
_DEFINITE = {"DIFFERENCE", "PREVIOUS_INPUT_STATE", "TRANSITION_DIFFERENCE"}
_UNKNOWN = {"UNRESOLVED", "UNREAD", "INPUT_UNRESOLVED", "INPUT_IN_PROGRESS", "CONDITIONAL"}
_CHECK_STATUSES = {"MATCH", "DIFFERENCE", "UNRESOLVED", "CONDITIONAL", "PREVIOUS_INPUT_STATE", "TRANSITION_DIFFERENCE"}


def _point(sample):
    return {key: deepcopy(sample[key]) for key in _POINT_KEYS if key in sample}


def _read(sample):
    observed = sample.get("observed")
    fields = observed.get("fields") if isinstance(observed, dict) else None
    return isinstance(fields, dict) and all(isinstance(fields.get(name), dict)
        and fields[name].get("state") in ("readable", "absent", "ambiguous", "unreadable") for name in FIELDS)


def _checks(sample):
    comparison = sample.get("comparison")
    comparison = comparison if isinstance(comparison, dict) else {}
    checks = comparison.get("checks")
    checks = checks if isinstance(checks, dict) else {}
    result = {name: checks.get(name) if isinstance(checks.get(name), dict) else {} for name in FIELDS}
    joint = comparison.get("joint_state")
    result["joint_state"] = joint if isinstance(joint, dict) else {}
    return result


def _comparison_complete(sample):
    checks = _checks(sample)
    return all(checks[name].get("status") in _CHECK_STATUSES for name in FIELDS) and \
        checks["joint_state"].get("status") in _CHECK_STATUSES | {"NOT_EVALUATED"}


def _counts(samples):
    fields, joint = Counter(), Counter()
    for sample in samples:
        checks = _checks(sample)
        for name in FIELDS:
            fields[checks[name].get("status", "UNRESOLVED")] += 1
        joint[checks["joint_state"].get("status", "UNRESOLVED")] += 1
    return {"required": len(samples) * len(FIELDS), "sample_count": len(samples),
            "fields": dict(fields), "joint_states": dict(joint)}


def _issues(samples):
    groups = {}
    for sample in samples:
        checks = _checks(sample)
        for name, check in checks.items():
            status = check.get("status", "UNRESOLVED")
            if status == "MATCH":
                continue
            reason = check.get("reason") or ("required comparison is missing" if not check
                                            else "required comparison did not establish agreement")
            key = name, status, reason
            group = groups.setdefault(key, {"field": name, "status": status, "reason": reason,
                                           "frame_ids": [], "first": _point(sample)})
            group["frame_ids"].append(sample.get("frame_id"))
    return list(groups.values())


def _declared(event):
    # A declared intersection counts even if extraction failed before any image
    # was selected/read. Legacy sequence callers without ranges can establish
    # only the events containing their explicit sample observations.
    return bool(event.get("declared_range_coverage")) or bool(event.get("coverage", {}).get("selected_recorded_frames"))


def _response_observed(event, samples):
    point = event.get("first_correct")
    if not isinstance(point, dict) or type(point.get("capture_ns")) is not int:
        return False
    capture = point["capture_ns"]
    if not event["start_ns"] <= capture < event["end_ns"]:
        return False
    ranges = event.get("declared_range_coverage", [])
    if ranges and not any(r["start_ns"] <= capture < r["end_ns"] for r in ranges):
        return False
    return any(_read(sample) and not sample.get("selection_error")
               and sample.get("video_frame_index") == point.get("video_frame_index")
               and sample.get("capture_ns") == capture and sample.get("frame_id") == point.get("frame_id")
               for sample in samples)


def _after_correct(events):
    result = {"differing_spans": 0, "unresolved_spans": 0, "differing_frames": 0,
              "unresolved_frames": 0, "details": [],
              "basis": "Later literal differences and unknowns are retained. First correctness does not prove a settled state; "
                       "permitted blinking is checked by the existing expectation, with no new tolerance."}
    for event in events:
        first = event.get("first_correct")
        if not isinstance(first, dict):
            continue
        for index, span in enumerate(event.get("observation_spans", [])):
            if span["first"]["capture_ns"] <= first["capture_ns"]:
                continue
            judgment = span.get("judgment", {})
            differing = judgment.get("status") == "NOT_CORRECT" or bool(judgment.get("not_correct_fields")) \
                or judgment.get("joint_state") in _DEFINITE
            unknown = judgment.get("status") in _UNKNOWN or bool(judgment.get("unresolved_fields")) \
                or judgment.get("joint_state") in ("UNRESOLVED", "NOT_EVALUATED", "CONDITIONAL")
            if not differing and not unknown:
                continue
            count = span["frame_count"]
            if differing:
                result["differing_spans"] += 1
                result["differing_frames"] += count
            if unknown:
                result["unresolved_spans"] += 1
                result["unresolved_frames"] += count
            result["details"].append({"event_id": event["event_id"], "span_index": index,
                "first": deepcopy(span["first"]), "last": deepcopy(span["last"]), "frame_count": count,
                "status": judgment.get("status"), "joint_state": judgment.get("joint_state"),
                "reason": judgment.get("reason"),
                "unresolved_reasons": {name: span.get("observed", {}).get(name, {}).get("reason")
                                       for name in judgment.get("unresolved_fields", [])},
                "not_correct_fields": list(judgment.get("not_correct_fields", [])),
                "unresolved_fields": list(judgment.get("unresolved_fields", []))})
    return result


def event_findings(sequence, product):
    """Index literal witnesses independently of the timed policy verdict.

    Differences during acquisition are measurements, not automatically firmware
    faults. Keep the last original witness as well as the first: a card still
    present seconds later must not be buried behind an early transition image.
    """
    decisions = {event["event_id"]: event for event in (product or {}).get("events", [])}
    findings = []
    for event in sequence.get("events", []):
        if product is not None and event["event_id"] not in decisions:
            continue
        if product is None and not _declared(event):
            continue
        differences, unknowns = {}, {}
        target = event.get("target") or {}
        for span in event.get("observation_spans", []):
            judgment = span.get("judgment", {})
            # A target still being sent is not yet an expected screen state.
            status = judgment.get("status")
            if status == "INPUT_IN_PROGRESS":
                continue
            comparable = status in ("CORRECT", "NOT_CORRECT", "UNRESOLVED")
            wrong = list(judgment.get("not_correct_fields", [])) if comparable else []
            if comparable and judgment.get("joint_state") in _DEFINITE:
                wrong.append("joint_state")
            for name in wrong:
                observed = deepcopy(span.get("observed", {}).get(name))
                group = differences.setdefault(name, {
                    "field": name, "frames": 0, "first": deepcopy(span["first"]),
                    "first_observed": observed,
                    "expected": deepcopy(target.get("joint_states") if name == "joint_state"
                                         else target.get("fields", {}).get(name)),
                })
                group.update(last=deepcopy(span["last"]), last_observed=observed)
                group["frames"] += span["frame_count"]
            unresolved = list(judgment.get("unresolved_fields", [])) if comparable else list(FIELDS)
            if judgment.get("joint_state") in ("UNRESOLVED", "CONDITIONAL", "NOT_EVALUATED"):
                unresolved.append("joint_state")
            for name in unresolved:
                group = unknowns.setdefault(name, {"field": name, "frames": 0,
                                                  "first": deepcopy(span["first"])})
                group["frames"] += span["frame_count"]
                group["last"] = deepcopy(span["last"])
        findings.append({"event_id": event["event_id"],
                         "policy_result": decisions.get(event["event_id"], {}).get("result"),
                         "input": deepcopy(event.get("wire_rows", [])),
                         "input_anchor_ns": (event.get("target_basis") or {}).get("first_complete_target_input_ns"),
                         "first_correct": deepcopy(event.get("first_correct")),
                         "differences": list(differences.values()),
                         "unresolved": list(unknowns.values()),
                         "coverage": deepcopy(event.get("coverage", {}))})
    return findings


def assess(samples, errors, sequence):
    """Summarize existing comparisons and event observations without new readings.

    PASS belongs only to a specifically named answer. Transition unknowns remain
    in their denominator; missing/interrupted analysis blocks even an otherwise
    observed-response answer. No result here modifies the strict run verdict.
    """
    problems = list(dict.fromkeys([*errors, *("Sequence interpretation: " + str(reason)
                                            for reason in sequence.get("errors", []))]))
    missing = []
    for sample in samples:
        if not _read(sample) or sample.get("selection_error") or not _comparison_complete(sample):
            missing.append({**_point(sample), "reason": sample.get("selection_error") or
                            "required source image or comparison was not analyzed"})
        if sample.get("role") not in ("held", "transition"):
            problems.append("Required sample has an unsupported observation role")
    if not samples:
        problems.append("No required samples were analyzed")
    problems = list(dict.fromkeys(problems))
    complete = bool(samples) and not problems and not missing
    held_samples = [sample for sample in samples if sample.get("role") == "held"]
    transition_samples = [sample for sample in samples if sample.get("role") == "transition"]
    held = _counts(held_samples)
    held["issues"] = _issues(held_samples)
    failed = held["fields"].get("DIFFERENCE", 0) or held["joint_states"].get("DIFFERENCE", 0)
    held["status"] = ("FAIL" if failed else "NOT_EVALUATED" if not held_samples else
                      "INCONCLUSIVE" if not complete or held["issues"] else "PASS")
    held["basis"] = "Every original held field and joint-state requirement is retained. A supported failure survives other unknowns."
    events = [event for event in sequence.get("events", []) if _declared(event)]
    missing_events = [event["event_id"] for event in events if not _response_observed(event, samples)]
    response = {"status": "PASS" if complete and events and not missing_events else "INCONCLUSIVE",
                "required": len(events), "observed": len(events) - len(missing_events),
                "missing_event_ids": missing_events,
                "basis": "Each declared event target was observed in at least one analyzed original frame before superseding input. "
                         "This is an existential observation, not timely response, continuity, or a product acceptance verdict."}
    if not events:
        response["reason"] = "No declared input events have sequence evidence"
    elif not complete:
        response["reason"] = "Required analysis is incomplete or has errors; observed targets do not override this"
    elif missing_events:
        response["reason"] = "Some declared event targets were not observed correct; missing alerts are not inferred from unreadable or unsampled frames"
    return {"schema_version": 1, "held": held, "event_response": response,
            "transitions": {**_counts(transition_samples),
                "basis": "All transition field comparisons and joint states are observations; none are removed or relabelled as correct."},
            "after_correct": _after_correct(events),
            "execution": {"status": "COMPLETE" if complete else "INCOMPLETE",
                          "missing_observation_frames": missing}, "errors": problems}
