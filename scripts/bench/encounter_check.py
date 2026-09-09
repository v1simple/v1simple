#!/usr/bin/env python3
"""Evaluate visible V1 encounters from an existing recording.

No hardware is operated. --observe-behavior compares recorded display events
with controlled inputs. Other modes retain sampled or consecutive frame
comparisons for diagnosis. No appearance deadline is imposed.
Selection is frozen before pixels are read. The clock binds host acceptance
and camera markers; it does not measure DUT receipt or locate a firmware defect.
"""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import nullcontext
from copy import deepcopy
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
import encounter_redraw_probe

FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")
PROBES = (-.05, .05, .15, .35)
MAX_SAMPLES = 5000


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
    startup = preflight.get("source_still")
    if startup is not None:
        require(isinstance(startup, dict) and any(
                    entry.get("path") == startup.get("name") and
                    entry.get("sha256") == startup.get("sha256")
                    for entry in entries.values()),
                "startup calibration image is not bound to the recording")
        from encounter_frequency_idle import registration_for_camera
        registration = registration_for_camera(preflight, camera / startup["name"])
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
        "camera": {"name": manifest["identity"]["camera"].get("name"),
                   "profile": manifest["identity"]["camera"].get("profile")},
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
                registration=registration, recorded_configuration=recorded_configuration,
                reader_capabilities_at_capture=preflight.get("reader_capabilities"),
                camera_name=manifest["identity"]["camera"].get("name"),
                camera_profile=manifest["identity"]["camera"].get("profile"))


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
    camera_start = (rows[0]["host_capture_ns"] - origin) / 1e9
    camera_end = (rows[-1]["host_capture_ns"] + rows[-1]["duration_ns"] - origin) / 1e9
    targets: dict[int, set[str]] = {}

    def add(offset, reason):
        ns = origin + round(offset * 1e9)
        targets.setdefault(ns, set()).add(reason)
        require(len(targets) <= MAX_SAMPLES, "selection exceeds 5000 samples; narrow the range or cadence")

    for start, end in ranges:
        require(all(math.isfinite(x) for x in (start, end)) and
                start < end <= camera_end and end > camera_start,
                "range must be finite, increasing, and overlap the recorded camera window")
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


def _select_all_frames_in_bounds(origin: int, rows: list[dict], bounds: list[tuple[int, int]],
                                 limit: int) -> list[dict]:
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


def select_all_frames(stimulus: list[dict], rows: list[dict],
                      ranges: list[tuple[float, float]], limit: int | None = None) -> list[dict]:
    """Select each recorded source image once by capture time, without cadence targets."""
    require(bool(stimulus) and bool(rows), "no recorded stimulus or camera frames")
    require(bool(ranges), "all-frame selection requires an explicit bounded range")
    limit = MAX_SAMPLES if limit is None else limit
    origin = stimulus[0]["requestedHostMonotonicNs"]
    camera_start = (rows[0]["host_capture_ns"] - origin) / 1e9
    camera_end = (rows[-1]["host_capture_ns"] + rows[-1]["duration_ns"] - origin) / 1e9
    bounds = []
    for start, end in ranges:
        require(all(math.isfinite(x) for x in (start, end)) and
                start < end <= camera_end and end > camera_start,
                "range must be finite, increasing, and overlap the recorded camera window")
        bounds.append((origin + round(max(start, camera_start) * 1e9),
                       origin + round(end * 1e9)))
    return _select_all_frames_in_bounds(origin, rows, bounds, limit)


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
    if len(indices) > 1 and indices[-1] - indices[0] + 1 == len(indices):
        return f"between(n,{indices[0]},{indices[-1]})"
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
    settings = document.get("settings")
    require(isinstance(settings, dict) and bool(settings),
            "configuration settings must be a nonempty object")
    allowed = {"stealthEnabled", "priorityArrowOnly", "alertPersistenceSeconds"}
    require(set(settings) <= allowed, "configuration contains an unsupported setting")
    for name in ("stealthEnabled", "priorityArrowOnly"):
        require(name not in settings or type(settings[name]) is bool,
                f"configuration {name} must be boolean")
    require("alertPersistenceSeconds" not in settings or
            (type(settings["alertPersistenceSeconds"]) is int and
             0 <= settings["alertPersistenceSeconds"] <= 5),
            "configuration alertPersistenceSeconds must be an integer from 0 through 5")
    return document


