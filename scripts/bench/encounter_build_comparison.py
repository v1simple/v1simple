"""Compare two camera behavior summaries without adding a response deadline.

Only like inputs, effective settings, targets and reader rules are comparable.
Firmware and host tooling identities are provenance, not compatibility keys.
Changed observations are reported with their uncertainty; disappearance of an
observed discrepancy is never promoted here into a proven firmware repair.
"""
from __future__ import annotations

from copy import deepcopy
import json
import math
from pathlib import PurePosixPath

try:
    from .encounter_capability import frequency_capability
except ImportError:
    from encounter_capability import frequency_capability


def _json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _witness(value, run):
    if not isinstance(value, dict):
        return None
    result = {key: deepcopy(value[key]) for key in (
        "frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
        "offset_seconds", "image_sha256", "comparison_status", "observed",
    ) if key in value}
    image = value.get("image")
    if isinstance(image, str) and image and not any(c in image for c in ("\\", ":", "?", "#", "%")):
        path = PurePosixPath(image)
        if not path.is_absolute() and ".." not in path.parts and path.suffix.lower() == ".png":
            result.update(run=run, image=path.as_posix())
    return result


def _time(point, anchor):
    capture = point.get("capture_ns") if isinstance(point, dict) else None
    return (capture - anchor) / 1e6 if _number(capture) and _number(anchor) else None


def _coverage_facts(coverage):
    # Independent run clocks are not changes in measurement coverage.
    clocks = ("start_ns", "end_ns", "first_read_capture_ns", "last_read_capture_ns")
    result = {key: value for key, value in coverage.items() if key not in clocks}
    start, end = coverage.get("start_ns"), coverage.get("end_ns")
    if _number(start) and _number(end):
        result["duration_ms"] = (end - start) / 1e6
        first, last = coverage.get("first_read_capture_ns"), coverage.get("last_read_capture_ns")
        result["first_read_after_start_ms"] = (first - start) / 1e6 if _number(first) else None
        result["last_read_before_end_ms"] = (end - last) / 1e6 if _number(last) else None
    return result


def _finding(finding, observation, run):
    anchor = observation.get("input_anchor_ns")
    return {**{key: deepcopy(finding.get(key)) for key in ("field", "kind", "expected", "observed", "reason")},
            "first": _witness(finding.get("first"), run),
            "last": _witness(finding.get("last"), run),
            "first_ms": _time(finding.get("first"), anchor),
            "last_ms": _time(finding.get("last"), anchor)}


def _finding_key(finding):
    value = {key: finding.get(key) for key in ("field", "kind", "expected", "observed")}
    if finding.get("kind") == "blink_phase_held" and isinstance(value["observed"], dict):
        # A different number of frames showing the same held phase is a
        # changed extent, not a newly appearing content defect.
        value["observed"] = {key: value["observed"].get(key) for key in ("phase_id", "phase")}
    return _json(value)


def _snapshot(event, run):
    observation = event["observation"]
    fields = {}
    for name, field in observation["fields"].items():
        fields[name] = {
            "target_observed": field.get("target_observed"),
            "first_target_ms": _time(field.get("first_target_observation"), observation.get("input_anchor_ns")),
            "counts": deepcopy(field.get("counts", {})),
            "last_definite_matches_target": field.get("end_state", {}).get("last_definite_matches_target"),
            "unresolved_suffix_frames": sum(item.get("frame_count", 0) for item in
                                             field.get("end_state", {}).get("unresolved_suffix", [])),
        }
    return {"event_id": event["event_id"], "target_observed": observation["target_observed"],
            "first_target_ms": observation["first_target_ms"], "fields": fields,
            "phase_observation": deepcopy(event.get("phase_observation", {})),
            "findings": [_finding(f, observation, run) for f in event["findings"]],
            "coverage": deepcopy(event.get("coverage", observation.get("coverage", {})))}


