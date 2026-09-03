#!/usr/bin/env python3
"""Compare count/mode pixels with replay input at fixed or automatic samples.

Requires ffmpeg/ffprobe. Reads existing recordings only; no hardware or inference
service is started. Results cover two fields at the saved frames, not complete
intervals, delivery to the DUT, response deadlines, or full display correctness.
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import struct
import subprocess
import sys
import zlib
from bisect import bisect_left
from pathlib import Path
from typing import Any

from artifact_privacy import sanitize_artifact_value
from camera_artifacts import load_capture_manifest, sha256_file, verify_capture_files
from camera_timing import compare_encoded_video_timing, probe_all_video_frames, validate_frame_sidecar
from counter_expectation import build_counter_timeline, counter_expectation_at
from counter_reader import observe
from visual_compare import FIELDS, _publish_new, _unique_object, compare

REQUIRED_FIELDS = ["count", "mode"]


def object_value(value: Any, name: str) -> dict:
    if not isinstance(value, dict):
        raise ValueError(f"{name} must be an object")
    return value


def finite_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError("nonfinite JSON number")
    return parsed


def read_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object,
                       parse_constant=finite_float, parse_float=finite_float)
    return object_value(value, path.name)


def read_records(path: Path) -> list[dict]:
    records = [json.loads(line, object_pairs_hook=_unique_object, parse_constant=finite_float, parse_float=finite_float)
               for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    if not records or any(not isinstance(item, dict) for item in records):
        raise ValueError(f"{path.name} must contain nonempty object records")
    return records


def owned_input(directory: Path, entry: dict) -> Path:
    entry = object_value(entry, "recorded input entry")
    name = entry.get("path")
    if not isinstance(name, str) or not name or Path(name).name != name or name in (".", ".."):
        raise ValueError("invalid recorded input filename")
    path = directory / name
    if path.stat().st_size != entry.get("size_bytes") or sha256_file(path) != entry.get("sha256"):
        raise ValueError(f"recorded input identity differs: {name}")
    return path


def select_frame(rows: list[dict], target_ns: int) -> tuple[int, dict]:
    """Select by timestamp before looking at pixels; never bridge a capture gap."""
    times = [row["host_capture_ns"] for row in rows]
    if not times or target_ns < times[0] or target_ns > times[-1] + rows[-1]["duration_ns"]:
        raise ValueError("requested time is outside the camera recording")
    index = bisect_left(times, target_ns)
    candidates = [i for i in (index - 1, index) if 0 <= i < len(rows)]
    selected = min(candidates, key=lambda i: (abs(times[i] - target_ns), i))
    if abs(times[selected] - target_ns) > rows[selected]["duration_ns"]:
        raise ValueError("no recorded frame within one source-frame duration of the request")
    return selected, rows[selected]


def select_automatic_frame(rows: list[dict], target_ns: int, interval: dict, timeline: dict,
                           stealth_enabled: bool | None) -> tuple[int, dict, dict]:
    """Select the closest input-resolved midpoint neighbor before reading pixels."""
    times = [row["host_capture_ns"] for row in rows]
    if not times or target_ns < times[0] or target_ns > times[-1] + rows[-1]["duration_ns"]:
        raise ValueError("interval midpoint is outside the camera recording")
    position = bisect_left(times, target_ns)
    candidates = [i for i in (position - 1, position) if 0 <= i < len(rows)]
    eligible = []
    for index in candidates:
        row = rows[index]
        capture_ns = row["host_capture_ns"]
        if not (interval["start_capture_ns"] <= capture_ns < interval["end_capture_ns"]):
            continue
        if abs(capture_ns - target_ns) > row["duration_ns"]:
            continue
        requirement = counter_expectation_at(timeline, capture_ns, stealth_enabled=stealth_enabled)
        fields = requirement.get("fields")
        if (not isinstance(fields, dict)
                or any(not isinstance(fields.get(name), dict)
                       or not isinstance(fields[name].get("allowed"), list)
                       or not fields[name]["allowed"] for name in REQUIRED_FIELDS)):
            continue
        eligible.append((index, row, requirement))
    if not eligible:
        raise ValueError("no input-resolved frame within one source-frame duration of the interval midpoint")
    return min(eligible, key=lambda item: (abs(item[1]["host_capture_ns"] - target_ns), item[0]))


def decode_frames(ffmpeg: str, video: Path, indices: list[int], width: int, height: int) -> dict[int, bytes]:
    indices = sorted(set(indices))
    if not indices:
        return {}
    expression = "+".join(f"eq(n,{index})" for index in indices)
    command = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(video),
               "-map", "0:v:0", "-vf", f"select='{expression}'", "-fps_mode", "passthrough",
               "-frames:v", str(len(indices)), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    process = subprocess.run(command, capture_output=True, check=False)
    frame_bytes = width * height * 3
    if process.returncode or len(process.stdout) != len(indices) * frame_bytes:
        raise ValueError("video decoding did not return exactly the requested original frames")
    return {index: process.stdout[n * frame_bytes:(n + 1) * frame_bytes] for n, index in enumerate(indices)}


def write_png(path: Path, rgb: bytes, width: int, height: int) -> None:
    """Retain the exact decoded RGB pixels without resizing or enhancement."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    stride = width * 3
    scanlines = b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    encoded = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))
    with path.open("xb") as handle:
        handle.write(encoded)