def full_camera_range(stimulus: list[dict], rows: list[dict]) -> list[tuple[float, float]]:
    """Return the complete recorded interval relative to the first request.

    The recording deliberately continues after the final request.  That tail
    contains the final event's appearance and verification window and therefore
    belongs to the default product scope.
    """
    require(bool(stimulus) and bool(rows), "default range has no input or camera records")
    origin = stimulus[0]["requestedHostMonotonicNs"]
    start_ns = rows[0]["host_capture_ns"]
    end_ns = rows[-1]["host_capture_ns"] + rows[-1]["duration_ns"]
    require(type(origin) is int and type(start_ns) is int and
            type(end_ns) is int and start_ns < end_ns and end_ns > origin,
            "default camera range is invalid")
    return [((start_ns - origin) / 1e9, (end_ns - origin) / 1e9)]


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
            configuration: Path | None = None, transition_only: bool = False,
            all_frames: bool = False) -> dict:
    samples, errors, evidence, data, config = [], [], {}, None, None
    method = {p.name: sha256_file(p) for p in Path(__file__).parent.glob("*.py")}
    for p in Path(__file__).parent.glob("encounter_*.swift"):
        method[p.name] = sha256_file(p)
    for p in Path(__file__).parent.glob("encounter_*.png"):
        method[p.name] = sha256_file(p)
    for p in Path(__file__).parent.glob("encounter_*.b64"):
        method[p.name] = sha256_file(p)
    (out / "method").mkdir()
    for name, digest in method.items():
        source = Path(__file__).with_name(name)
        shutil.copyfile(source, out / "method" / name)
        require(sha256_file(out / "method" / name) == digest, "analysis source changed while being retained")
    result = dict(schema_version=1, kind="sampled_encounter_check", full_run_correctness="not_evaluated",
                  selection_mode="all_recorded_frames" if all_frames else "transition_windows" if transition_only else "encounter_samples",
                  response_deadline="not_evaluated", reader_qualification="see independent validation; not established by this run",
                  comparison_basis=("Consecutive recorded-frame" if all_frames else "Sampled") +
                  " display agreement with recorded host input. Host acceptance is not DUT receipt. Transition observations impose no response deadline.",
                  implementation_sha256=method, environment=dict(python=platform.python_version(), system=platform.platform()))
    try:
        data = load_run(run)
        evidence = {**data["identity"], "video_timing": data["timing"],
                    "primary_frequency_calibration": data["registration"].get("primary_frequency_calibration"),
                    "reader_capabilities_at_capture": data.get("reader_capabilities_at_capture"),
                    "timing_basis": "hash-bound original capture verification and validated source sidecar"}
        origin = data["stimulus"][0]["requestedHostMonotonicNs"]
        require(not all_frames or ranges is not None, "all-frame selection requires an explicit bounded range")
        if ranges is None:
            ranges = full_camera_range(data["stimulus"], data["rows"])
        samples = (select_all_frames(data["stimulus"], data["rows"], ranges) if all_frames else
                   select_samples(data["stimulus"], data["rows"], ranges, cadence))
        if transition_only:
            for sample in samples:
                sample["role"] = "transition"
        recorded_config = configuration_for_samples(
            data.get("recorded_configuration"), samples)
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
                                               samples=samples))
        evidence["selection_manifest_sha256"] = sha256_file(out / "selection.json")
        by_index = {}
        for sample in samples:
            if "video_frame_index" in sample:
                by_index.setdefault(sample["video_frame_index"], []).append(sample)
        (out / "frames").mkdir()
        import encounter_reader
        observe = encounter_reader.observe
        if hasattr(encounter_reader, "prepare_reader"):
            reader_cache = out / "reader-cache"
            evidence["reader"] = encounter_reader.prepare_reader(reader_cache)
            from encounter_runtime_probe import probe_ocr_runtime
            probe = probe_ocr_runtime(evidence["reader"])
            evidence["reader"]["ocr_compiled"] = evidence["reader"].get("ocr_available") is True
            evidence["reader"]["ocr_runtime_probe"] = probe
            evidence["reader"]["ocr_available"] = (
                evidence["reader"]["ocr_compiled"] and probe.get("status") == "operational")
        total = len(by_index)
        with getattr(encounter_reader, "analysis_session", nullcontext)():
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
                # Fixed, expectation-blind spatial measurements are retained
                # separately from the single-frame field decisions. A probe
                # failure cannot downgrade or replace the ordinary reader result.
                try:
                    reading["redraw_profiles"] = encounter_redraw_probe.observe(
                        pixels, data["width"], data["height"], data["registration"])
                except Exception as exc:
                    reading["redraw_profiles"] = {
                        "schema_version": 1,
                        "method_version": encounter_redraw_probe.METHOD_VERSION,
                        "status": "unavailable",
                        "reason": f"redraw probe failed: {type(exc).__name__}: {exc}",
                    }
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
    raw_verdict = verdict
    from encounter_assessment import assess, event_findings
    result.update(result=verdict, raw_frame_result=raw_verdict, counts=counts,
                  evidence=evidence, errors=errors, samples=samples,
                  sequence=sequence, assessment=assess(samples, errors, sequence),
                  event_findings=event_findings(sequence, None),
                  observed_state_spans=spans, observed_changes=changes,
                  coverage=dict(requests=len(samples), selected_unique_frames=selected_unique,
                                unique_frames=unique, regions=coverage))
    save_json(out / "result.json", result)
    result = read_json(out / "result.json")  # Render only the same sanitized data that was retained.
    write_report(out, result)
    return result


