"""Measure authored display behavior from originals, without a response deadline.

The input determines coverage before decoding. Pixels determine literal readings.
Only then are those readings compared with the event's source-grounded target.
"""
from __future__ import annotations

from contextlib import nullcontext
from copy import deepcopy
import gzip
import json
import os
from pathlib import Path
import shutil
import subprocess

from artifact_privacy import sanitize_artifact_value
from camera_artifacts import sha256_file
from counter_check import read_json, save_json, write_png
from encounter_behavior_contract import behavior_contract
from encounter_build_comparison import compare_behavior_runs
from encounter_configuration import configuration_for_samples
from encounter_expectation import FIELDS, encounter_expectation_at
from encounter_observation import summarize_event_observations
from encounter_phase_observation import measure_event_phases
from encounter_sequence import interpret_sequence, product_event_targets, _literal, _status


DIFFERENT = {"DIFFERENCE", "PREVIOUS_INPUT_STATE", "TRANSITION_DIFFERENCE"}
RESULT_CODES = {"NO_DIFFERENCES_OBSERVED": 0, "DIFFERENCES_FOUND": 1, "MEASUREMENT_INCOMPLETE": 2}


def event_findings(event, observation, contract):
    """Index contradictions, without turning acquisition time into a failure.

    An ending difference says what remained when input changed, not how quickly
    firmware was obliged to respond. Every earlier difference remains in the
    observation history. Source grace/persistence and unknown intervals remain
    visible; neither a lone match nor a terminal unknown proves a healthy hold.
    """
    findings = []
    complete_target = event.get("first_correct")
    ending_observed = event.get("coverage", {}).get("complete_recorded_frame_coverage") is True
    for name, field in observation["fields"].items():
        end = field["end_state"]
        terminal = end.get("last_definite_observation")
        if ending_observed and terminal and end.get("last_definite_matches_target") is False:
            intervals = field["difference_intervals"]
            matching_literal = [s for s in intervals if s["last"]["observed"] == terminal["observed"]]
            first = matching_literal[0]["first"] if matching_literal else terminal
            findings.append({"field": name, "kind": "ending_difference",
                             "expected": deepcopy((event.get("target") or {}).get("fields", {}).get(name)),
                             "observed": deepcopy(terminal["observed"]), "first": first, "last": terminal,
                             "reason": "The last definite reading before the authored input ended still differed. "
                                       "This is an observed content discrepancy, not a response-time violation. "
                                       "Read the source rule and acquisition history before assigning its cause.",
                             "rule_ids": contract.get("field_rule_ids", {}).get(name, [])})
        for interval in field["post_target_departures"]:
            if complete_target is None or interval["first"]["capture_ns"] <= complete_target["capture_ns"]:
                continue  # One field can match before source-explained whole-display acquisition finishes.
            if ending_observed and terminal and terminal.get("observed") == interval["last"]["observed"] \
                    and end.get("last_definite_matches_target") is False:
                continue  # Already indexed, with its full earlier/later history.
            key = (name, json.dumps(interval["first"]["observed"], sort_keys=True))
            prior = next((f for f in findings if f.get("_key") == key), None)
            if prior:
                prior["last"] = interval["last"]
            else:
                findings.append({"_key": key, "field": name, "kind": "departure_after_target",
                                 "expected": deepcopy((event.get("target") or {}).get("fields", {}).get(name)),
                                 "observed": deepcopy(interval["first"]["observed"]),
                                 "first": interval["first"], "last": interval["last"],
                                 "reason": "A definite contrary value appeared after the complete display target had matched the current input. "
                                           "The original frames show its extent; camera transition effects and firmware cause remain distinct.",
                                 "rule_ids": contract.get("field_rule_ids", {}).get(name, [])})
    # Individually allowed blink fields must also belong to one coherent phase.
    first = event.get("first_correct")
    anchor = observation.get("input_anchor_ns")
    joint_spans = [s for s in event.get("observation_spans", [])
                   if anchor is not None and s["first"]["capture_ns"] >= anchor
                   and s["judgment"].get("status") in ("CORRECT", "NOT_CORRECT", "UNRESOLVED")
                   and s["judgment"].get("joint_state") in ({"MATCH"} | DIFFERENT)]
    ending_joint = joint_spans[-1] if ending_observed and joint_spans and joint_spans[-1]["judgment"]["joint_state"] in DIFFERENT \
        and not joint_spans[-1]["judgment"].get("not_correct_fields") else None
    if ending_joint:
        findings.append({"field": "joint_state", "kind": "ending_difference",
                         "expected": deepcopy((event.get("target") or {}).get("joint_states")),
                         "observed": {k: ending_joint["observed"][k] for k in ("counter_glyph", "active_bands", "main_arrows")},
                         "first": ending_joint["first"], "last": ending_joint["last"],
                         "reason": "The last definite counter, bands and arrows combination did not form one permitted shared blink phase. No response-time violation is inferred.",
                         "rule_ids": ["shared_blink"]})
    for span in event.get("observation_spans", []):
        judgment = span["judgment"]
        if first and span["first"]["capture_ns"] > first["capture_ns"] \
                and judgment.get("status") == "NOT_CORRECT" \
                and judgment.get("joint_state") in DIFFERENT and not judgment.get("not_correct_fields") \
                and span is not ending_joint:
            findings.append({"field": "joint_state", "kind": "departure_after_target",
                             "expected": deepcopy((event.get("target") or {}).get("joint_states")),
                             "observed": {k: span["observed"][k] for k in ("counter_glyph", "active_bands", "main_arrows")},
                             "first": span["first"], "last": span["last"],
                             "reason": "Readable counter, bands and arrows did not form one permitted shared blink phase after a complete target was observed.",
                             "rule_ids": ["shared_blink"]})
    for finding in findings:
        finding.pop("_key", None)
    return findings