def save_json(path: Path, value: dict) -> None:
    _publish_new(path, sanitize_artifact_value(value, run_dir=path.parent))


def _display_mute(notification: dict, *, live: bool) -> bool:
    """Read mute state from an already validated display packet."""
    if notification.get("kind") != "display_frame":
        raise ValueError("automatic selection requires a display notification")
    raw = bytes.fromhex(notification.get("bytesHex", ""))
    data = raw[5:-2]
    if len(data) != 8:
        raise ValueError("automatic selection requires an eight-byte display payload")
    muted = bool(data[5] & 0x01)
    if live:
        planes = [bool(data[index] & 0x10) for index in (3, 4)]
        if planes[0] != planes[1] or planes[0] != muted:
            raise ValueError("automatic selection found contradictory display mute fields")
    return muted


def select_live_intervals(stimulus_records: list[dict], timeline: dict) -> list[dict]:
    """Choose every bounded count/mute live interval using recorded input only."""
    recorded: dict[int, dict] = {}
    for record in stimulus_records:
        source_index = record.get("sourceIndex")
        notifications = record.get("notifications")
        if type(source_index) is not int or source_index in recorded or not isinstance(notifications, list):
            raise ValueError("automatic selection found invalid stimulus identity")
        displays = [notification for notification in notifications
                    if isinstance(notification, dict) and notification.get("kind") == "display_frame"]
        if len(displays) != 1:
            raise ValueError("automatic selection requires one display notification per stimulus")
        recorded[source_index] = displays[0]

    states = []
    for item in timeline.get("stimuli", []):
        source_index = item.get("source_index")
        display = recorded.get(source_index)
        if display is None:
            raise ValueError("automatic selection cannot bind the validated stimulus timeline")
        alert_count = item.get("alert_count")
        states.append({
            "source_index": source_index,
            "requested_ns": item.get("stimulus_requested_ns"),
            "alert_count": alert_count,
            "muted": _display_mute(display, live=type(alert_count) is int and alert_count > 0),
        })
    if len(states) != len(recorded) or not states:
        raise ValueError("automatic selection cannot bind every recorded stimulus")

    origin = states[0]["requested_ns"]
    intervals = []
    index = 0
    while index < len(states):
        state = states[index]
        end_index = index + 1
        while (end_index < len(states)
               and states[end_index]["alert_count"] == state["alert_count"]
               and states[end_index]["muted"] == state["muted"]):
            end_index += 1
        if type(state["alert_count"]) is not int or state["alert_count"] < 0:
            raise ValueError("automatic selection found an invalid alert count")
        if state["alert_count"]:
            if end_index == len(states):
                raise ValueError("automatic selection found a live interval without a recorded end")
            start_ns = state["requested_ns"]
            end_ns = states[end_index]["requested_ns"]
            if type(start_ns) is not int or type(end_ns) is not int or end_ns <= start_ns:
                raise ValueError("automatic selection found invalid interval bounds")
            midpoint_ns = start_ns + (end_ns - start_ns) // 2
            intervals.append({
                "start_capture_ns": start_ns,
                "end_capture_ns": end_ns,
                "midpoint_capture_ns": midpoint_ns,
                "midpoint_offset_seconds": (midpoint_ns - origin) / 1_000_000_000,
                "alert_count": state["alert_count"],
                "muted": state["muted"],
                "source_indices": [item["source_index"] for item in states[index:end_index]],
            })
        index = end_index
    if not intervals or len(intervals) > 128:
        raise ValueError("automatic selection requires 1 to 128 bounded live-alert intervals")
    return intervals