def review_payload(result):
    """Keep the viewer responsive while full measurements stay in result.json."""
    payload = {key: result[key] for key in ("result", "raw_frame_result", "selection_mode", "counts",
               "coverage", "reader_qualification", "product_adapter", "observed_state_spans",
               "observed_changes", "sequence", "assessment", "event_findings") if key in result}
    evidence = result.get("evidence", {})
    identity = evidence.get("runtime_identity") or {}
    payload["recorded_firmware"] = {key: identity.get(key) for key in ("git_sha", "image_id", "boot_id")}
    payload["reader_method_version"] = evidence.get("reader", {}).get("method_version")

    def compact_judgment(judgment):
        if not isinstance(judgment, dict):
            return None
        compact = {key: deepcopy(judgment[key]) for key in
                   ("schema_version", "kind", "contract", "execution", "counts", "result", "reason_code")
                   if key in judgment}
        compact["events"] = [{key: deepcopy(event[key]) for key in
                              ("event_id", "mode", "result", "reason_code", "reasons",
                               "deadline_ns", "verification_end_ns", "observed_joint_state_ids",
                               "response_acquisition", "acquisition_observations", "first_current_correct",
                               "verification_closure_proof", "closing_current_correct",
                               "first_decisive_marker")
                              if key in event}
                             for event in judgment.get("events", []) if isinstance(event, dict)]
        return compact

    payload["product_candidate_judgment"] = compact_judgment(result.get("product_candidate_judgment"))
    payload["primary_judgment"] = compact_judgment(result.get("primary_judgment"))
    temporal = result.get("temporal_classification", {})
    payload["temporal_classification"] = {
        "schema_version": temporal.get("schema_version"),
        "classification_count": len(temporal.get("classifications", [])),
        "rejected_run_count": len(temporal.get("rejected_runs", [])),
        "errors": temporal.get("errors", []),
    }
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
                            "visible_directions", "color_qualification", "partial_cards",
                            "sampled_illumination") if key in reading}
            if "cards" in reading:
                fields[name]["cards"] = [{key: card[key] for key in ("slot", "band", "frequency", "direction",
                                       "bars", "compatible_bars", "bars_state", "text_visible") if key in card}
                                          for card in reading["cards"]]
        item["observed"] = {"fields": fields}
        payload["samples"].append(item)
    return payload


def event_title(event):
    rows = event.get("wire_rows", [])
    primary = next((row for row in rows if row.get("priority")), None)
    return (f"{primary['band']} {primary['frequency']} primary; {len(rows)} alert(s)"
            if primary else "No live radar alerts")


def finding_point(point, anchor):
    if not point:
        return "not observed"
    label = (f"+{(point['capture_ns'] - anchor) / 1e6:.3f} ms" if anchor is not None
             else f"frame {point['frame_id']}")
    return f"[{label}](report.html#sample={point['frame_id']})"