def _validate(summary, label):
    reasons = []
    if not isinstance(summary, dict):
        return [f"{label}: behavior summary is missing"]
    if summary.get("schema_version") != 1 or summary.get("kind") != "firmware_visual_behavior":
        reasons.append(f"{label}: unsupported behavior summary")
    evidence = summary.get("evidence", {})
    if not isinstance(evidence, dict):
        return reasons + [f"{label}: evidence is missing"]
    runtime = evidence.get("runtime_identity")
    if not isinstance(runtime, dict) or not runtime.get("git_sha") or not runtime.get("image_id"):
        reasons.append(f"{label}: recorded firmware identity is missing")
    configuration = evidence.get("configuration", {})
    if not isinstance(configuration, dict) or not isinstance(configuration.get("settings"), dict) or not configuration["settings"]:
        reasons.append(f"{label}: effective settings are missing")
    if not isinstance(summary.get("reader_method"), dict) or not summary["reader_method"]:
        reasons.append(f"{label}: reader method is missing")
    contract = summary.get("behavior_contract", {})
    if not isinstance(contract, dict) or not contract.get("comparison_key"):
        reasons.append(f"{label}: behavior comparison rules are missing")
    events = summary.get("events")
    if not isinstance(events, list) or not events:
        return reasons + [f"{label}: authored input events are missing"]
    ids = set()
    for index, event in enumerate(events):
        if not isinstance(event, dict):
            reasons.append(f"{label}: event {index} is malformed")
            continue
        eid = event.get("event_id")
        if not isinstance(eid, str) or not eid or eid in ids:
            reasons.append(f"{label}: event identities are missing or duplicated")
        ids.add(eid if isinstance(eid, str) else index)
        if event.get("input_key") is None or not isinstance(event.get("target"), dict) or not event["target"]:
            reasons.append(f"{label}: event {index} has no authored input or target")
        observation = event.get("observation", {})
        if not isinstance(observation, dict) or type(observation.get("target_observed")) is not bool:
            reasons.append(f"{label}: event {index} target observation is unavailable")
            continue
        first_ms = observation.get("first_target_ms")
        if first_ms is not None and not _number(first_ms):
            reasons.append(f"{label}: event {index} appearance time is malformed")
        fields = observation.get("fields")
        if not isinstance(fields, dict) or not fields or any(not isinstance(f, dict) for f in fields.values()):
            reasons.append(f"{label}: event {index} field measurements are missing")
        else:
            for name, field in fields.items():
                counts, end = field.get("counts"), field.get("end_state")
                if (type(field.get("target_observed")) is not bool or not isinstance(counts, dict)
                        or any(type(counts.get(key)) is not int or counts[key] < 0 for key in
                               ("matching_frames", "different_frames", "unresolved_frames"))
                        or not isinstance(end, dict) or not isinstance(end.get("unresolved_suffix", []), list)
                        or any(not isinstance(item, dict) or type(item.get("frame_count")) is not int
                               or item["frame_count"] < 0 for item in end.get("unresolved_suffix", []))):
                    reasons.append(f"{label}: event {index} {name} measurement counts are malformed")
        findings = event.get("findings")
        if not isinstance(findings, list) or any(not isinstance(f, dict) or not f.get("field") or not f.get("kind") for f in findings):
            reasons.append(f"{label}: event {index} classified findings are missing")
    return reasons


