#!/usr/bin/env python3
"""Read sampled or consecutive V1 encounter frames from an existing recording.

No hardware is operated. Selection uses packet changes and a declared cadence,
or every recorded frame in explicit bounds, before pixels are read. A disagreement is with recorded host input; it does not
locate a firmware defect or establish a response deadline.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
import math
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

from artifact_privacy import sanitize_artifact_value
from camera_artifacts import load_capture_manifest, sha256_file, verify_capture_files
from camera_timing import validate_frame_sidecar
from counter_check import owned_input, read_json, read_records, save_json, select_frame, write_png
from encounter_expectation import build_encounter_timeline, encounter_expectation_at, compare_sample
from encounter_configuration import recorded_snapshots, configuration_for_samples

FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")
PROBES = (-.05, .05, .15, .35)
MAX_SAMPLES = 5000
MAX_TRANSITION_FRAMES = 20000
TRANSITION_BEFORE = .05
TRANSITION_AFTER = .50


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ValueError(reason)


def load_run(run: Path) -> dict:
    """Reuse the capture's hash-bound timing verification, not an unbound cache."""
    window_path = run / "window_result.json"
    window = read_json(window_path)
    stimulus_path = owned_input(run, window["artifacts"]["replay_stimulus"])
    delivery_path = owned_input(run, window["artifacts"]["replay_delivery"])
    scenario_path = run / "replay_scenario.json"
    stimulus, delivery = read_records(stimulus_path), read_records(delivery_path)
    scenario = read_json(scenario_path)
    timeline = build_encounter_timeline(scenario, stimulus, delivery)
    camera = run / "camera"
    manifest_path = camera / "capture_manifest.json"
    read_json(manifest_path)  # Reject duplicate JSON keys before the shared loader.
    manifest = load_capture_manifest(manifest_path)
    require(window.get("camera", {}).get("capture_id") == manifest["capture_id"],
            "camera capture belongs to a different replay window")
    verify_capture_files(camera, manifest)
    entries = manifest["identity"]["artifacts"]
    preflight = read_json(camera / entries["preflight"]["path"])
    registration = preflight.get("registration", {})
    require(preflight.get("result") == registration.get("result") == "PASS",
            "camera registration did not pass")
    records = read_records(camera / entries["frame_timing"]["path"])
    validate_frame_sidecar(records)
    require(all(r["phase"] == "recording" for r in records), "mixed camera phases")
    rows = [r for r in records if r["status"] == "written"]
    require(bool(rows) and all(b["host_capture_ns"] > a["host_capture_ns"]
                              for a, b in zip(rows, rows[1:])), "nonincreasing capture timestamps")
    timing = read_json(camera / entries["video_timing_verification"]["path"])
    require(timing.get("status") == "verified", "encoded video timing was not verified")
    for key in ("timestamp_error_count", "missing_encoded_frame_count", "extra_encoded_frame_count",
                "duration_mismatch_count"):
        require(type(timing.get(key)) is int and timing[key] == 0, f"video timing: {key} is not zero")
    require(timing.get("written_frame_count") == timing.get("encoded_frame_count") == len(rows),
            "timing verification does not describe all written frames")
    require(timing.get("source_frame_count") == len(records), "source-frame denominator differs")
    require(not any(r["status"] == "timestamp_error" for r in records), "camera timestamp errors")
    probe = manifest["capture"]["video_probe"]
    width, height = probe["width"], probe["height"]
    require(type(width) is int and type(height) is int and 1 <= width <= 4096 and 1 <= height <= 2160,
            "unsupported camera dimensions")
    require(f"{width}x{height}" == manifest["identity"]["camera"]["profile"].get("video_size"),
            "video dimensions differ from the capture profile")
    identity = {
        "window_result_sha256": sha256_file(window_path), "capture_id": manifest["capture_id"],
        "capture_manifest_sha256": sha256_file(manifest_path),
        "stimulus_sha256": sha256_file(stimulus_path), "delivery_sha256": sha256_file(delivery_path),
        "scenario_sha256": sha256_file(scenario_path),
        "camera_artifacts": {k: {"sha256": v["sha256"], "size_bytes": v["size_bytes"]}
                             for k, v in entries.items()},
        "runtime_identity": window.get("runtime_identity"),
        "recorded_runtime_qualification": window.get("runtime_qualification"),
        "recorded_collection_result": window.get("result"),
    }
    recorded_configuration = None
    timeline_artifact = window.get("artifacts", {}).get("bench_timeline")
    if timeline_artifact:
        try:
            timeline_path = owned_input(run, timeline_artifact)
            recorded_configuration = recorded_snapshots(read_records(timeline_path), window.get("runtime_identity"))
            identity["configuration_timeline_sha256"] = sha256_file(timeline_path)
        except (OSError, ValueError, RuntimeError, KeyError, TypeError) as exc:
            recorded_configuration = {"status": "unavailable", "reason": f"configuration timeline could not be verified: {exc}"}
    return dict(identity=identity, stimulus=stimulus, timeline=timeline, rows=rows, source_records=records, timing=timing,
                video=camera / entries["video"]["path"], width=width, height=height,
                registration=registration, recorded_configuration=recorded_configuration)