def write_report(out: Path, result: dict) -> None:
    counts = result["counts"]
    tally = ", ".join(f"{value} {key.lower().replace('_', ' ')}" for key, value in counts["fields"].items())
    config = result.get("evidence", {}).get("configuration")
    config_summary = ("Recorded display settings: " + json.dumps(config["settings"], sort_keys=True)
                      if config else "Display settings unresolved: " + result.get("evidence", {}).get(
                          "recorded_configuration", {}).get("reason", "independent configuration unavailable"))
    title = ("Visible encounter product" if result.get("primary_judgment") else
             "Encounter with consecutive transitions" if result["selection_mode"] == "input_transition_review" else
             "Consecutive-frame encounter" if result["selection_mode"] == "all_recorded_frames" else "Sampled encounter")
    lines = [f"# {title}: {result['result']}", ""]
    identity = result.get("evidence", {}).get("runtime_identity") or {}
    lines += [f"Recorded firmware: commit `{identity.get('git_sha', 'unavailable')}`, "
              f"image `{identity.get('image_id', 'unavailable')}`, boot `{identity.get('boot_id', 'unavailable')}`. "
              "These findings describe that recording.", ""]
    findings = result.get("event_findings", [])
    if findings:
        lines += ["## Observed display information", "",
                  "These are literal observations after complete host input acceptance. Differences during "
                  "acquisition are not automatically firmware faults. Times identify recorded camera markers; "
                  "DUT receipt and execution time are not measured. Unreadable fields remain separate, even "
                  "when other fields demonstrate a difference. Coverage is limited to the selected originals.", "",
                  "| Input event | First complete target observed | Latest different information | Unanswered fields |",
                  "| --- | --- | --- | --- |"]
        for finding in findings:
            anchor = finding["input_anchor_ns"]
            different = "; ".join(
                f"{item['field'].replace('_', ' ')} {finding_point(item['last'], anchor)}"
                for item in finding["differences"]) or "none observed"
            unknown = "; ".join(f"{item['field'].replace('_', ' ')}: {item['frames']} frames"
                                for item in finding["unresolved"]) or "none in these observations"
            lines.append(f"| {finding['event_id']} | {finding_point(finding['first_correct'], anchor)} | "
                         f"{different} | {unknown} |")
    # Retain display compatibility for historical result documents; no grading runs here.
    primary = result.get("primary_judgment")
    if primary:
        product_counts = primary.get("counts", {})
        contract = primary.get("contract", {})
        qualification = result.get("reader_qualification", {})
        lines += [f"Contract `{contract.get('id', 'unavailable')}/v{contract.get('version', '?')}`: "
                  f"{product_counts.get('passed', 0)} passed, {product_counts.get('failed', 0)} failed and "
                  f"{product_counts.get('inconclusive', 0)} inconclusive across "
                  f"{product_counts.get('required_events', 0)} required visible events.", "",
                  f"Reason: `{primary.get('reason_code', 'unavailable')}`. Reader qualification: "
                  f"**{qualification.get('status', 'REJECTED')}**.", ""]
        fatal = primary.get("execution", {}).get("fatal_integrity_errors", [])
        if fatal:
            lines += ["Evidence blockers:", ""] + ["- " + reason for reason in fatal] + [""]
        candidate = result.get("product_candidate_judgment")
        if qualification.get("status") != "QUALIFIED" and isinstance(candidate, dict):
            lines += [f"The unqualified diagnostic candidate was **{candidate.get('result')}** "
                      f"(`{candidate.get('reason_code')}`). It is retained for development and is not the product verdict.", ""]
        if primary.get("events"):
            lines += ["| Required visible event | Result | Reason | Original evidence |", "| --- | --- | --- | --- |"]
            for event in primary["events"]:
                points = [("Decisive image", event.get("first_decisive_marker")),
                          ("Deadline", (event.get("response_acquisition") or {}).get(
                              "deadline_capture_marker_bracket", {}).get("end")),
                          ("Verification boundary", (event.get("verification_closure_proof") or {}).get(
                              "verification_boundary")),
                          ("Closing observation", event.get("closing_current_correct"))]
                links = [f"[{label}](report.html#sample={point['frame_id']})"
                         for label, point in points if point and point.get("frame_id")]
                lines.append(f"| {event.get('event_id', 'unavailable')} | **{event.get('result', 'INCONCLUSIVE')}** | "
                             f"`{event.get('reason_code', 'unavailable')}` | {'; '.join(links)} |")
            lines.append("")
        pass_scope = (
            "A `PASS` means every required target was correct or in a qualified legal display phase "
            "at the sole bounded deadline observation, then remained raw-current or in a qualified legal "
            "display phase through 192 ms and ended with either a timely raw-current observation or an "
            "exactly qualified unresolved boundary run immediately bracketed by raw-current observations. "
            "Pre-deadline optical observations remain diagnostic. "
            if contract.get("version", 0) >= 3 else
            "A `PASS` means every required event met its declared acquisition and continued-presentation contract. ")
        lines += [pass_scope + "A `FAIL` means trusted evidence established a specific violation in that scope. "
                  "`INCONCLUSIVE` means the testing product did not deliver a verdict for all required observations.",
                  "", "## Raw reader evidence", ""]
    lines += [result["comparison_basis"], "", config_summary, "",
             f"{tally or 'No evaluable samples'} / {counts['required']} required field checks.", "",
             f"{result['coverage']['requests']} requests, {result['coverage']['selected_unique_frames']} selected and "
             f"{result['coverage']['unique_frames']} decoded original frames with reader attempts. "
             "Unknowns remain in the denominator; duplicate frames are not independent trials.", "",
             "[Open the visual review](report.html). It shows each original image beside its input expectations and pixel readings.", "",
             "The raw result is **" + result.get("raw_frame_result", result["result"]) + "**. Raw differences and "
             "reader refusals remain visible here even when the qualified product layer can classify a physical "
             "display transition.", ""]
    assessment = result.get("assessment")
    if assessment:
        held, response = assessment["held"], assessment["event_response"]
        lines += ["## What can be judged", "",
                  f"Held observations: **{held['status']}** across {held['required']} required field checks.", "",
                  f"Event targets observed: **{response['status']}** — {response['observed']} / {response['required']}.", "",
                  "An observed target proves that the content appeared in an analyzed image before the event ended. "
                  "It does not establish timely response or continued correctness. Transition differences and unknowns "
                  "remain below. These separate answers do not replace the aggregate verdict or establish tool acceptance.", ""]
        for issue in held["issues"]:
            point = issue["first"]
            link = f"[sample {point['frame_id']}](report.html#sample={point['frame_id']})"
            lines.append(f"- {issue['field']}: {issue['reason']} ({len(issue['frame_ids'])} held observation(s); first {link}).")
        lines.append("")
    lines += ["| Region (seconds, end exclusive) | Decoded / recorded frames | First–last selected | Largest gap, including boundaries | All recorded frames read |", "| --- | ---: | --- | ---: | --- |"]
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
</style><header><h1 id="title"></h1><p id="summary"></p><p id="coverage"></p><p id="recordedFirmware"></p><section id="findings" hidden><h2>Observed display information</h2><p>Each row separates the target being seen, definite different information, and reader uncertainty. A difference during acquisition is an observation, not automatically a firmware fault. Times are from complete host input acceptance to recorded camera markers; DUT receipt is not measured. Click a witness to see the original and its expected and observed fields.</p><div class="history"><table><thead><tr><th>Input event</th><th>First complete target</th><th>Latest different information</th><th>Unanswered fields</th></tr></thead><tbody id="findingRows"></tbody></table></div></section><p>Original camera observations against recorded host input. Frames cannot show what happened between exposures. The visible-event product verdict requires exact reader qualification; missing or stale qualification is a product failure and appears as INCONCLUSIVE.</p><section id="assessment" hidden><h2>What can be judged</h2><p id="heldAssessment"></p><p id="responseAssessment"></p><p id="afterAssessment"></p><div id="heldIssues" class="toolbar"></div><p>These are separate evidence claims. A target appearing once does not establish timely response, continued correctness or tool acceptance. The aggregate verdict and all transition observations are retained.</p></section></header>
<section id="eventReview" style="padding:20px 28px;border-bottom:1px solid #35414d"><h2>What happened</h2><label>Input event <select id="eventSelect"></select></label><p id="eventJudgment"></p><div id="productEventLinks" class="toolbar"></div><p id="eventSummary"></p><p id="eventTiming"></p><p id="eventCoverage"></p><div id="eventLinks" class="toolbar"></div><details><summary>First correctly observed content by field</summary><div id="eventFields"></div></details><details><summary>Changes after the first correct image</summary><div id="eventAfter" class="history"></div></details></section>
<main><aside><label>Show <select id="filter"><option value="all">All samples</option><option value="attention">Needs attention</option><option value="held">Held samples</option><option value="transition">Transitions</option></select></label><nav id="samples"></nav></aside>
<section><div class="toolbar"><button id="prev">← Previous</button><button id="next">Next →</button><button id="play">Play consecutive frames</button><label>Playback <select id="speed"><option value="20">20× slower</option><option value="10">10× slower</option><option value="5">5× slower</option></select></label><strong id="sampleTitle"></strong><a id="original">Open original</a></div><label>Original frame <input id="scrub" type="range" min="0" max="0" step="1" value="0"></label><img id="frame" alt="Unmodified selected camera frame"><p id="detail"></p><p id="changed"></p><details open><summary>Observed changes — jump to the original frame</summary><div id="changes"></div></details><details><summary>Per-field observed spans — every brief state and unreadable frame retained</summary><p>Adjacent source frames with exactly the same literal reading are grouped for review only. A one-frame state is retained. Gaps break spans. First and last timestamps bound the readings; no value is carried across an unreadable frame, and these spans do not establish response latency.</p><label>Display field <select id="spanField"></select></label><div class="history"><table><thead><tr><th>First–last reading</th><th>Source frames</th><th>Count</th><th>Literal reading</th></tr></thead><tbody id="spans"></tbody></table></div></details><table><thead><tr><th>Display field</th><th>Permitted input state</th><th>Observed pixels</th><th>Judgment</th></tr></thead><tbody id="checks"></tbody></table><p id="joint"></p></section></main>
<script>const result=PAYLOAD;const all=result.samples;let selected=0,visible=[];const navButtons=new Map();
const el=id=>document.getElementById(id);const fmt=x=>x===undefined?'unavailable':JSON.stringify(x,null,2);const names={counter_glyph:'Counter / mode',primary_frequency:'Primary frequency',active_bands:'Active bands',main_arrows:'Main arrows',main_bars:'Main strength',secondary:'Secondary cards',muted_badge:'MUTED badge'};const literal=o=>o.state+': '+JSON.stringify(o.value)+(o.reason?' ('+o.reason+')':'');const seconds=x=>x===null?'none':x.toFixed(6)+' s';const changes=result.observed_changes||[];const events=result.sequence?.events||[];const product=result.primary_judgment;
el('title').textContent=(product?'Recorded display evaluation · timed policy: ':result.selection_mode==='input_transition_review'?'Encounter with consecutive transitions: ':result.selection_mode==='all_recorded_frames'?'Consecutive-frame encounter: ':'Sampled encounter: ')+result.result;if(product){const c=product.counts||{};const q=result.reader_qualification||{};el('summary').textContent=(c.passed||0)+' passed · '+(c.failed||0)+' failed · '+(c.inconclusive||0)+' inconclusive / '+(c.required_events||0)+' required visible events. Reason '+product.reason_code+'. Reader qualification '+(q.status||'REJECTED')+'. Raw frame result '+result.raw_frame_result+'.'}else{el('summary').textContent=Object.entries(result.counts.fields).map(([k,v])=>v+' '+k.toLowerCase().replaceAll('_',' ')).join(' · ')+' / '+result.counts.required+' required checks. '+result.coverage.unique_frames+' unique original frames.'}
if(result.assessment){const a=result.assessment;el('assessment').hidden=false;el('heldAssessment').textContent='Held observations: '+a.held.status+' · '+a.held.required+' required field checks.';el('responseAssessment').textContent='Event targets observed: '+a.event_response.status+' · '+a.event_response.observed+'/'+a.event_response.required+'.';el('afterAssessment').textContent='After the first correct image: '+a.after_correct.differing_spans+' differing and '+a.after_correct.unresolved_spans+' unresolved spans retained.';for(const issue of a.held.issues){const b=document.createElement('button');b.textContent=(names[issue.field]||issue.field)+': '+issue.reason+' · '+issue.frame_ids.length+' held observation(s)';b.onclick=()=>show(all.findIndex(s=>s.frame_id===issue.first.frame_id));el('heldIssues').append(b)}}
el('coverage').textContent=result.coverage.regions.map(r=>r.start_seconds+'–'+r.end_seconds+' s (end exclusive): '+r.unique_frames+'/'+r.available_recorded_frames+' recorded frames read'+(r.complete_recorded_frame_coverage===null?' (sampled)':r.complete_recorded_frame_coverage?' (complete within range)':' (incomplete)')+'. Selected '+seconds(r.first_selected_offset_seconds)+' to '+seconds(r.last_selected_offset_seconds)+'. Read '+seconds(r.first_observed_offset_seconds)+' to '+seconds(r.last_observed_offset_seconds)+'. Largest gap including boundaries '+seconds(r.maximum_unobserved_gap_seconds)+'. '+r.unrecorded_source_frames+' source frames not recorded.').join(' ');