def summarize(events, errors, qualification):
    summary = {
        "events": len(events),
        "targets_observed": sum(e["observation"]["target_observed"] for e in events),
        "events_with_findings": sum(bool(e["findings"]) for e in events),
        "findings": sum(len(e["findings"]) for e in events),
        "events_without_complete_target": sum(not e["observation"]["target_observed"] for e in events),
        "unresolved_frames": sum(e["unresolved_frames"] for e in events),
        "unresolved_field_observations": sum(f["counts"]["unresolved_frames"] for e in events
                                             for f in e["observation"]["fields"].values()),
        "read_frames": sum(e["coverage"]["read_recorded_frames"] for e in events),
        "available_frames": sum(e["coverage"]["available_recorded_frames"] for e in events),
        "events_with_unobserved_blink_phases": sum(
            len(e.get("phase_observation", {}).get("required_phase_ids", [])) > 1 and
            set(e["phase_observation"]["required_phase_ids"]) != set(e["phase_observation"]["observed_phase_ids"])
            for e in events),
    }
    complete = bool(events) and all(e["coverage"]["complete_recorded_frame_coverage"]
                                   and not e["coverage"]["unrecorded_source_frames"] for e in events)
    result = ("MEASUREMENT_INCOMPLETE" if errors or qualification.get("status") != "QUALIFIED" or not events else
              "DIFFERENCES_FOUND" if summary["findings"] else
              "MEASUREMENT_INCOMPLETE" if summary["events_without_complete_target"] or
              summary["events_with_unobserved_blink_phases"] or not complete else
              "NO_DIFFERENCES_OBSERVED")
    return result, summary


def _input_key(definition, data):
    members = set(definition["stimulus_sequences"])
    states = [s for s in data["timeline"]["states"] if s["stimulus_sequence"] in members]
    stimuli = {s["stimulusSequence"]: s for s in data["stimulus"]}
    following = next((s for s in data["stimulus"] if s["stimulusSequence"] > max(members)), None)
    return {"packets": states[0]["packet_signature"],
            "authored_offsets_seconds": [stimuli[s["stimulus_sequence"]]["replayOffsetSeconds"] for s in states],
            "next_authored_offset_seconds": following["replayOffsetSeconds"] if following else None}