def select_samples(stimulus: list[dict], rows: list[dict], ranges: list[tuple[float, float]],
                   cadence: float) -> list[dict]:
    """Freeze every packet-state midpoint, regular hold, and fixed edge probe."""
    require(math.isfinite(cadence) and cadence > 0, "cadence must be positive and finite")
    require(bool(stimulus), "no recorded stimulus")
    origin = stimulus[0]["requestedHostMonotonicNs"]
    # Packet bytes, not expectedDisplay and not count alone, define changes.
    states = []
    for item in stimulus:
        signature = tuple((n["kind"], n["bytesHex"]) for n in item["notifications"])
        if not states or signature != states[-1]["signature"]:
            states.append(dict(start=(item["requestedHostMonotonicNs"] - origin) / 1e9,
                               signature=signature))
    camera_end = (rows[-1]["host_capture_ns"] + rows[-1]["duration_ns"] - origin) / 1e9
    targets: dict[int, set[str]] = {}

    def add(offset, reason):
        ns = origin + round(offset * 1e9)
        targets.setdefault(ns, set()).add(reason)
        require(len(targets) <= MAX_SAMPLES, "selection exceeds 5000 samples; narrow the range or cadence")

    for start, end in ranges:
        require(all(math.isfinite(x) for x in (start, end)) and 0 <= start < end <= camera_end,
                "range must be finite, increasing, nonnegative, and inside the recorded camera window")
        for index, state in enumerate(states):
            stop = states[index + 1]["start"] if index + 1 < len(states) else camera_end
            lo, hi = max(start, state["start"]), min(end, stop)
            if lo < hi:
                add((lo + hi) / 2, "packet-state midpoint")
            if index and start <= state["start"] < end:
                for delta in PROBES:
                    if start <= state["start"] + delta < end:
                        add(state["start"] + delta, "transition probe")
        offset = start + min(.5, cadence / 2, (end - start) / 2)
        while offset < end:
            add(offset, "regular hold")
            offset += cadence
    samples = []
    for number, (target, reasons) in enumerate(sorted(targets.items()), 1):
        sample = dict(frame_id=f"{number:04d}", target_capture_ns=target,
                      requested_offset_seconds=(target - origin) / 1e9, selection_reasons=sorted(reasons),
                      role="transition" if reasons == {"transition probe"} else "held")
        try:
            index, row = select_frame(rows, target)
            sample.update(video_frame_index=index, source_frame_seq=row["frame_seq"],
                          capture_ns=row["host_capture_ns"],
                          offset_seconds=(row["host_capture_ns"] - origin) / 1e9)
        except ValueError as exc:
            sample["selection_error"] = str(exc)
        samples.append(sample)
    require(bool(samples), "selection contains no samples")
    return samples


def select_all_frames(stimulus: list[dict], rows: list[dict],
                      ranges: list[tuple[float, float]], limit: int | None = None) -> list[dict]:
    """Select each recorded source image once by capture time, without cadence targets."""
    require(bool(stimulus) and bool(rows), "no recorded stimulus or camera frames")
    require(bool(ranges), "all-frame selection requires an explicit bounded range")
    limit = MAX_SAMPLES if limit is None else limit
    origin = stimulus[0]["requestedHostMonotonicNs"]
    camera_end = (rows[-1]["host_capture_ns"] + rows[-1]["duration_ns"] - origin) / 1e9
    bounds = []
    for start, end in ranges:
        require(all(math.isfinite(x) for x in (start, end)) and 0 <= start < end <= camera_end,
                "range must be finite, increasing, nonnegative, and inside the recorded camera window")
        bounds.append((origin + round(start * 1e9), origin + round(end * 1e9)))
    samples = []
    for index, row in enumerate(rows):
        capture = row["host_capture_ns"]
        if not any(start <= capture < end for start, end in bounds):
            continue
        require(len(samples) < limit, f"selection exceeds {limit} frames; narrow the explicit range")
        offset = (capture - origin) / 1e9
        samples.append(dict(frame_id=f"{len(samples) + 1:04d}", target_capture_ns=capture,
                            requested_offset_seconds=offset, selection_reasons=["every recorded frame in range"],
                            role="held", video_frame_index=index, source_frame_seq=row["frame_seq"],
                            capture_ns=capture, offset_seconds=offset))
    require(bool(samples), "explicit range contains no recorded frames")
    return samples


def select_transition_review(stimulus, rows, ranges, cadence):
    """Add every recorded frame around input changes without replacing held checks.

    Windows are chosen from packet bytes and time before observing any pixels.
    The half-second inspection window is a coverage choice, never a deadline.
    """
    samples = select_samples(stimulus, rows, ranges, cadence)
    origin = stimulus[0]["requestedHostMonotonicNs"]
    changes, prior = [], None
    for item in stimulus:
        signature = tuple((n["kind"], n["bytesHex"]) for n in item["notifications"])
        if signature != prior:
            changes.append((item["requestedHostMonotonicNs"] - origin) / 1e9)
        prior = signature
    windows = []
    for index, change in enumerate(changes):
        next_change = changes[index + 1] if index + 1 < len(changes) else float("inf")
        for start, end in ranges:
            lo, hi = max(start, change - TRANSITION_BEFORE), min(end, change + TRANSITION_AFTER, next_change)
            if lo < hi:
                windows.append((lo, hi))
    present = {s["video_frame_index"] for s in samples if "video_frame_index" in s}
    dense = select_all_frames(stimulus, rows, windows, limit=MAX_TRANSITION_FRAMES) if windows else []
    for sample in dense:
        if sample["video_frame_index"] in present:
            continue
        sample.update(role="transition", selection_reasons=["every recorded frame around an input change"])
        samples.append(sample)
    require(len(samples) <= MAX_TRANSITION_FRAMES,
            "transition review exceeds 20000 observations; use an explicit range")
    samples.sort(key=lambda s: s["target_capture_ns"])
    for index, sample in enumerate(samples, 1):
        sample["frame_id"] = f"{index:04d}"
    return samples, windows


def observation_history(samples: list[dict]) -> tuple[dict, list[dict]]:
    """Run-length encode literal readings only; preserve one-frame states and refusals."""
    spans = {field: [] for field in FIELDS}
    changes, previous = [], None
    # Repeated requests for the same original image are one visual observation.
    originals = {s["video_frame_index"]: s for s in samples if "video_frame_index" in s}
    for _, sample in sorted(originals.items()):
        point = {key: sample[key] for key in ("frame_id", "video_frame_index", "source_frame_seq",
                                             "capture_ns", "offset_seconds")}
        if "image" in sample:
            point["image"] = sample["image"]
        adjacent = previous is not None and sample["video_frame_index"] == previous["video_frame_index"] + 1 \
            and sample["source_frame_seq"] == previous["source_frame_seq"] + 1
        changed = {}
        for field in FIELDS:
            reading = sample.get("observed", {}).get("fields", {}).get(field)
            if not isinstance(reading, dict):
                reading = dict(state="unreadable", value=None, reason="analysis did not reach this required sample")
            literal = {key: reading.get(key) for key in ("state", "value")}
            if reading.get("reason"):
                literal["reason"] = reading["reason"]
            prior = spans[field][-1] if spans[field] else None
            if adjacent and prior and prior["observed"] == literal:
                prior["last"] = point
                prior["frame_count"] += 1
            else:
                spans[field].append(dict(observed=literal, first=point, last=point, frame_count=1))
            if prior is None or prior["observed"] != literal:
                changed[field] = literal
        if changed or not adjacent:
            changes.append(dict(**point, fields=changed, initial=previous is None,
                                consecutive_with_previous=adjacent))
        previous = sample
    return spans, changes