const eventName=e=>{const p=e.wire_rows.find(r=>r.priority);return p?p.band+' '+p.frequency+' primary · '+e.wire_rows.length+' alerts':'No live radar alerts'};
const jump=(parent,point,label)=>{if(!point)return;const index=all.findIndex(s=>s.frame_id===point.frame_id);if(index<0)return;let b=document.createElement('button');b.textContent=label+' · '+seconds(point.offset_seconds??all[index].offset_seconds??null);b.onclick=()=>show(index);parent.append(b)};
const firmware=result.recorded_firmware||{};
el('recordedFirmware').textContent='Recorded firmware: '+(firmware.git_sha||'source unavailable')+' · image '+(firmware.image_id||'unavailable')+' · boot '+(firmware.boot_id??'unavailable')+' · reader '+(result.reader_method_version??'unavailable')+'. These findings describe that recording.';
const findings=result.event_findings||[];
if(findings.length){
  el('findings').hidden=false;
  for(const finding of findings){
    const row=document.createElement('tr');
    const input=document.createElement('td');
    const priority=finding.input.find(item=>item.priority);
    input.textContent=finding.event_id+' · '+(priority?priority.band+' '+priority.frequency+' · '+finding.input.length+' alerts':'No live radar alerts');
    row.append(input);
    const marker=(cell,point,label)=>{
      if(!point){cell.append(document.createTextNode(label+': not observed'));return}
      const time=finding.input_anchor_ns===null?'': ' · +'+((point.capture_ns-finding.input_anchor_ns)/1e6).toFixed(3)+' ms';
      jump(cell,point,label+time);
    };
    const correct=document.createElement('td');marker(correct,finding.first_correct,'All required fields');row.append(correct);
    const different=document.createElement('td');
    for(const item of finding.differences){
      marker(different,item.last,names[item.field]||'Joint display state');
      const value=item.last_observed;
      if(value){const text=document.createElement('p');text.textContent=JSON.stringify(value.value);different.append(text)}
    }
    if(!finding.differences.length)different.textContent='None observed';
    row.append(different);
    const unknown=document.createElement('td');
    for(const item of finding.unresolved)marker(unknown,item.first,(names[item.field]||'Joint display state')+' · '+item.frames+' frames');
    if(!finding.unresolved.length)unknown.textContent='None in these observations';
    row.append(unknown);el('findingRows').append(row);
  }
}
for(let i=0;i<events.length;i++){let option=document.createElement('option');option.value=i;const verdict=product?.events?.find(e=>e.event_id===events[i].event_id);option.textContent=(verdict?verdict.result+' · ':'')+events[i].event_id+' · '+eventName(events[i]);el('eventSelect').append(option)}
function renderEvent(){const e=events[Number(el('eventSelect').value)];if(!e){el('eventReview').hidden=true;return}const verdict=product?.events?.find(v=>v.event_id===e.event_id);el('eventJudgment').textContent=verdict?verdict.result+' · '+verdict.reason_code.replaceAll('_',' ').toLowerCase():product?'No qualified judgment for this event.':'';el('productEventLinks').replaceChildren();if(verdict){jump(el('productEventLinks'),verdict.first_decisive_marker,'Decisive image');jump(el('productEventLinks'),verdict.response_acquisition?.deadline_capture_marker_bracket?.end,'Deadline observation');jump(el('productEventLinks'),verdict.verification_closure_proof?.verification_boundary,'Verification boundary');jump(el('productEventLinks'),verdict.closing_current_correct,'Closing observation')}el('eventSummary').textContent=e.summary;const t=e.timing;el('eventTiming').textContent=t.status==='observed_capture_marker'?'The first correct recorded image was '+t.host_send_to_first_correct_capture_ms.map(x=>x.toFixed(3)).join('–')+' ms after the completing notification send call. '+(t.complete_recorded_frame_prefix?'Every recorded image from the event request through that image was read. ':'Earlier recorded images were not all read; this is a sampled observation time. ')+t.physical_appearance_reason:'First-correct timing unavailable: '+t.reason;const c=e.coverage;el('eventCoverage').textContent=c.read_recorded_frames+'/'+c.available_recorded_frames+' recorded images read across this entire input event; '+c.unrecorded_source_frames+' source frames not recorded; largest gap '+c.maximum_gap_between_read_markers_ms.toFixed(3)+' ms. A correct image does not establish correctness through an unobserved gap.';el('eventLinks').replaceChildren();jump(el('eventLinks'),e.preceding_observation,'Before input');jump(el('eventLinks'),e.last_definite_not_correct_before_first,'Last observed different state before correct');jump(el('eventLinks'),e.first_correct,'First all-required correct');el('eventFields').replaceChildren();for(const [name,point]of Object.entries(e.first_correct_by_field)){if(point)jump(el('eventFields'),point,names[name]);else{const p=document.createElement('p');p.textContent=names[name]+': no supported correct reading';el('eventFields').append(p)}}el('eventAfter').replaceChildren();for(const change of e.changes_after_correct){const detail=e.observation_spans[change.span_index];let label=change.status.replaceAll('_',' ').toLowerCase();if(change.not_correct_fields.length)label+=' · differing '+change.not_correct_fields.map(n=>names[n]).join(', ');if(change.unresolved_fields.length)label+=' · unresolved '+change.unresolved_fields.map(n=>names[n]).join(', ');label+=' · '+detail.frame_count+' consecutive frame(s)';jump(el('eventAfter'),change.first,label)}if(!e.changes_after_correct.length)el('eventAfter').textContent='No later change was observed in the selected images.'}
el('eventSelect').onchange=()=>{stopPlayback();renderEvent();const e=events[Number(el('eventSelect').value)];const point=e?.observation_spans[0]?.first;if(point)show(all.findIndex(s=>s.frame_id===point.frame_id))};renderEvent();