def analyze_behavior(run, out, ranges=None, configuration=None, reader_qualification=None, compare_to=None,
                     reuse_readings=None):
    # Import shared acquisition only here: encounter_check also exposes this
    # product through its command-line entry point.
    import encounter_check as capture
    import encounter_reader
    from encounter_qualification import STATIC_READER_IMPLEMENTATION_FILES, verify_qualification
    from encounter_runtime_probe import probe_ocr_runtime
    from encounter_behavior_report import write_behavior_report

    root = Path(__file__).parents[2]
    method_dir = out / "method"
    method_dir.mkdir()
    method = {}
    for source in sorted(Path(__file__).parent.iterdir()):
        if source.suffix in (".py", ".swift", ".b64"):
            method[source.name] = sha256_file(source)
            shutil.copyfile(source, method_dir / source.name)
            capture.require(sha256_file(method_dir / source.name) == method[source.name], "method changed while retained")
    result = {"schema_version": 1, "kind": "firmware_visual_behavior", "events": [], "errors": [],
              "evidence": {}, "reader_method": {k: method[k] for k in (
                  *STATIC_READER_IMPLEMENTATION_FILES, "encounter_sequence.py",
                  "encounter_observation.py", "encounter_phase_observation.py", "encounter_behavior.py")},
              "implementation_sha256": method, "reader_qualification": {"status": "REJECTED"},
              "scope": {"measured_fields": list(FIELDS),
                        "meaning": "Counter glyph, frequency, active bands, active arrow directions, strength bars, secondary cards and MUTED badge across the authored input sequence.",
                        "not_measured": ["Counter decimal dot", "exact palette or brightness", "audio", "RF detection", "internal firmware state", "DUT-only latency"],
                        "timing": "Complete host input to recorded capture markers. No response deadline. Unknown exposure and DUT receipt prevent firmware-only latency claims."},
              "samples_index": []}
    samples, data, definitions, configuration_value = [], None, [], None
    try:
        data = capture.load_run(run)
        window = read_json(run / "window_result.json")
        result["evidence"] = {**data["identity"], "video_timing": data["timing"],
                              "recorded_tooling_source": window.get("tooling_source")}
        result["evidence"]["tooling_source"] = {
            "git_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
            "dirty": bool(subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"], cwd=root, text=True).strip())}
        identity = data["identity"].get("runtime_identity") or {}
        result["behavior_contract"] = behavior_contract(root, identity.get("git_sha"))
        definitions = product_event_targets(data["timeline"], data["source_records"])
        origin = data["stimulus"][0]["requestedHostMonotonicNs"]
        requested = [(origin + round(a * 1e9), origin + round(b * 1e9)) for a, b in ranges] if ranges else None
        bounds = [(max(d["start_ns"], lo), min(d["end_ns"], hi)) for d in definitions
                  for lo, hi in (requested or [(d["start_ns"], d["end_ns"])])
                  if max(d["start_ns"], lo) < min(d["end_ns"], hi)]
        samples = capture._select_all_frames_in_bounds(origin, data["rows"], bounds, len(data["rows"]))
        configuration_value = configuration_for_samples(data.get("recorded_configuration"), samples)
        if configuration:
            supplied = capture.configuration_for(configuration, data["identity"], samples)
            capture.require(configuration_value.get("status") != "verified" or
                            all(configuration_value["settings"].get(k) == v for k, v in supplied["settings"].items()),
                            "supplied settings contradict the recorded configuration")
            capture.require((data.get("recorded_configuration") or {}).get("status") != "available" or
                            configuration_value.get("status") == "verified", "static settings cannot override incomplete recorded configuration")
            configuration_value = supplied
        result["evidence"]["configuration"] = configuration_value
        capture.require(configuration_value.get("status") == "verified", "Effective display settings are not established across the authored events: " + configuration_value.get("reason", "unavailable"))
        settings = configuration_value["settings"]
        definitions = product_event_targets(data["timeline"], data["source_records"], settings)
        save_json(out / "selection.json", {"schema_version": 1, "kind": "authored_event_full_frame_selection",
                                           "identity": data["identity"], "bounds_ns": bounds,
                                           "selection_basis": "Every recorded frame within input-only event bounds; frozen before pixels are read.",
                                           "samples": samples})
        result["evidence"]["selection_sha256"] = sha256_file(out / "selection.json")
        runtime = encounter_reader.prepare_reader(out / "reader-cache")
        runtime["ocr_runtime_probe"] = probe_ocr_runtime(runtime)
        runtime["ocr_compiled"] = runtime.get("ocr_available") is True
        runtime["ocr_available"] = runtime["ocr_compiled"] and runtime["ocr_runtime_probe"].get("status") == "operational"
        result["evidence"]["reader"] = runtime
        # Static qualification version selects the retained blind references.
        # No temporal reinterpretation, appearance deadline or policy grade is used.
        result["reader_qualification"] = verify_qualification(
            reader_qualification, implementation_sha256=method, reader_runtime=runtime,
            camera_name=data["camera_name"], camera_profile=data["camera_profile"],
            policy={"contract_version": 3, "qualified_temporal_classifiers": {}},
            bench_source_sha256=sha256_file(root / "bench.sh"))
        capture.require(result["reader_qualification"].get("status") == "QUALIFIED",
                        "Reader qualification: " + "; ".join(result["reader_qualification"].get("errors", ["not qualified"])))
        os.link(data["video"], out / "original.mov")
        result["original_video"] = {"path": "original.mov", "sha256": sha256_file(data["video"]),
                                    "meaning": "Unchanged capture; browser seeking is context. Retained lossless PNGs identify exact witness frames."}
        (out / "frames").mkdir()
        reused = None
        if reuse_readings:
            from encounter_reading_reuse import load_reusable_readings
            reused = load_reusable_readings(reuse_readings, data, method, samples, out)
            result["evidence"]["reused_readings"] = reused["provenance"]
        definition_index, previous, previous_signature = 0, None, None
        retained = set()

        def retain(sample, pixels):
            index = sample["video_frame_index"]
            if index in retained:
                return
            path = out / sample["image"]
            if reused:
                original = reused["originals"].get(index)
                capture.require(original is not None and path.is_file(), "reused readings lack an exact required original witness")
                sample["image_sha256"] = original["image_sha256"]
            else:
                write_png(path, pixels, data["width"], data["height"])
                sample["image_sha256"] = sha256_file(path)
            row = data["rows"][index]
            result["samples_index"].append({"frame_index": index, "video_frame_index": index,
                "frame_id": sample["frame_id"], "source_frame_seq": sample["source_frame_seq"],
                "capture_ns": sample["capture_ns"], "image": sample["image"],
                "image_sha256": sample["image_sha256"], "event_id": sample["event_id"],
                "video_seconds": row["video_pts_value"] / row["video_pts_timescale"]})
            retained.add(index)

        frame_stream = (((s["video_frame_index"], None) for s in samples) if reused else capture.stream_frames(
            data["video"], [s["video_frame_index"] for s in samples], data["width"], data["height"]))
        with gzip.open(out / "readings.ndjson.gz", "xt", encoding="utf-8") as raw, \
                getattr(encounter_reader, "analysis_session", nullcontext)():
            for number, (sample, (index, pixels)) in enumerate(zip(samples, frame_stream, strict=True), 1):
                while definitions[definition_index]["end_ns"] <= sample["capture_ns"]:
                    definition_index += 1
                definition = definitions[definition_index]
                # Reader gets only original pixels and fixed camera geometry.
                try:
                    reading = reused["readings"][index] if reused else encounter_reader.observe(
                        pixels, data["width"], data["height"], data["registration"])
                    if "fields" not in reading:
                        reading = {"fields": {name: reading.get(name) for name in FIELDS},
                                   "diagnostics": {k: v for k, v in reading.items() if k not in FIELDS}}
                except Exception as exc:
                    reading = capture.unresolved(f"reader failed: {type(exc).__name__}: {exc}")
                sample.update(observed={"fields": reading["fields"]}, image=f"frames/{index:06d}.png",
                              event_id=definition["event_id"])
                raw.write(json.dumps(sanitize_artifact_value({"frame_id": sample["frame_id"],
                    "video_frame_index": index, "capture_ns": sample["capture_ns"], "observed": reading}, run_dir=out),
                    separators=(",", ":"), allow_nan=False) + "\n")
                current = encounter_expectation_at(data["timeline"], sample["capture_ns"], settings)
                judgment = _status(sample, definition["target"], current, definition["target_basis"])
                arrow_states = {k: {key: value.get(key) for key in ("state", "color")}
                                for k, value in reading["fields"].get("main_arrows", {}).get("direction_states", {}).items()}
                signature = (sample["event_id"], _literal(sample), judgment, arrow_states)
                adjacent = previous is not None and index == previous[0]["video_frame_index"] + 1 \
                    and sample["source_frame_seq"] == previous[0]["source_frame_seq"] + 1
                if signature != previous_signature or not adjacent:
                    if previous:
                        retain(*previous)
                    retain(sample, pixels)
                previous, previous_signature = (sample, pixels), signature
                if number == 1 or number % 500 == 0 or number == len(samples):
                    print(f"Read {number}/{len(samples)} original event frames", flush=True)
            if previous:
                retain(*previous)
        result["evidence"]["readings_sha256"] = sha256_file(out / "readings.ndjson.gz")
        result["samples_index"].sort(key=lambda s: s["frame_index"])
        sequence = interpret_sequence(samples, data["timeline"], data["source_records"], settings, bounds)
        result["errors"].extend(sequence.get("errors", []))
        by_id = {d["event_id"]: d for d in definitions}
        by_frame = {s["video_frame_index"]: s for s in samples}
        for event in sequence.get("events", []):
            if not event["coverage"]["selected_recorded_frames"]:
                continue
            observation = summarize_event_observations(event, samples)
            first, anchor = observation["first_target_observation"], observation["input_anchor_ns"]
            observation["first_target_ms"] = (first["capture_ns"] - anchor) / 1e6 if first and anchor is not None else None
            observation["timing"] = event["timing"]
            unknown = sum(s["frame_count"] for s in event["observation_spans"]
                          if anchor is not None and s["first"]["capture_ns"] >= anchor
                          and (s["judgment"].get("unresolved_fields") or s["judgment"].get("joint_state") == "UNRESOLVED"
                               or s["judgment"].get("status") in ("INPUT_UNRESOLVED", "UNREAD")))
            findings = event_findings(event, observation, result["behavior_contract"])
            phases = measure_event_phases(event, result["behavior_contract"])
            findings.extend(phases["findings"])
            for finding in findings:
                detail = by_frame[finding["last"]["video_frame_index"]]["observed"]["fields"].get(finding["field"])
                if detail and detail.get("state") != "readable":
                    finding["observation_detail"] = deepcopy(detail)
            result["events"].append({"event_id": event["event_id"], "input_key": _input_key(by_id[event["event_id"]], data),
                                     "wire_rows": event["wire_rows"], "start_ns": event["start_ns"], "end_ns": event["end_ns"],
                                     "target": event["target"], "observation": observation,
                                     "findings": findings,
                                     "phase_observation": phases,
                                     "coverage": event["coverage"], "unresolved_frames": unknown,
                                     "observation_spans": event["observation_spans"]})
        if ranges:
            result["scope"]["requested_ranges_seconds"] = ranges
            result["scope"]["meaning"] += " Only the explicitly selected range was read; event totals retain their full input bounds."
        if compare_to:
            baseline = read_json(compare_to)
            result["comparison"] = compare_behavior_runs(result, baseline)
            result["comparison"]["baseline_result_sha256"] = sha256_file(compare_to)
            # Keep the paired originals accessible without copying large runs.
            # Relative paths are local output references, not public metadata.
            result["comparison"]["baseline_report"] = os.path.relpath(compare_to.parent / "report.html", out)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, ImportError, subprocess.SubprocessError) as exc:
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    except KeyboardInterrupt:
        result["errors"].append("Analysis interrupted; the unfinished run does not establish display behavior")
    result["result"], result["summary"] = summarize(result["events"], result["errors"], result["reader_qualification"])
    save_json(out / "result.json", result)
    result = read_json(out / "result.json")
    write_behavior_report(out, result)
    return result