def observation_coverage(samples: list[dict], ranges: list[tuple[float, float]],
                         data: dict, all_frames: bool) -> list[dict]:
    origin = data["stimulus"][0]["requestedHostMonotonicNs"]
    regions = []
    for start, end in ranges:
        lo, hi = origin + round(start * 1e9), origin + round(end * 1e9)
        available = {index: row for index, row in enumerate(data["rows"])
                     if lo <= row["host_capture_ns"] < hi}
        selected = {s["video_frame_index"]: s for s in samples
                    if "capture_ns" in s and lo <= s["capture_ns"] < hi}
        observed = {i: s for i, s in selected.items() if "observed" in s}
        times = sorted(s["offset_seconds"] for s in observed.values())
        boundaries = [start, *times, end]
        region = dict(start_seconds=start, end_seconds=end, unique_frames=len(observed),
                      selected_unique_frames=len(selected), available_recorded_frames=len(available),
                      first_selected_offset_seconds=min((s["offset_seconds"] for s in selected.values()), default=None),
                      last_selected_offset_seconds=max((s["offset_seconds"] for s in selected.values()), default=None),
                      first_observed_offset_seconds=min(times, default=None), last_observed_offset_seconds=max(times, default=None),
                      maximum_unobserved_gap_seconds=max(b - a for a, b in zip(boundaries, boundaries[1:])),
                      maximum_capture_gap_seconds=max((b - a for a, b in zip(times, times[1:])), default=None),
                      complete_recorded_frame_coverage=bool(available) and set(observed) == set(available) if all_frames else None)
        if all_frames:
            region["unread_recorded_source_frames"] = [row["frame_seq"] for i, row in available.items() if i not in observed]
        dropped = [r for r in data.get("source_records", []) if r.get("status") != "written"
                   and type(r.get("host_capture_ns")) is int and lo <= r["host_capture_ns"] < hi]
        region["unrecorded_source_frames"] = len(dropped)
        regions.append(region)
    return regions


def balanced_selection(indices: list[int]) -> str:
    require(bool(indices), "empty frame extraction")
    if len(indices) == 1:
        return f"eq(n,{indices[0]})"
    midpoint = len(indices) // 2
    return f"({balanced_selection(indices[:midpoint])}+{balanced_selection(indices[midpoint:])})"


def stream_frames(video: Path, indices: list[int], width: int, height: int):
    """Decode selected originals once, keeping only one RGB frame in memory."""
    indices = sorted(set(indices))
    if not indices:
        return
    ffmpeg = shutil.which("ffmpeg")
    require(ffmpeg is not None, "ffmpeg is required")
    command = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(video),
               "-map", "0:v:0", "-vf", f"select='{balanced_selection(indices)}'",
               "-fps_mode", "passthrough", "-frames:v", str(len(indices)),
               "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors)
        try:
            for index in indices:
                pixels = process.stdout.read(width * height * 3)
                require(len(pixels) == width * height * 3, "decoder stopped before all selected frames")
                yield index, pixels
            require(not process.stdout.read(1), "decoder returned additional unselected pixels")
            require(process.wait() == 0, "video decoder failed")
        finally:
            process.stdout.close()
            if process.poll() is None:
                process.terminate()
                process.wait()


def configuration_for(path: Path | None, identity: dict, samples: list[dict]) -> dict | None:
    if path is None:
        return None
    document = read_json(path)
    require(document.get("window_result_sha256") == identity["window_result_sha256"]
            and document.get("runtime_identity") == identity["runtime_identity"],
            "configuration does not identify this exact window and boot")
    require(document.get("status") == "verified" and isinstance(document.get("basis"), str)
            and bool(document["basis"].strip()), "configuration requires a stated independent verification basis")
    coverage = document.get("coverage", {})
    times = [s["capture_ns"] for s in samples if "capture_ns" in s]
    require(times and type(coverage.get("start_capture_ns")) is int
            and type(coverage.get("end_capture_ns")) is int
            and coverage["start_capture_ns"] <= min(times) <= max(times) <= coverage["end_capture_ns"],
            "configuration does not cover the selected observations")
    require(isinstance(document.get("settings"), dict), "configuration settings must be an object")
    return document


def unresolved(reason: str) -> dict:
    return {"fields": {name: {"state": "unreadable", "value": None, "reason": reason} for name in FIELDS}}


def summarize(samples: list[dict], errors: list[str]) -> tuple[str, dict]:
    counts = Counter()
    joint_counts = Counter()
    roles = {role: Counter() for role in ("held", "transition")}
    for sample in samples:
        checks = sample.get("comparison", {}).get("checks", {})
        for field in FIELDS:
            status = checks.get(field, {}).get("status", "UNRESOLVED")
            counts[status] += 1
            roles[sample["role"]][status] += 1
        joint = sample.get("comparison", {}).get("joint_state", {}).get("status")
        if joint:
            joint_counts[joint] += 1
    verdict = ("FAIL" if counts["DIFFERENCE"] or joint_counts["DIFFERENCE"] else
               "INCONCLUSIVE" if errors or any(counts[k] for k in counts if k != "MATCH")
               or any(joint_counts[k] for k in joint_counts if k not in ("MATCH", "NOT_EVALUATED"))
               or not samples else "PASS")
    return verdict, {"required": len(samples) * len(FIELDS), "fields": dict(counts),
                     "joint_states": dict(joint_counts), "by_role": {k: dict(v) for k, v in roles.items()}}