def compare_behavior_runs(current, baseline):
    """Return factual differences, or explain why the runs cannot be compared.

    The caller resolves witness ``image`` paths relative to the named run and
    verifies the file before rendering a link. This function never constructs
    external or absolute links from summary contents.
    """
    result = {"schema_version": 1, "kind": "firmware_visual_build_comparison",
              "status": "INCOMPATIBLE", "compatible": False, "reasons": [], "events": [],
              "basis": "Like-input observations only. Timing changes are measurements, not deadline failures. "
                       "Unknown readings and capture gaps remain unknown; absent findings alone do not prove a repair."}
    try:
        # Validate JSON values up front, including non-finite numbers.
        _json(current)
        _json(baseline)
        result["reasons"] = _validate(current, "current") + _validate(baseline, "baseline")
    except (TypeError, ValueError):
        result["reasons"] = ["behavior summaries must contain finite JSON values"]
    if result["reasons"]:
        return result
    for label, summary in (("current", current), ("baseline", baseline)):
        result[label] = {"runtime_identity": deepcopy(summary["evidence"]["runtime_identity"]),
                         "tooling_source": deepcopy(summary["evidence"].get("tooling_source", summary.get("tooling_source")))}
    capabilities = {label: frequency_capability(summary["evidence"].get("primary_frequency_calibration"))
                    for label, summary in (("current", current), ("baseline", baseline))}
    states = {capability["status"] for capability in capabilities.values()}
    if "NOT_RECORDED" in states:
        reason = "Unknown-count comparisons are unreliable: calibrated frequency fallback availability " \
                 "was not recorded for one or both analyses."
    elif len(states) > 1:
        reason = "Unknown-count comparisons are unreliable: calibrated frequency fallback availability differs."
    else:
        reason = "Both analyses recorded the same calibrated frequency fallback availability."
    result["unknown_count_comparison"] = {
        "status": "COMPARABLE" if len(states) == 1 and "NOT_RECORDED" not in states else "UNRELIABLE",
        "reason": reason, **capabilities,
        "basis": "This checks reader capability only; image quality and coverage can still differ. "
                 "Observed counts and content findings are retained. This is not a firmware verdict.",
    }
    for name, a, b in (
        ("effective settings", current["evidence"]["configuration"]["settings"], baseline["evidence"]["configuration"]["settings"]),
        ("reader method", current["reader_method"], baseline["reader_method"]),
        ("behavior comparison rules", current["behavior_contract"]["comparison_key"], baseline["behavior_contract"]["comparison_key"]),
        ("ordered authored inputs", [e["input_key"] for e in current["events"]], [e["input_key"] for e in baseline["events"]]),
        ("expected targets", [e["target"] for e in current["events"]], [e["target"] for e in baseline["events"]]),
        ("measured fields", [sorted(e["observation"]["fields"]) for e in current["events"]],
         [sorted(e["observation"]["fields"]) for e in baseline["events"]]),
    ):
        if _json(a) != _json(b):
            result["reasons"].append(name + " differ")
    if result["reasons"]:
        return result

    for new_event, old_event in zip(current["events"], baseline["events"]):
        new, old = _snapshot(new_event, "current"), _snapshot(old_event, "baseline")
        old_findings = {_finding_key(f): f for f in old["findings"]}
        new_findings = {_finding_key(f): f for f in new["findings"]}
        added = [new_findings[key] for key in sorted(new_findings.keys() - old_findings.keys())]
        removed = [old_findings[key] for key in sorted(old_findings.keys() - new_findings.keys())]
        timing = {"baseline_ms": old["first_target_ms"], "current_ms": new["first_target_ms"],
                  "difference_ms": (new["first_target_ms"] - old["first_target_ms"]
                                    if _number(new["first_target_ms"]) and _number(old["first_target_ms"]) else None)}
        phase_coverage_changed = any(new["phase_observation"].get(key) != old["phase_observation"].get(key)
                                     for key in ("required_phase_ids", "observed_phase_ids"))
        content_changed = bool(added or removed or phase_coverage_changed or new["target_observed"] != old["target_observed"] or any(
            (new["fields"][name]["target_observed"], new["fields"][name]["last_definite_matches_target"])
            != (old["fields"][name]["target_observed"], old["fields"][name]["last_definite_matches_target"])
            for name in new["fields"]))
        timing_changed = new["first_target_ms"] != old["first_target_ms"] or any(
            new["fields"][name]["first_target_ms"] != old["fields"][name]["first_target_ms"] for name in new["fields"]) or (
            sorted(_json([_finding_key(f), f["first_ms"], f["last_ms"]]) for f in new["findings"])
            != sorted(_json([_finding_key(f), f["first_ms"], f["last_ms"]]) for f in old["findings"]))
        uncertainty_changed = any(
            (new["fields"][name]["counts"].get("unresolved_frames"), new["fields"][name]["unresolved_suffix_frames"])
            != (old["fields"][name]["counts"].get("unresolved_frames"), old["fields"][name]["unresolved_suffix_frames"])
            for name in new["fields"])
        coverage_changed = _coverage_facts(new["coverage"]) != _coverage_facts(old["coverage"])
        result["events"].append({"event_id": new["event_id"], "baseline_event_id": old["event_id"],
                                 "input_key": deepcopy(new_event["input_key"]), "baseline": old, "current": new,
                                 "appearance": timing, "newly_observed_findings": added,
                                 "previously_observed_findings_absent": removed,
                                 "content_changed": content_changed, "timing_changed": timing_changed,
                                 "uncertainty_changed": uncertainty_changed,
                                 "coverage_changed": coverage_changed,
                                 "changed": content_changed or timing_changed or uncertainty_changed or coverage_changed})
    result.update(status="COMPARED", compatible=True,
                  summary={"compared_events": len(result["events"]), **{
                      key + "_events": sum(bool(event[key]) for event in result["events"])
                      for key in ("changed", "content_changed", "timing_changed", "uncertainty_changed", "coverage_changed")}})
    return result