def _add_samples(expected: dict, samples: list[dict], offsets: list[float],
                 intervals: list[dict] | None = None) -> None:
    for number, offset in enumerate(offsets, 1):
        identity = {"frame_id": f"sample-{number:04d}", "image_sha256": None,
                    "source_frame_seq": None, "capture_ns": None}
        expected["frames"].append({**identity, "fields": {name: {"unresolved": "sample evidence unavailable"}
                                                           for name in REQUIRED_FIELDS}})
        safe_offset = offset if type(offset) in (float, int) and math.isfinite(offset) else str(offset)
        sample = {"frame_id": identity["frame_id"], "requested_offset_seconds": safe_offset}
        if intervals is not None:
            sample["live_interval"] = intervals[number - 1]
        samples.append(sample)


def analyze(run_dir: Path, offsets: list[float] | None, out_dir: Path,
            configuration: Path | None = None) -> dict:
    automatic = offsets is None
    offsets = [] if automatic else list(offsets)
    source: dict[str, Any] = {}
    expected = {"schema_version": 1, "source": source, "required_fields": REQUIRED_FIELDS,
                "excluded_fields": {name: "outside this count/mode-only observation scope" for name in FIELDS if name not in REQUIRED_FIELDS},
                "frames": []}
    observed = {"schema_version": 1, "source": source, "anomaly_scope": REQUIRED_FIELDS, "frames": []}
    samples: list[dict] = []
    if not automatic:
        _add_samples(expected, samples, offsets)
    errors: list[str] = []
    evidence: dict[str, Any] = {}
    try:
        if (not automatic and (not offsets or len(offsets) > 128 or len(set(offsets)) != len(offsets)
                               or any(not math.isfinite(x) or x < 0 for x in offsets))):
            raise ValueError("supply 1 to 128 distinct finite nonnegative sample offsets")
        window_path = run_dir / "window_result.json"
        window = read_json(window_path)
        evidence["window_result_sha256"] = sha256_file(window_path)
        evidence["runtime_identity"] = window.get("runtime_identity")
        evidence["runtime_qualification"] = window.get("runtime_qualification")
        stimulus_path = owned_input(run_dir, window["artifacts"]["replay_stimulus"])
        delivery_path = owned_input(run_dir, window["artifacts"]["replay_delivery"])
        scenario_path = run_dir / "replay_scenario.json"
        stimulus, delivery = read_records(stimulus_path), read_records(delivery_path)
        scenario = read_json(scenario_path)
        timeline = build_counter_timeline(scenario, stimulus, delivery)
        if automatic:
            intervals = select_live_intervals(stimulus, timeline)
            offsets = [interval["midpoint_offset_seconds"] for interval in intervals]
            _add_samples(expected, samples, offsets, intervals)
        evidence.update(replay_delivery_sha256=sha256_file(delivery_path), scenario_sha256=sha256_file(scenario_path),
                        scenario_binding="Scenario semantics cross-checked against hash-bound stimulus and delivery; original window does not own the scenario file hash.")
        ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
        if not ffmpeg or not ffprobe:
            raise ValueError("ffmpeg and ffprobe are required")
        camera_dir = run_dir / "camera"
        manifest_path = camera_dir / "capture_manifest.json"
        read_json(manifest_path)  # Reject contradictory duplicate keys before the shared loader.
        manifest = load_capture_manifest(manifest_path)
        if object_value(window.get("camera"), "window camera").get("capture_id") != manifest["capture_id"]:
            raise ValueError("camera capture is not the one recorded in this replay window")
        verify_capture_files(camera_dir, manifest)
        evidence["capture_manifest_sha256"] = sha256_file(manifest_path)
        evidence["capture_id"] = manifest["capture_id"]
        entries = manifest["identity"]["artifacts"]
        source.update(run_id=manifest["capture_id"], video_sha256=entries["video"]["sha256"],
                      stimulus_sha256=sha256_file(stimulus_path))
        stealth_enabled = None
        if configuration is not None:
            settings = read_json(configuration)
            if settings.get("window_result_sha256") != evidence["window_result_sha256"]:
                raise ValueError("configuration evidence belongs to a different replay window")
            stealth_enabled = object_value(settings.get("settings"), "configuration settings").get("stealthEnabled")
            if type(stealth_enabled) is not bool:
                raise ValueError("configuration evidence requires a boolean stealthEnabled")
            evidence["configuration"] = {"sha256": sha256_file(configuration), "stealthEnabled": stealth_enabled,
                                         "precondition": settings.get("precondition"), "status": settings.get("status")}
        preflight = read_json(camera_dir / entries["preflight"]["path"])
        registration = object_value(preflight.get("registration"), "camera registration")
        if preflight.get("result") != "PASS" or registration.get("result") != "PASS":
            raise ValueError("recorded camera registration did not pass")
        video = camera_dir / entries["video"]["path"]
        sidecar = camera_dir / entries["frame_timing"]["path"]
        records = read_records(sidecar)
        validate_frame_sidecar(records)
        timing = compare_encoded_video_timing(records, probe_all_video_frames(ffprobe, video))
        save_json(out_dir / "video-timing.json", timing)
        evidence["video_timing"] = timing
        if timing.get("status") != "verified" or any(timing.get(key) for key in ("timestamp_error_count", "missing_encoded_frame_count", "extra_encoded_frame_count")):
            raise ValueError("original video and source timing do not establish frame identity")
        if any(row["phase"] != "recording" for row in records):
            raise ValueError("video sidecar contains a different capture phase")
        rows = [row for row in records if row["status"] == "written"]
        if not rows or any(b["host_capture_ns"] <= a["host_capture_ns"] for a, b in zip(rows, rows[1:])):
            raise ValueError("recording capture timestamps are missing or not increasing")
        probe = manifest["capture"]["video_probe"]
        width, height = probe["width"], probe["height"]
        if type(width) is not int or type(height) is not int or not (1 <= width <= 4096 and 1 <= height <= 2160):
            raise ValueError("unsupported recorded image dimensions")
        profile = object_value(manifest["identity"]["camera"]["profile"], "camera profile")
        if f"{width}x{height}" != profile.get("video_size"):
            raise ValueError("image dimensions differ from the owned camera profile")
        origin = stimulus[0]["requestedHostMonotonicNs"]
        selected = {}
        used_indices = set()
        for number, offset in enumerate(offsets):
            try:
                if not math.isfinite(offset * 1_000_000_000):
                    raise ValueError("sample offset is outside the camera recording")
                interval = samples[number].get("live_interval")
                target = (interval["midpoint_capture_ns"] if isinstance(interval, dict)
                          else origin + round(offset * 1_000_000_000))
                samples[number]["target_capture_ns"] = target
                if isinstance(interval, dict):
                    index, row, requirement = select_automatic_frame(
                        rows, target, interval, timeline, stealth_enabled)
                else:
                    index, row = select_frame(rows, target)
                    requirement = None
                if index in used_indices:
                    raise ValueError("request resolves to a frame already selected by another sample")
                used_indices.add(index)
                selected[number] = (index, row, requirement)
            except ValueError as exc:
                samples[number]["error"] = str(exc)
        pixels = decode_frames(ffmpeg, video, [index for index, _, _ in selected.values()], width, height)
        (out_dir / "frames").mkdir()
        for number, (index, row, requirement) in selected.items():
            image_path = out_dir / "frames" / f"{number + 1:04d}.png"
            identity = {"frame_id": samples[number]["frame_id"], "image_sha256": None,
                        "source_frame_seq": row["frame_seq"], "capture_ns": row["host_capture_ns"]}
            try:
                write_png(image_path, pixels[index], width, height)
                identity["image_sha256"] = sha256_file(image_path)
                samples[number].update(**identity, video_frame_index=index, image=f"frames/{image_path.name}")
                # Pixel observation never receives packet expectations.
                reading = observe(pixels[index], width, height, registration)
                if requirement is None:
                    requirement = counter_expectation_at(
                        timeline, row["host_capture_ns"], stealth_enabled=stealth_enabled)
                expected["frames"][number] = {**identity, "fields": requirement["fields"]}
                observed["frames"].append({**identity, "inference_status": "complete",
                                           "anomalies": reading.get("anomalies", []), "fields": reading["fields"]})
                samples[number].update(reader=reading, input=requirement)
            except (OSError, ValueError, KeyError, TypeError, RuntimeError) as exc:
                samples[number]["error"] = str(exc)
                expected["frames"][number] = {**identity, "fields": {name: {"unresolved": str(exc)} for name in REQUIRED_FIELDS}}
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as exc:
        errors.append(f"{type(exc).__name__}: {exc}")
    comparison = compare(expected, observed)
    if errors:
        if comparison["result"] != "FAIL":
            comparison["result"] = "INCONCLUSIVE"
        comparison["errors"] = errors + comparison["errors"]
    basis = ("Count/mode agreement with recorded host input at the closest qualifying bracketing frame near each "
             "fixed live count/mute interval midpoint; no response deadline or DUT-receipt claim." if automatic else
             "Count/mode agreement with recorded host input at requested samples; no response deadline or DUT-receipt claim.")
    result = {**comparison, "kind": "sampled_counter_check", "selection_mode": "automatic_live_intervals" if automatic else "explicit_offsets",
              "full_run_correctness": "not_evaluated", "comparison_basis": basis,
              "evidence": evidence, "samples": samples,
              "implementation_sha256": {p.name: sha256_file(p) for p in (Path(__file__), Path(__file__).with_name("counter_reader.py"), Path(__file__).with_name("counter_expectation.py"))}}
    save_json(out_dir / "expected.json", expected)
    save_json(out_dir / "observed.json", observed)
    save_json(out_dir / "result.json", result)
    write_report(out_dir / "report.md", result)
    return result