def analyze(run: Path, out: Path, ranges: list[tuple[float, float]] | None, cadence: float,
            configuration: Path | None = None, transition_only: bool = False, all_frames: bool = False,
            inspect_transitions: bool = False) -> dict:
    samples, errors, evidence, data, transition_windows, config = [], [], {}, None, [], None
    method = {p.name: sha256_file(p) for p in Path(__file__).parent.glob("*.py")}
    for p in Path(__file__).parent.glob("encounter_*.swift"):
        method[p.name] = sha256_file(p)
    (out / "method").mkdir()
    for name, digest in method.items():
        source = Path(__file__).with_name(name)
        shutil.copyfile(source, out / "method" / name)
        require(sha256_file(out / "method" / name) == digest, "analysis source changed while being retained")
    result = dict(schema_version=1, kind="sampled_encounter_check", full_run_correctness="not_evaluated",
                  selection_mode="input_transition_review" if inspect_transitions else "all_recorded_frames" if all_frames else "transition_windows" if transition_only else "encounter_samples",
                  response_deadline="not_evaluated", reader_qualification="see independent validation; not established by this run",
                  comparison_basis=("Consecutive recorded-frame" if all_frames else "Sampled") +
                  " display agreement with recorded host input. Host acceptance is not DUT receipt. Transition observations impose no response deadline.",
                  implementation_sha256=method, environment=dict(python=platform.python_version(), system=platform.platform()))
    try:
        data = load_run(run)
        evidence = {**data["identity"], "video_timing": data["timing"],
                    "timing_basis": "hash-bound original capture verification and validated source sidecar"}
        origin = data["stimulus"][0]["requestedHostMonotonicNs"]
        require(not all_frames or ranges is not None, "all-frame selection requires an explicit bounded range")
        if ranges is None:
            end = (data["stimulus"][-1]["requestedHostMonotonicNs"] - origin) / 1e9
            ranges = [(0, end)]
        require(not inspect_transitions or not (all_frames or transition_only),
                "automatic transition review cannot be combined with explicit all-frame or transition-only selection")
        if inspect_transitions:
            samples, transition_windows = select_transition_review(data["stimulus"], data["rows"], ranges, cadence)
        else:
            samples = (select_all_frames(data["stimulus"], data["rows"], ranges) if all_frames else
                       select_samples(data["stimulus"], data["rows"], ranges, cadence))
        if transition_only:
            for sample in samples:
                sample["role"] = "transition"
        recorded_config = configuration_for_samples(data.get("recorded_configuration"), samples)
        evidence["recorded_configuration"] = recorded_config
        config = recorded_config if recorded_config.get("status") == "verified" else None
        if configuration is not None:
            require((data.get("recorded_configuration") or {}).get("status") != "available" or config is not None,
                    "supplied static configuration cannot override incomplete or changing recorded CFG coverage")
            supplied = configuration_for(configuration, evidence, samples)
            if config:
                require(all(config["settings"].get(k) == v for k, v in supplied["settings"].items()),
                        "supplied configuration contradicts recorded normal-runtime settings")
            config = supplied
        if config:
            evidence["configuration"] = dict(config)
            if configuration is not None:
                evidence["configuration"]["sha256"] = sha256_file(configuration)
        # Immutable input-only selection is published before any reader call.
        save_json(out / "selection.json", dict(schema_version=1, identity=data["identity"], ranges=ranges,
                                               selection_mode=result["selection_mode"],
                                               cadence_seconds=None if all_frames else cadence,
                                               transition_offsets_seconds=[] if all_frames else PROBES,
                                               consecutive_transition_windows=transition_windows, samples=samples))
        by_index = {}
        for sample in samples:
            if "video_frame_index" in sample:
                by_index.setdefault(sample["video_frame_index"], []).append(sample)
        (out / "frames").mkdir()
        import encounter_reader
        observe = encounter_reader.observe
        if hasattr(encounter_reader, "prepare_reader"):
            evidence["reader"] = encounter_reader.prepare_reader(out / "reader-cache")
        total = len(by_index)
        for number, (index, pixels) in enumerate(stream_frames(data["video"], list(by_index), data["width"], data["height"]), 1):
            image = out / "frames" / f"{index:06d}.png"
            write_png(image, pixels, data["width"], data["height"])
            image_hash = sha256_file(image)
            # The pixel reader receives no expected values, packet data or timestamps.
            try:
                reading = observe(pixels, data["width"], data["height"], data["registration"])
                if "fields" not in reading:
                    reading = {"fields": {name: reading.get(name) for name in FIELDS},
                               "diagnostics": {k: v for k, v in reading.items() if k not in FIELDS}}
            except Exception as exc:
                reading = unresolved(f"reader failed: {type(exc).__name__}: {exc}")
            for sample in by_index[index]:
                requirement = encounter_expectation_at(data["timeline"], sample["capture_ns"],
                                                       configuration=config["settings"] if config else None)
                sample.update(image=f"frames/{image.name}", image_sha256=image_hash, observed=reading, expected=requirement)
                sample["comparison"] = compare_sample(requirement, reading, role=sample["role"])
            if number == 1 or number % 25 == 0 or number == total:
                print(f"Read {number}/{total} selected original frames", flush=True)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, ImportError) as exc:
        errors.append(f"{type(exc).__name__}: {exc}")
    except KeyboardInterrupt:
        errors.append("Analysis interrupted; unfinished required samples remain unresolved")
    for sample in samples:
        if "comparison" not in sample:
            reason = sample.get("selection_error") or "analysis did not reach this required sample"
            sample["comparison"] = {"checks": {f: {"status": "UNRESOLVED", "reason": reason} for f in FIELDS}}
    verdict, counts = summarize(samples, errors)
    selected_unique = len({s["video_frame_index"] for s in samples if "video_frame_index" in s})
    unique = len({s["video_frame_index"] for s in samples if "observed" in s})
    coverage = observation_coverage(samples, ranges or [], data, all_frames) if data else []
    spans, changes = observation_history(samples)
    from encounter_sequence import interpret_sequence
    origin = data["stimulus"][0]["requestedHostMonotonicNs"] if data else 0
    sequence = interpret_sequence(samples, data["timeline"], data.get("source_records", data["rows"]),
                                  configuration=config["settings"] if config else None,
                                  ranges=[(origin + round(a * 1e9), origin + round(b * 1e9)) for a, b in ranges or []]) if data else {}
    errors.extend("Sequence interpretation: " + reason for reason in sequence.get("errors", []))
    verdict, counts = summarize(samples, errors)
    result.update(result=verdict, counts=counts, evidence=evidence, errors=errors, samples=samples,
                  sequence=sequence,
                  observed_state_spans=spans, observed_changes=changes,
                  coverage=dict(requests=len(samples), selected_unique_frames=selected_unique,
                                unique_frames=unique, regions=coverage,
                                transition_windows=observation_coverage(samples, transition_windows, data, True) if data else []))
    save_json(out / "result.json", result)
    result = read_json(out / "result.json")  # Render only the same sanitized data that was retained.
    write_report(out, result)
    return result