let playback=null;function stopPlayback(){if(playback!==null)clearTimeout(playback);playback=null;el('play').textContent='Play consecutive frames'}
function playNext(){const here=all[selected];let nextIndex=selected+1;while(nextIndex<all.length&&all[nextIndex].video_frame_index===here.video_frame_index)nextIndex++;const next=all[nextIndex];if(!next||next.video_frame_index!==here.video_frame_index+1||next.source_frame_seq!==here.source_frame_seq+1){stopPlayback();return}const delay=(next.capture_ns-here.capture_ns)/1e6*Number(el('speed').value);playback=setTimeout(()=>{show(nextIndex,true);playNext()},delay)}
el('play').onclick=()=>{if(playback!==null){stopPlayback();return}el('play').textContent='Pause';playNext()};el('speed').onchange=stopPlayback;

function readingText(name,o){if(!o)return'not read';let text=o.state+': '+fmt(o.value)+(o.reason?'\n'+o.reason:'');if(o.sampled_illumination&&!o.sampled_illumination.within_sampled_ratio_bounds)text+='\nUnequal sampled stroke brightness; literal value retained. '+o.sampled_illumination.meaning;if(name==='main_arrows'&&o.direction_states)text+='\n'+Object.entries(o.direction_states).map(([n,d])=>n+': '+d.state+' ('+d.color+')').join('\n');if(name==='secondary'&&o.cards)text+='\n'+o.cards.map(c=>'Card '+(c.slot+1)+': '+(c.band&&c.frequency?c.band+' '+c.frequency:'text unresolved')+', '+(c.direction||'direction unresolved')+', '+(c.bars_state==='readable'?c.bars+' bars':'bars '+c.bars_state)).join('\n');return text}
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
    parser.add_argument("--observe-behavior", action="store_true", help="read every recorded image of authored input events and report actual behavior without a response deadline")
    parser.add_argument("--compare-to", type=Path, help="compare behavior with an earlier result.json from the same inputs and reader")
    parser.add_argument("--reuse-readings", type=Path, help="reuse verified independent readings of this exact recording; inputs and behavior are compared anew")
    parser.add_argument("--reader-workers", type=int, choices=range(1, 9), default=4,
                        help="bounded independent frame readers for --observe-behavior (1–8)")
    parser.add_argument("--configuration", type=Path, help="independently verified, exact-window display settings; missing settings stay unknown")
    parser.add_argument("--reader-qualification", type=Path,
                        help="exact retained qualification manifest for the behavior reader, camera profile and controls")
    parser.add_argument("--out", type=Path, required=True, help="new result directory; existing results are never replaced")
    args = parser.parse_args()
    if args.compare_to and not args.observe_behavior:
        parser.error("--compare-to requires --observe-behavior")
    if args.reuse_readings and not args.observe_behavior:
        parser.error("--reuse-readings requires --observe-behavior")
    if args.observe_behavior and (args.all_frames or args.transition_window):
        parser.error("--observe-behavior owns full event selection; use --range for a bounded subset")
    if args.all_frames and not (args.ranges or args.transition_window):
        parser.error("--all-frames requires explicit --range or --transition-window bounds")
    if args.all_frames and args.cadence != 2:
        parser.error("--cadence cannot be combined with --all-frames")
    try:
        args.out.mkdir(parents=True, exist_ok=False)
        if args.observe_behavior:
            from encounter_behavior import analyze_behavior
            result = analyze_behavior(args.run_dir, args.out, args.ranges,
                                      args.configuration, args.reader_qualification, args.compare_to, args.reuse_readings,
                                      workers=args.reader_workers)
        else:
            result = analyze(args.run_dir, args.out, args.transition_window or args.ranges, args.cadence,
                         args.configuration, transition_only=bool(args.transition_window), all_frames=args.all_frames)
    except (OSError, ValueError) as exc:
        print(sanitize_artifact_value(str(exc), run_dir=args.out), file=sys.stderr)
        return 2
    label = "consecutive-frame encounter" if args.all_frames else "sampled encounter"
    if args.observe_behavior:
        counts = result["summary"]
        from encounter_capability import frequency_capability_summary
        print("[bench] frequency reading: " + frequency_capability_summary(
            result.get("evidence", {}).get("primary_frequency_calibration")))
        print(f"{result['result']} — {counts['targets_observed']}/{counts['events']} complete display targets observed; "
              f"{counts['events_with_findings']} events with ending or post-target findings; "
              f"{counts['unresolved_frames']} frames have unresolved field comparisons. "
              f"Read {counts['read_frames']}/{counts['available_frames']} recorded event frames.")
        interval = counts.get("interval_coverage", {}).get("counts", {})
        if interval.get("after_complete_input_frames"):
            print(f"Recorded matches: {interval['matching_frames']}/{interval['after_complete_input_frames']} "
                  f"frames after complete host input. Unresolved: {interval['unresolved_before_target_frames']} "
                  f"before first target, {interval['unresolved_after_target_frames']} after. "
                  f"Other acquisition content: {interval['other_acquisition_observed_frames']} frames; cause unassigned.")
        print("Open report.html for actual transitions, original images, source explanations and build comparison.")
        return {"NO_DIFFERENCES_OBSERVED": 0, "DIFFERENCES_FOUND": 1, "MEASUREMENT_INCOMPLETE": 2}[result["result"]]
    print(f"{result['result']} — {label}: {result['counts']['fields']} / {result['counts']['required']} required checks")
    print("Open report.html for original images, expected states, pixel readings and unresolved checks.")
    return {"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 2}[result["result"]]


if __name__ == "__main__":
    raise SystemExit(main())