def write_report(path: Path, result: dict) -> None:
    counts = result["counts"]
    lines = [f"# Sampled count/mode: {result['result']}", "", result["comparison_basis"], "",
             f"{counts['matched']} matched, {counts['mismatched']} mismatched, {counts['unresolved']} unresolved / {counts['required']} required checks.", "",
             "Other display fields, complete intervals and full-run correctness are not evaluated.", "",
             "| Sample | Requested offset | Field | Expected | Observed | Result | Original frame | Reason |",
             "| --- | ---: | --- | --- | --- | --- | --- | --- |"]
    for sample, frame in zip(result["samples"], result["frames"], strict=True):
        link = f"[frame]({sample['image']})" if "image" in sample else "unavailable"
        for check in frame["checks"]:
            expected = (check.get("expected") or {}).get("allowed", "unresolved")
            observation = check.get("observed") or {}
            actual = observation.get("value") if observation.get("state") == "readable" else "unreadable"
            reasons = [sample.get("error"), sample.get("reader", {}).get("reason"), check.get("reason")]
            reason = "; ".join(dict.fromkeys(str(value) for value in reasons if value))
            reason = str(sanitize_artifact_value(reason, run_dir=path.parent)).replace("|", "\\|").replace("\n", " ")
            lines.append(f"| {sample['frame_id']} | {sample['requested_offset_seconds']}s | {check['field']} | "
                         f"`{json.dumps(expected)}` | `{json.dumps(actual)}` | {check['result']} | {link} | {reason} |")
    if result["errors"]:
        lines += ["", "Analysis errors:", ""] + ["- " + str(sanitize_artifact_value(error, run_dir=path.parent)) for error in result["errors"]]
    with path.open("x", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True, help="replay directory containing window_result.json")
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--at", type=float, nargs="+", help="seconds after the first replay request; sampled times, not deadlines")
    selection.add_argument("--auto", action="store_true", help="sample each bounded live count/mute interval midpoint")
    parser.add_argument("--configuration", type=Path, help="window-bound display configuration evidence; missing idle configuration stays unknown")
    parser.add_argument("--out", type=Path, required=True, help="new output directory; never overwrites a prior analysis")
    args = parser.parse_args()
    try:
        args.out.mkdir(parents=True, exist_ok=False)
        result = analyze(args.run_dir, None if args.auto else args.at, args.out, args.configuration)
    except (OSError, ValueError) as exc:
        print(sanitize_artifact_value(str(exc), run_dir=args.out), file=sys.stderr)
        return 2
    counts = result["counts"]
    print(f"{result['result']} — sampled count/mode: {counts['matched']} matched, "
          f"{counts['mismatched']} mismatched, {counts['unresolved']} unresolved / {counts['required']} required")
    print("Full-run correctness and response timing: not evaluated.")
    return {"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 2}[result["result"]]


if __name__ == "__main__":
    raise SystemExit(main())