def review_payload(result):
    """Keep the viewer responsive while full measurements stay in result.json."""
    payload = {key: result[key] for key in ("result", "selection_mode", "counts", "coverage",
               "observed_state_spans", "observed_changes", "sequence") if key in result}
    payload["samples"] = []
    for sample in result["samples"]:
        item = {key: value for key, value in sample.items() if key not in ("observed", "expected", "comparison")}
        item["expected"] = {"fields": sample.get("expected", {}).get("fields", {})}
        item["comparison"] = {"checks": {name: {key: value for key, value in check.items()
                              if key in ("status", "reason")} for name, check in sample["comparison"]["checks"].items()},
                              "joint_state": sample["comparison"].get("joint_state")}
        fields = {}
        for name, reading in sample.get("observed", {}).get("fields", {}).items():
            if not isinstance(reading, dict):
                continue
            fields[name] = {key: reading[key] for key in ("state", "value", "reason", "direction_states",
                            "visible_directions", "color_qualification", "partial_cards") if key in reading}
            if "cards" in reading:
                fields[name]["cards"] = [{key: card[key] for key in ("slot", "band", "frequency", "direction",
                                       "bars", "bars_state", "text_visible") if key in card} for card in reading["cards"]]
        item["observed"] = {"fields": fields}
        payload["samples"].append(item)
    return payload


def event_title(event):
    rows = event.get("wire_rows", [])
    primary = next((row for row in rows if row.get("priority")), None)
    return (f"{primary['band']} {primary['frequency']} primary; {len(rows)} alert(s)"
            if primary else "No live radar alerts")


def write_report(out: Path, result: dict) -> None:
    counts = result["counts"]
    tally = ", ".join(f"{value} {key.lower().replace('_', ' ')}" for key, value in counts["fields"].items())
    config = result.get("evidence", {}).get("configuration")
    config_summary = ("Recorded display settings: " + json.dumps(config["settings"], sort_keys=True)
                      if config else "Display settings unresolved: " + result.get("evidence", {}).get(
                          "recorded_configuration", {}).get("reason", "independent configuration unavailable"))
    title = ("Encounter with consecutive transitions" if result["selection_mode"] == "input_transition_review" else
             "Consecutive-frame encounter" if result["selection_mode"] == "all_recorded_frames" else "Sampled encounter")
    lines = [f"# {title}: {result['result']}", "", result["comparison_basis"], "", config_summary, "",
             f"{tally or 'No evaluable samples'} / {counts['required']} required field checks.", "",
             f"{result['coverage']['requests']} requests, {result['coverage']['selected_unique_frames']} selected and "
             f"{result['coverage']['unique_frames']} decoded original frames with reader attempts. "
             "Unknowns remain in the denominator; duplicate frames are not independent trials.", "",
             "[Open the visual review](report.html). It shows each original image beside its input expectations and pixel readings.", "",
             "A FAIL identifies sampled input/display disagreement. It does not establish its cause or a timing violation. "
             "A PASS covers these samples and fields only. Reader accuracy requires separate validation.", "",
             "| Region (seconds, end exclusive) | Decoded / recorded frames | First–last selected | Largest gap, including boundaries | All recorded frames read |", "| --- | ---: | --- | ---: | --- |"]
    for region in result["coverage"]["regions"]:
        bounds = (f"{region['first_selected_offset_seconds']:.6f}–{region['last_selected_offset_seconds']:.6f}s"
                  if region['first_selected_offset_seconds'] is not None else "none")
        complete = region["complete_recorded_frame_coverage"]
        lines.append(f"| {region['start_seconds']:g}–{region['end_seconds']:g} | {region['unique_frames']} / {region['available_recorded_frames']} | {bounds} | {region['maximum_unobserved_gap_seconds']:.6f}s | {'yes' if complete else 'no' if complete is False else 'sampled'} |")
        if region["unrecorded_source_frames"]:
            lines += ["", f"{region['unrecorded_source_frames']} source frames in this region were not recorded; their content remains unknown.", ""]
    if result["selection_mode"] == "all_recorded_frames":
        lines += ["", "Every available recorded frame whose capture timestamp is inside each declared range is selected. "
                  "This does not observe between exposures or recover dropped camera frames. "
                  "Per-field spans in the visual review preserve every literal change and unreadable frame, without smoothing. "
                  "Their timestamps identify first/last readings only, not a qualified response time.", ""]
    if result["errors"]:
        lines += ["", "Analysis errors:", ""] + ["- " + e for e in result["errors"]]
    events = result.get("sequence", {}).get("events", [])
    if events:
        lines += ["", "## What happened during each input event", "",
                  "First correct means all seven fields and their checked joint state agreed in that original image. "
                  "Host-send intervals end at the first observed correct capture; they do not measure physical appearance "
                  "or DUT processing latency. A correct frame does not erase a later disagreement or unknown.", "",
                  "| Input event | First all-required correct image | After correct: differing / unresolved spans | Recorded event frames read |", "| --- | --- | ---: | ---: |"]
        for event in events:
            first = event["first_correct"]
            first_text = f"[{first['offset_seconds']:.6f}s]({first['image']})" if first and first.get("image") else "not observed"
            unknown = sum(bool(c.get("unresolved_fields")) or c["status"] in ("UNRESOLVED", "INPUT_UNRESOLVED", "INPUT_IN_PROGRESS", "UNREAD")
                          for c in event["changes_after_correct"])
            observed = event["coverage"]
            lines.append(f"| {event['event_id']}: {event_title(event)} | {first_text} | "
                         f"{len(event['not_correct_after_correct'])} / {unknown} | "
                         f"{observed['read_recorded_frames']} / {observed['available_recorded_frames']} |")
    lines += ["", "| Sample | Time | Role | Checks needing attention | Original |", "| --- | ---: | --- | --- | --- |"]
    for sample in result["samples"]:
        issues = [f"{f}: {c['status']}" for f, c in sample["comparison"]["checks"].items() if c["status"] != "MATCH"]
        joint = sample["comparison"].get("joint_state", {}).get("status")
        if joint and joint not in ("MATCH", "NOT_EVALUATED"):
            issues.append("joint display: " + joint)
        link = f"[frame]({sample['image']})" if "image" in sample else "unavailable"
        lines.append(f"| {sample['frame_id']} | {sample.get('offset_seconds',sample['requested_offset_seconds']):.3f}s | {sample['role']} | {'; '.join(issues) or 'all matched'} | {link} |")
    (out / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    payload = json.dumps(review_payload(result), ensure_ascii=True).replace("<", "\\u003c")
    page = r'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Recorded encounter review</title><style>
*{box-sizing:border-box}body{margin:0;background:#10151b;color:#e7edf4;font:15px system-ui,sans-serif}
header{padding:24px 28px;border-bottom:1px solid #35414d}h1{font-size:25px;margin:0 0 10px}p{line-height:1.5;color:#b7c4d1;max-width:1000px}
main{display:grid;grid-template-columns:240px 1fr;gap:22px;padding:22px}nav{max-height:78vh;overflow:auto}button,select{font:inherit;color:inherit;background:#1b2630;border:1px solid #425161;border-radius:6px;padding:8px;cursor:pointer}nav button{display:block;width:100%;text-align:left;margin:5px 0}button[aria-current=true]{border-color:#69c5ff;background:#1c394e}.toolbar{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:14px}img{width:100%;max-height:53vh;object-fit:contain;background:#000;border-radius:7px}table{width:100%;border-collapse:collapse;margin-top:18px}td,th{text-align:left;vertical-align:top;padding:10px;border-bottom:1px solid #35414d}th{color:#9dafbf}td{white-space:pre-wrap;overflow-wrap:anywhere}.MATCH{color:#8edfbe}.DIFFERENCE,.JOINT_DIFFERENCE{color:#ff9292}.UNRESOLVED,.CONDITIONAL{color:#f2cf83}.PREVIOUS_INPUT_STATE,.TRANSITION_DIFFERENCE{color:#bcb1ff}small{color:#9dafbf}.empty{padding:30px}a{color:#8dcfff}summary{cursor:pointer;margin:16px 0}#scrub{width:100%;margin:10px 0}#changes{max-height:250px;overflow:auto}#changes button{display:block;margin:6px 0;width:100%;text-align:left}.history{max-height:400px;overflow:auto}#changed{color:#e7edf4}@media(max-width:800px){main{display:block}nav{max-height:180px;margin-bottom:20px}table{font-size:12px}td,th{padding:6px}}
</style><header><h1 id="title"></h1><p id="summary"></p><p id="coverage"></p><p>Original camera observations against recorded host input. Frames cannot show what happened between exposures. Transition observations establish no response deadline; an input/display disagreement does not locate its cause. Automatic reader accuracy is qualified separately.</p></header>
<section id="eventReview" style="padding:20px 28px;border-bottom:1px solid #35414d"><h2>What happened</h2><label>Input event <select id="eventSelect"></select></label><p id="eventSummary"></p><p id="eventTiming"></p><p id="eventCoverage"></p><div id="eventLinks" class="toolbar"></div><details><summary>First correctly observed content by field</summary><div id="eventFields"></div></details><details><summary>Changes after the first correct image</summary><div id="eventAfter" class="history"></div></details></section>
<main><aside><label>Show <select id="filter"><option value="all">All samples</option><option value="attention">Needs attention</option><option value="held">Held samples</option><option value="transition">Transitions</option></select></label><nav id="samples"></nav></aside>
<section><div class="toolbar"><button id="prev">← Previous</button><button id="next">Next →</button><button id="play">Play consecutive frames</button><label>Playback <select id="speed"><option value="20">20× slower</option><option value="10">10× slower</option><option value="5">5× slower</option></select></label><strong id="sampleTitle"></strong><a id="original">Open original</a></div><label>Original frame <input id="scrub" type="range" min="0" max="0" step="1" value="0"></label><img id="frame" alt="Unmodified selected camera frame"><p id="detail"></p><p id="changed"></p><details open><summary>Observed changes — jump to the original frame</summary><div id="changes"></div></details><details><summary>Per-field observed spans — every brief state and unreadable frame retained</summary><p>Adjacent source frames with exactly the same literal reading are grouped for review only. A one-frame state is retained. Gaps break spans. First and last timestamps bound the readings; no value is carried across an unreadable frame, and these spans do not establish response latency.</p><label>Display field <select id="spanField"></select></label><div class="history"><table><thead><tr><th>First–last reading</th><th>Source frames</th><th>Count</th><th>Literal reading</th></tr></thead><tbody id="spans"></tbody></table></div></details><table><thead><tr><th>Display field</th><th>Permitted input state</th><th>Observed pixels</th><th>Judgment</th></tr></thead><tbody id="checks"></tbody></table><p id="joint"></p></section></main>
<script>const result=PAYLOAD;const all=result.samples;let selected=0,visible=[];const navButtons=new Map();
const el=id=>document.getElementById(id);const fmt=x=>x===undefined?'unavailable':JSON.stringify(x,null,2);const names={counter_glyph:'Counter / mode',primary_frequency:'Primary frequency',active_bands:'Active bands',main_arrows:'Main arrows',main_bars:'Main strength',secondary:'Secondary cards',muted_badge:'MUTED badge'};const literal=o=>o.state+': '+JSON.stringify(o.value)+(o.reason?' ('+o.reason+')':'');const seconds=x=>x===null?'none':x.toFixed(6)+' s';const changes=result.observed_changes||[];const events=result.sequence?.events||[];
el('title').textContent=(result.selection_mode==='input_transition_review'?'Encounter with consecutive transitions: ':result.selection_mode==='all_recorded_frames'?'Consecutive-frame encounter: ':'Sampled encounter: ')+result.result;el('summary').textContent=Object.entries(result.counts.fields).map(([k,v])=>v+' '+k.toLowerCase().replaceAll('_',' ')).join(' · ')+' / '+result.counts.required+' required checks. '+result.coverage.unique_frames+' unique original frames.';
el('coverage').textContent=result.coverage.regions.map(r=>r.start_seconds+'–'+r.end_seconds+' s (end exclusive): '+r.unique_frames+'/'+r.available_recorded_frames+' recorded frames read'+(r.complete_recorded_frame_coverage===null?' (sampled)':r.complete_recorded_frame_coverage?' (complete within range)':' (incomplete)')+'. Selected '+seconds(r.first_selected_offset_seconds)+' to '+seconds(r.last_selected_offset_seconds)+'. Read '+seconds(r.first_observed_offset_seconds)+' to '+seconds(r.last_observed_offset_seconds)+'. Largest gap including boundaries '+seconds(r.maximum_unobserved_gap_seconds)+'. '+r.unrecorded_source_frames+' source frames not recorded.').join(' ');

const eventName=e=>{const p=e.wire_rows.find(r=>r.priority);return p?p.band+' '+p.frequency+' primary · '+e.wire_rows.length+' alerts':'No live radar alerts'};
const jump=(parent,point,label)=>{if(!point)return;let b=document.createElement('button');b.textContent=label+' · '+seconds(point.offset_seconds);b.onclick=()=>show(all.findIndex(s=>s.frame_id===point.frame_id));parent.append(b)};
for(let i=0;i<events.length;i++){let option=document.createElement('option');option.value=i;option.textContent=events[i].event_id+' · '+eventName(events[i]);el('eventSelect').append(option)}
function renderEvent(){const e=events[Number(el('eventSelect').value)];if(!e){el('eventReview').hidden=true;return}el('eventSummary').textContent=e.summary;const t=e.timing;el('eventTiming').textContent=t.status==='observed_capture_marker'?'The first correct recorded image was '+t.host_send_to_first_correct_capture_ms.map(x=>x.toFixed(3)).join('–')+' ms after the completing notification send call. '+(t.complete_recorded_frame_prefix?'Every recorded image from the event request through that image was read. ':'Earlier recorded images were not all read; this is a sampled observation time. ')+t.physical_appearance_reason:'First-correct timing unavailable: '+t.reason;const c=e.coverage;el('eventCoverage').textContent=c.read_recorded_frames+'/'+c.available_recorded_frames+' recorded images read across this entire input event; '+c.unrecorded_source_frames+' source frames not recorded; largest gap '+c.maximum_gap_between_read_markers_ms.toFixed(3)+' ms. A correct image does not establish correctness through an unobserved gap.';el('eventLinks').replaceChildren();jump(el('eventLinks'),e.preceding_observation,'Before input');jump(el('eventLinks'),e.last_definite_not_correct_before_first,'Last observed different state before correct');jump(el('eventLinks'),e.first_correct,'First all-required correct');el('eventFields').replaceChildren();for(const [name,point]of Object.entries(e.first_correct_by_field)){if(point)jump(el('eventFields'),point,names[name]);else{const p=document.createElement('p');p.textContent=names[name]+': no supported correct reading';el('eventFields').append(p)}}el('eventAfter').replaceChildren();for(const change of e.changes_after_correct){const detail=e.observation_spans[change.span_index];let label=change.status.replaceAll('_',' ').toLowerCase();if(change.not_correct_fields.length)label+=' · differing '+change.not_correct_fields.map(n=>names[n]).join(', ');if(change.unresolved_fields.length)label+=' · unresolved '+change.unresolved_fields.map(n=>names[n]).join(', ');label+=' · '+detail.frame_count+' consecutive frame(s)';jump(el('eventAfter'),change.first,label)}if(!e.changes_after_correct.length)el('eventAfter').textContent='No later change was observed in the selected images.'}
el('eventSelect').onchange=()=>{stopPlayback();renderEvent();const e=events[Number(el('eventSelect').value)];const point=e?.observation_spans[0]?.first;if(point)show(all.findIndex(s=>s.frame_id===point.frame_id))};renderEvent();

let playback=null;function stopPlayback(){if(playback!==null)clearTimeout(playback);playback=null;el('play').textContent='Play consecutive frames'}
function playNext(){const here=all[selected];let nextIndex=selected+1;while(nextIndex<all.length&&all[nextIndex].video_frame_index===here.video_frame_index)nextIndex++;const next=all[nextIndex];if(!next||next.video_frame_index!==here.video_frame_index+1||next.source_frame_seq!==here.source_frame_seq+1){stopPlayback();return}const delay=(next.capture_ns-here.capture_ns)/1e6*Number(el('speed').value);playback=setTimeout(()=>{show(nextIndex,true);playNext()},delay)}
el('play').onclick=()=>{if(playback!==null){stopPlayback();return}el('play').textContent='Pause';playNext()};el('speed').onchange=stopPlayback;

function readingText(name,o){if(!o)return'not read';let text=o.state+': '+fmt(o.value)+(o.reason?'\n'+o.reason:'');if(name==='main_arrows'&&o.direction_states)text+='\n'+Object.entries(o.direction_states).map(([n,d])=>n+': '+d.state+' ('+d.color+')').join('\n');if(name==='secondary'&&o.cards)text+='\n'+o.cards.map(c=>'Card '+(c.slot+1)+': '+(c.band&&c.frequency?c.band+' '+c.frequency:'text unresolved')+', '+(c.direction||'direction unresolved')+', '+(c.bars_state==='readable'?c.bars+' bars':'bars '+c.bars_state)).join('\n');return text}
if(result.coverage.transition_windows?.length){const windows=result.coverage.transition_windows;el('coverage').textContent+=' Consecutive input-change windows: '+windows.filter(w=>w.complete_recorded_frame_coverage).length+'/'+windows.length+' had every recorded frame read. Each window is a declared inspection interval, not a response deadline.'}

el('scrub').max=Math.max(0,all.length-1);el('scrub').oninput=e=>show(Number(e.target.value));
for(const c of changes){let b=document.createElement('button');b.textContent=seconds(c.offset_seconds)+' · '+(c.initial?'Initial reading':!c.consecutive_with_previous?'After unobserved gap':'Changed')+': '+(Object.keys(c.fields).map(n=>names[n]).join(', ')||'same literal fields');b.onclick=()=>show(all.findIndex(s=>s.frame_id===c.frame_id));el('changes').append(b)}
for(const [key,label]of Object.entries(names)){let option=document.createElement('option');option.value=key;option.textContent=label;el('spanField').append(option)}
function renderSpans(){el('spans').replaceChildren();for(const span of result.observed_state_spans?.[el('spanField').value]||[]){let row=document.createElement('tr');let first=document.createElement('td');let button=document.createElement('button');button.textContent=seconds(span.first.offset_seconds)+' – '+seconds(span.last.offset_seconds);button.onclick=()=>show(all.findIndex(s=>s.frame_id===span.first.frame_id));first.append(button);row.append(first);for(const value of [span.first.source_frame_seq+'–'+span.last.source_frame_seq,span.frame_count,literal(span.observed)]){let cell=document.createElement('td');cell.textContent=value;row.append(cell)}el('spans').append(row)}}el('spanField').onchange=renderSpans;renderSpans();
function needsAttention(s){let joint=s.comparison.joint_state?.status;return Object.values(s.comparison.checks).some(c=>c.status!=='MATCH')||(joint&&!['MATCH','NOT_EVALUATED'].includes(joint))}
function renderList(){let f=el('filter').value;visible=all.map((s,i)=>[s,i]).filter(([s])=>f==='all'||s.role===f||(f==='attention'&&needsAttention(s)));el('samples').replaceChildren();navButtons.clear();for(const [s,i]of visible){let b=document.createElement('button');b.textContent=(s.offset_seconds??s.requested_offset_seconds).toFixed(3)+' s · '+s.role;b.onclick=()=>show(i);b.setAttribute('aria-current',i===selected);el('samples').append(b);navButtons.set(i,b)}}
function show(i,playing=false){if(!all.length)return;if(!playing)stopPlayback();const previousSelected=selected;selected=Math.max(0,Math.min(i,all.length-1));let s=all[selected];const eventIndex=events.findIndex(e=>e.start_ns<=s.capture_ns&&s.capture_ns<e.end_ns);if(eventIndex>=0&&Number(el('eventSelect').value)!==eventIndex){el('eventSelect').value=eventIndex;renderEvent()}el('scrub').value=selected;el('sampleTitle').textContent='Sample '+s.frame_id+' · '+(s.offset_seconds??s.requested_offset_seconds).toFixed(6)+' s';if(s.image){el('frame').src=s.image;el('frame').hidden=false;el('original').href=s.image}else{el('frame').hidden=true;el('original').removeAttribute('href')}el('detail').textContent=s.role+'; '+s.selection_reasons.join(', ')+'. Source frame '+(s.source_frame_seq??'unavailable')+'.';let change=changes.find(c=>c.video_frame_index===s.video_frame_index);el('changed').textContent=s.video_frame_index===undefined?'No recorded image was available for this requested observation.':change?(change.initial?'Initial reading. ':!change.consecutive_with_previous?'Unobserved gap before this frame. ':'Changed on this frame. ')+Object.entries(change.fields).map(([n,o])=>names[n]+' '+literal(o)).join(' · '):'Same literal readings as the preceding observed source frame.';el('checks').replaceChildren();for(const [name,c]of Object.entries(s.comparison.checks)){let row=document.createElement('tr');let expected=s.expected?.fields?.[name];let observed=s.observed?.fields?.[name];let values=[names[name]||name.replaceAll('_',' '),expected?.unresolved??fmt(expected?.allowed),readingText(name,observed),c.status+(c.reason?'\n'+c.reason:'')];for(let n=0;n<4;n++){let cell=document.createElement('td');cell.textContent=values[n];if(n===3)cell.className=c.status;row.append(cell)}el('checks').append(row)}el('joint').textContent=s.comparison.joint_state?'Coherent display state: '+fmt(s.comparison.joint_state):'';navButtons.get(previousSelected)?.setAttribute('aria-current',false);navButtons.get(selected)?.setAttribute('aria-current',true)}
el('filter').onchange=renderList;el('prev').onclick=()=>{let p=visible.findIndex(([,i])=>i===selected);if(p>0)show(visible[p-1][1])};el('next').onclick=()=>{let p=visible.findIndex(([,i])=>i===selected);if(p+1<visible.length)show(visible[p+1][1])};document.addEventListener('keydown',e=>{if(e.key==='ArrowLeft')el('prev').click();if(e.key==='ArrowRight')el('next').click()});function openLinkedSample(){let id=new URLSearchParams(location.hash.slice(1)).get('sample');let index=all.findIndex(s=>s.frame_id===id);show(index<0?0:index)}window.addEventListener('hashchange',openLinkedSample);openLinkedSample();renderList();</script></html>'''
    (out / "report.html").write_text(page.replace("PAYLOAD", payload), encoding="utf-8")


def parse_range(value: str) -> tuple[float, float]:
    try:
        start, end = map(float, value.split(":"))
        require(math.isfinite(start) and math.isfinite(end) and 0 <= start < end, "invalid range")
        return start, end
    except ValueError as exc:
        raise argparse.ArgumentTypeError("use nonnegative START:END seconds, with START < END") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True, help="retained replay directory with window_result.json")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--range", type=parse_range, action="append", dest="ranges", help="START:END seconds from first replay request; repeat for separate regions")
    selection.add_argument("--transition-window", type=parse_range, action="append", help="inspect START:END as transition observations; differences impose no response deadline")
    parser.add_argument("--cadence", type=float, default=2, help="regular sample interval in seconds (default 2), in addition to packet-state midpoints and edge probes")
    parser.add_argument("--all-frames", action="store_true", help="select every recorded source frame in explicit --range or --transition-window bounds; cadence is not used; at most 5000 frames")
    parser.add_argument("--inspect-transitions", action="store_true", help="retain held samples and inspect every recorded frame from 50 ms before through 500 ms after each input change; at most 20000 observations; this is not a response deadline")
    parser.add_argument("--configuration", type=Path, help="independently verified, exact-window display settings; missing settings stay unknown")
    parser.add_argument("--out", type=Path, required=True, help="new result directory; existing results are never replaced")
    args = parser.parse_args()
    if args.all_frames and not (args.ranges or args.transition_window):
        parser.error("--all-frames requires explicit --range or --transition-window bounds")
    if args.all_frames and args.cadence != 2:
        parser.error("--cadence cannot be combined with --all-frames")
    if args.inspect_transitions and (args.all_frames or args.transition_window):
        parser.error("--inspect-transitions cannot be combined with --all-frames or --transition-window")
    try:
        args.out.mkdir(parents=True, exist_ok=False)
        result = analyze(args.run_dir, args.out, args.transition_window or args.ranges, args.cadence,
                         args.configuration, transition_only=bool(args.transition_window), all_frames=args.all_frames,
                         inspect_transitions=args.inspect_transitions)
    except (OSError, ValueError) as exc:
        print(sanitize_artifact_value(str(exc), run_dir=args.out), file=sys.stderr)
        return 2
    label = "encounter with consecutive transitions" if args.inspect_transitions else "consecutive-frame encounter" if args.all_frames else "sampled encounter"
    print(f"{result['result']} — {label}: {result['counts']['fields']} / {result['counts']['required']} required checks")
    print("Open report.html for original images, expected states, pixel readings and unresolved checks.")
    return {"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 2}[result["result"]]


if __name__ == "__main__":
    raise SystemExit(main())
