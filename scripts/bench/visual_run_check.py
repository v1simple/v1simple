#!/usr/bin/env python3
"""Measure alert display timing from retained emulator and camera evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable

from PIL import Image, ImageChops, ImageStat


FRAME_WIDTH = 480
FRAME_HEIGHT = 200
FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 3
# display_layout.h reserves framebuffer x=0..76 for the live top/status field.
ALERT_REGION_LEFT = round(FRAME_WIDTH * 77 / 640)
REFERENCE_FRAME_COUNT = 10
PRE_EVENT_NS = 150_000_000
POST_EVENT_NS = 250_000_000
LEDGER_SCHEMA = 8


class VisualCheckError(RuntimeError):
    pass


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise VisualCheckError(f"unreadable evidence: {path.name}") from exc
    if not isinstance(value, dict):
        raise VisualCheckError(f"malformed evidence: {path.name}")
    return value


def read_ndjson(path: Path) -> Iterable[dict[str, Any]]:
    try:
        with path.open(encoding="utf-8") as handle:
            for line in handle:
                if line.strip():
                    value = json.loads(line)
                    if not isinstance(value, dict):
                        raise ValueError
                    yield value
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        raise VisualCheckError(f"malformed evidence: {path.name}") from exc


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise VisualCheckError(f"unreadable evidence: {path.name}") from exc
    return digest.hexdigest()


def hardware_revision(serial_path: Path) -> str:
    try:
        match = re.search(r"\brevision=([A-Za-z0-9_-]+)", serial_path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise VisualCheckError("bench serial log is unreadable") from exc
    if not match:
        raise VisualCheckError("bench serial log has no hardware revision")
    return match.group(1)


def run_metadata(run_dir: Path) -> dict[str, Any]:
    replay = run_dir / "replay"
    result = read_json(replay / "window_result.json")
    camera_result = read_json(replay / "camera" / "camera_result.json")
    preflight = read_json(replay / "camera" / "camera_preflight.json")
    timing = camera_result.get("video_timing_verification_result") or {}
    recorder = camera_result.get("recorder_stats") or {}
    if result.get("result") != "PASS" or (result.get("runtime_qualification") or {}).get("status") != "qualified":
        raise VisualCheckError("run is not runtime-qualified")
    if camera_result.get("result") != "CAPTURED" or preflight.get("result") != "PASS":
        raise VisualCheckError("run has no qualified camera evidence")
    if any(
        int(value or 0) != 0
        for value in (
            recorder.get("capture_drops"),
            recorder.get("writer_backpressure_drops"),
            recorder.get("timestamp_errors"),
            timing.get("missing_encoded_frame_count"),
            timing.get("extra_encoded_frame_count"),
        )
    ):
        raise VisualCheckError("run camera evidence has drops or timestamp errors")

    transform = (preflight.get("registration") or {}).get("transform")
    profile = camera_result.get("profile")
    if not isinstance(transform, dict) or not isinstance(profile, dict):
        raise VisualCheckError("camera profile or registration is incomplete")
    crop = transform.get("crop_fractions")
    framerate = profile.get("framerate")
    if (
        not isinstance(crop, list)
        or len(crop) != 4
        or not all(isinstance(value, (int, float)) for value in crop)
        or not isinstance(framerate, int)
        or framerate <= 0
    ):
        raise VisualCheckError("camera timing or crop is malformed")
    video = replay / "camera" / str(camera_result.get("video") or "")
    if not video.is_file():
        raise VisualCheckError("camera video is missing")

    compatibility = {
        "scenario_sha256": sha256(replay / "replay_scenario.json"),
        "hardware_revision": hardware_revision(replay / "bench_serial.log"),
        "runtime_mode": (result.get("runtime_qualification") or {}).get("mode"),
        "emulator_mode": (result.get("emulator") or {}).get("mode"),
        "blink_profile": (result.get("emulator") or {}).get("blink_profile"),
        "camera_name": camera_result.get("camera_name"),
        "camera_profile": profile,
    }
    compatibility_id = hashlib.sha256(
        json.dumps(compatibility, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    artifact_hashes = result.get("artifacts") or {}
    source_identity = {
        "git_sha": result.get("git_sha"),
        "image_id": (result.get("runtime_identity") or {}).get("image_id"),
        "capture_id": camera_result.get("capture_id"),
        "stimulus_sha256": (artifact_hashes.get("replay_stimulus") or {}).get("sha256"),
        "delivery_sha256": (artifact_hashes.get("replay_delivery") or {}).get("sha256"),
    }
    return {
        "run_id": run_dir.name,
        "git_sha": result.get("git_sha"),
        "image_id": (result.get("runtime_identity") or {}).get("image_id"),
        "compatibility_id": compatibility_id,
        "source_id": hashlib.sha256(
            json.dumps(source_identity, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest(),
        "crop_fractions": crop,
        "video": video,
        "replay": replay,
    }


def alert_events(replay: Path) -> list[dict[str, Any]]:
    stimuli = list(read_ndjson(replay / "replay_stimulus.ndjson"))
    deliveries: dict[tuple[Any, Any], dict[str, dict[str, Any]]] = {}
    for item in read_ndjson(replay / "replay_delivery.ndjson"):
        key = (item.get("stimulusSequence"), item.get("emissionOrdinal"))
        deliveries.setdefault(key, {})[str(item.get("state"))] = item
    if not stimuli:
        raise VisualCheckError("replay stimulus evidence is empty")
    events: list[dict[str, Any]] = []
    previous = stimuli[0].get("expected") or {}
    for stimulus in stimuli[1:]:
        expected = stimulus.get("expected") or {}
        before_alerts = previous.get("alerts") or []
        after_alerts = expected.get("alerts") or []
        kind = "appear" if not before_alerts and after_alerts else "clear" if before_alerts and not after_alerts else None
        if kind:
            display = next(
                (item for item in stimulus.get("notifications") or [] if item.get("kind") == "display_frame"),
                None,
            )
            if not display:
                raise VisualCheckError("alert transition has no display-frame emission")
            key = (stimulus.get("stimulusSequence"), display.get("ordinal"))
            delivery = deliveries.get(key) or {}
            requested = delivery.get("notification_requested")
            accepted = delivery.get("notification_accepted")
            if not requested or not accepted:
                raise VisualCheckError("alert transition lacks requested/accepted display evidence")
            events.append(
                {
                    "kind": kind,
                    "sequence": int(stimulus["stimulusSequence"]),
                    "before": previous,
                    "expected": expected,
                    "requested_ns": int(requested["hostMonotonicNs"]),
                    "accepted_ns": int(accepted["hostMonotonicNs"]),
                }
            )
        previous = expected
    if not events:
        raise VisualCheckError("scenario has no alert appearance or clearance transitions")
    return events


def written_timings(path: Path) -> list[dict[str, int]]:
    timings: list[dict[str, int]] = []
    video_index = 0
    for item in read_ndjson(path):
        if item.get("status") != "written":
            continue
        capture_ns = item.get("host_capture_ns")
        if not isinstance(capture_ns, int):
            raise VisualCheckError("written camera frame has no host capture timestamp")
        timings.append(
            {
                "index": video_index,
                "frame_seq": int(item["frame_seq"]),
                "capture_ns": capture_ns,
            }
        )
        video_index += 1
    if not timings:
        raise VisualCheckError("camera timing evidence has no written frames")
    return timings


def decode_event_frames(
    video: Path,
    crop: list[float],
    timings: list[dict[str, int]],
    events: list[dict[str, Any]],
) -> dict[int, Image.Image]:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise VisualCheckError("ffmpeg is required")
    ranges: list[tuple[int, int]] = []
    selected: list[dict[str, int]] = []
    for event in events:
        members = [
            item
            for item in timings
            if event["requested_ns"] - PRE_EVENT_NS
            <= item["capture_ns"]
            <= event["requested_ns"] + POST_EVENT_NS
        ]
        if len(members) < 10:
            raise VisualCheckError("camera evidence does not bracket an alert transition")
        ranges.append((members[0]["index"], members[-1]["index"]))
        selected.extend(members)
    expression = "+".join(f"between(n,{start},{end})" for start, end in ranges)
    crop_x, crop_y, crop_width, crop_height = crop
    filter_graph = (
        f"select='{expression}',"
        f"crop=iw*{crop_width}:ih*{crop_height}:iw*{crop_x}:ih*{crop_y},"
        f"scale={FRAME_WIDTH}:{FRAME_HEIGHT}:flags=area"
    )
    process = subprocess.run(
        [
            ffmpeg,
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(video),
            "-vf",
            filter_graph,
            "-fps_mode",
            "passthrough",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb24",
            "-",
        ],
        check=False,
        capture_output=True,
    )
    if process.returncode != 0:
        raise VisualCheckError("camera video decoding failed")
    expected_bytes = len(selected) * FRAME_BYTES
    if len(process.stdout) != expected_bytes:
        raise VisualCheckError("camera video and timing evidence disagree")
    images: dict[int, Image.Image] = {}
    for offset, timing in enumerate(selected):
        start = offset * FRAME_BYTES
        images[timing["index"]] = Image.frombytes(
            "RGB", (FRAME_WIDTH, FRAME_HEIGHT), process.stdout[start : start + FRAME_BYTES]
        )
    return images


def alert_region(image: Image.Image) -> Image.Image:
    return image.crop((ALERT_REGION_LEFT, 0, FRAME_WIDTH, FRAME_HEIGHT))


def squared_difference(first: Image.Image, second: Image.Image) -> float:
    rms = ImageStat.Stat(
        ImageChops.difference(alert_region(first), alert_region(second))
    ).rms
    return sum(value * value for value in rms)


def reference_medoid(
    frames: list[dict[str, int]], images: dict[int, Image.Image]
) -> dict[str, int]:
    return min(
        frames,
        key=lambda candidate: sum(
            squared_difference(images[candidate["index"]], images[item["index"]])
            for item in frames
        ),
    )


def progress_crossing(
    points: list[dict[str, Any]], level: float
) -> tuple[int, dict[str, Any], dict[str, Any]]:
    for left, right in zip(points, points[1:]):
        left_progress = float(left["progress"])
        right_progress = float(right["progress"])
        if left_progress < level <= right_progress:
            fraction = (level - left_progress) / (right_progress - left_progress)
            estimate_ns = round(
                int(left["capture_ns"])
                + fraction * (int(right["capture_ns"]) - int(left["capture_ns"]))
            )
            return estimate_ns, left, right
    raise VisualCheckError(f"camera progress never crossed {level:.0%}")


def format_alert(expected: dict[str, Any]) -> str:
    alerts = expected.get("alerts") or []
    if not alerts:
        return "idle"
    alert = alerts[0]
    band = str(alert.get("band") or "?")
    band = "Ka" if band.casefold() == "ka" else band.upper()
    frequency = float(alert.get("frequencyMHz") or 0) / 1000.0
    return f"{band} {frequency:.3f} {str(alert.get('direction') or '?').lower()}"


def measure_event(
    event: dict[str, Any],
    timings: list[dict[str, int]],
    images: dict[int, Image.Image],
) -> dict[str, Any]:
    window = [
        item
        for item in timings
        if item["index"] in images
        and event["requested_ns"] - PRE_EVENT_NS
        <= item["capture_ns"]
        <= event["requested_ns"] + POST_EVENT_NS
    ]
    before = [item for item in window if item["capture_ns"] < event["requested_ns"]]
    after = [item for item in window if item["capture_ns"] >= event["requested_ns"]]
    if len(before) < REFERENCE_FRAME_COUNT or len(after) < REFERENCE_FRAME_COUNT:
        raise VisualCheckError("camera frames do not bracket the display request")
    old_frames = before[-REFERENCE_FRAME_COUNT:]
    final_frames = after[-REFERENCE_FRAME_COUNT:]
    old_timing = reference_medoid(old_frames, images)
    final_timing = reference_medoid(final_frames, images)
    old = images[old_timing["index"]]
    final = images[final_timing["index"]]
    full_signal = squared_difference(old, final)
    old_noise = max(squared_difference(old, images[item["index"]]) for item in old_frames)
    final_noise = max(squared_difference(final, images[item["index"]]) for item in final_frames)
    if full_signal <= max(old_noise, final_noise):
        raise VisualCheckError(f"camera noise obscures stimulus {event['sequence']}")
    points: list[dict[str, Any]] = []
    for item in window:
        frame = images[item["index"]]
        progress = (
            squared_difference(old, frame)
            - squared_difference(final, frame)
            + full_signal
        ) / (2.0 * full_signal)
        points.append({**item, "progress": progress})

    old_progress = [float(item["progress"]) for item in points if item["capture_ns"] < event["requested_ns"]]
    final_progress = [float(item["progress"]) for item in points[-REFERENCE_FRAME_COUNT:]]
    reference_span = max(
        max(old_progress) - min(old_progress),
        max(final_progress) - min(final_progress),
    )
    changing = [
        float(item["progress"])
        for item in points
        if 0.5 <= float(item["progress"]) <= 0.9
    ]
    backsteps = sum(
        right < left - reference_span for left, right in zip(changing, changing[1:])
    )

    def timing(level: float) -> tuple[dict[str, Any], int]:
        estimate_ns, left, right = progress_crossing(points, level)
        if not event["requested_ns"] <= event["accepted_ns"] <= estimate_ns:
            raise VisualCheckError("display timing precedes its CoreBluetooth send")
        return {
            "ms": [
                round((estimate_ns - event["accepted_ns"]) / 1_000_000.0, 3),
                round((estimate_ns - event["requested_ns"]) / 1_000_000.0, 3),
            ],
            "camera_frame_ms": [
                round((item["capture_ns"] - event["requested_ns"]) / 1_000_000.0, 3)
                for item in (left, right)
            ],
            "frames": [left["frame_seq"], right["frame_seq"]],
            "progress": [round(float(left["progress"]), 6), round(float(right["progress"]), 6)],
        }, estimate_ns

    t50, t50_ns = timing(0.5)
    t90, t90_ns = timing(0.9)

    old_label = format_alert(event["before"])
    new_label = format_alert(event["expected"])
    return {
        "key": f"{event['kind']}:{event['sequence']}",
        "kind": event["kind"],
        "sequence": event["sequence"],
        "label": f"{old_label} to {new_label}",
        "t50": t50,
        "t90": t90,
        "draw_ms": round((t90_ns - t50_ns) / 1_000_000.0, 3),
        "reference_span": round(reference_span, 6),
        "backsteps": backsteps,
    }


def analyze_run(run_dir: Path, metadata: dict[str, Any]) -> dict[str, Any]:
    events = alert_events(metadata["replay"])
    timings = written_timings(metadata["replay"] / "camera" / "frame_timing.ndjson")
    images = decode_event_frames(
        metadata["video"], metadata["crop_fractions"], timings, events
    )
    measured = [measure_event(event, timings, images) for event in events]
    return {
        "run_id": metadata["run_id"],
        "source_id": metadata["source_id"],
        "compatibility_id": metadata["compatibility_id"],
        "git_sha": metadata["git_sha"],
        "image_id": metadata["image_id"],
        "events": measured,
    }


def load_ledger(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"schema_version": LEDGER_SCHEMA, "runs": []}
    ledger = read_json(path)
    if ledger.get("schema_version") != LEDGER_SCHEMA:
        return {"schema_version": LEDGER_SCHEMA, "runs": []}
    if not isinstance(ledger.get("runs"), list):
        raise VisualCheckError("visual ledger is malformed")
    return ledger


def save_ledger(path: Path, ledger: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(ledger, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def cached_run(ledger: dict[str, Any], metadata: dict[str, Any]) -> dict[str, Any] | None:
    return next(
        (
            item
            for item in ledger["runs"]
            if item.get("run_id") == metadata["run_id"]
            and item.get("source_id") == metadata["source_id"]
        ),
        None,
    )


def add_to_ledger(ledger: dict[str, Any], result: dict[str, Any]) -> None:
    ledger["runs"] = [item for item in ledger["runs"] if item.get("run_id") != result["run_id"]]
    ledger["runs"].append(result)
    ledger["runs"].sort(key=lambda item: str(item.get("run_id")))


def run_bracket(result: dict[str, Any], field: str) -> tuple[float, float]:
    brackets = [event[field]["ms"] for event in result["events"]]
    return (
        statistics.fmean(float(pair[0]) for pair in brackets),
        statistics.fmean(float(pair[1]) for pair in brackets),
    )


def group_mean_bracket(results: list[dict[str, Any]], field: str) -> tuple[float, float]:
    brackets = [run_bracket(result, field) for result in results]
    return (
        statistics.fmean(pair[0] for pair in brackets),
        statistics.fmean(pair[1] for pair in brackets),
    )


def group_observed_span(results: list[dict[str, Any]], field: str) -> tuple[float, float]:
    brackets = [run_bracket(result, field) for result in results]
    return min(pair[0] for pair in brackets), max(pair[1] for pair in brackets)


def run_average(result: dict[str, Any], field: str) -> float:
    return statistics.fmean(float(event[field]) for event in result["events"])


def progress_backsteps(result: dict[str, Any]) -> int:
    return sum(int(event["backsteps"]) for event in result["events"])


def format_bracket(pair: tuple[float, float]) -> str:
    return f"{pair[0]:.1f}-{pair[1]:.1f} ms"


def comparison_line(
    label: str,
    current: list[dict[str, Any]],
    previous: list[dict[str, Any]],
    field: str,
) -> str:
    current_mean = group_mean_bracket(current, field)
    previous_mean = group_mean_bracket(previous, field)
    current_span = group_observed_span(current, field)
    previous_span = group_observed_span(previous, field)
    overlap = min(current_span[1], previous_span[1]) - max(current_span[0], previous_span[0])
    if overlap >= 0:
        result = "run ranges overlap; no change is demonstrated"
    elif current_span[0] > previous_span[1]:
        result = f"current runs are at least {current_span[0] - previous_span[1]:.1f} ms slower"
    else:
        result = f"current runs are at least {previous_span[0] - current_span[1]:.1f} ms faster"
    return (
        f"  {label}: current mean {format_bracket(current_mean)} "
        f"(runs {format_bracket(current_span)}); prior mean {format_bracket(previous_mean)} "
        f"(runs {format_bracket(previous_span)}); {result}."
    )


def scalar_comparison_line(
    label: str,
    current: list[dict[str, Any]],
    previous: list[dict[str, Any]],
    field: str,
) -> str:
    current_runs = [run_average(result, field) for result in current]
    previous_runs = [run_average(result, field) for result in previous]
    current_span = (min(current_runs), max(current_runs))
    previous_span = (min(previous_runs), max(previous_runs))
    overlap = min(current_span[1], previous_span[1]) - max(current_span[0], previous_span[0])
    if overlap >= 0:
        result = "run ranges overlap; no change is demonstrated"
    elif current_span[0] > previous_span[1]:
        result = f"current runs are at least {current_span[0] - previous_span[1]:.1f} ms slower"
    else:
        result = f"current runs are at least {previous_span[0] - current_span[1]:.1f} ms faster"
    return (
        f"  {label}: current mean {statistics.fmean(current_runs):.1f} ms "
        f"(runs {format_bracket(current_span)}); prior mean "
        f"{statistics.fmean(previous_runs):.1f} ms (runs {format_bracket(previous_span)}); "
        f"{result}."
    )


def print_result(
    current: dict[str, Any], history: list[dict[str, Any]], group_size: int
) -> None:
    t50 = run_bracket(current, "t50")
    t90 = run_bracket(current, "t90")
    draw_ms = run_average(current, "draw_ms")
    worst = max(current["events"], key=lambda event: float(event["t90"]["ms"][1]))
    reference_span = max(float(event["reference_span"]) for event in current["events"])
    backsteps = progress_backsteps(current)
    camera_spacing = statistics.fmean(
        float(event["t50"]["camera_frame_ms"][1])
        - float(event["t50"]["camera_frame_ms"][0])
        for event in current["events"]
    )
    print(f"Display evidence for {current['run_id']}")
    print(f"Firmware {str(current.get('git_sha') or '')[:7]} | image {current.get('image_id') or 'unknown'}")
    print()
    print("Timing bounds the emulator send between its request and successful CoreBluetooth return.")
    print("This includes BLE transport; DUT receipt time is not observed.")
    print(
        f"T50 and T90 are estimates interpolated inside camera samples "
        f"{camera_spacing:.1f} ms apart on average."
    )
    print("The live status field is excluded from the camera measurement.")
    print(f"Measured {len(current['events'])} alert appearance/clear transitions.")
    print(f"Average 50% visual response estimate: {format_bracket(t50)}")
    print(f"Average 90% visual response estimate: {format_bracket(t90)}")
    print(f"Average 50-90% draw time: {draw_ms:.1f} ms")
    print(
        f"Slowest 90% response: "
        f"{format_bracket(tuple(float(value) for value in worst['t90']['ms']))} "
        f"({worst['label']})"
    )
    if backsteps:
        print(f"Progress warning: {backsteps} backward step(s) exceeded reference variation.")
    else:
        print(
            f"Measured 50-90% progress was monotonic; maximum old/final "
            f"reference variation was {reference_span * 100:.1f}%."
        )
    print()
    current_group = [current] + [
        item for item in history if item.get("image_id") == current.get("image_id")
    ]
    current_group = current_group[:group_size]
    if len(current_group) < group_size:
        print(
            f"Timing comparison withheld: current image has {len(current_group)} of "
            f"{group_size} compatible runs."
        )
        return

    previous_candidates = [item for item in history if item.get("image_id") != current.get("image_id")]
    image_order = list(dict.fromkeys(item.get("image_id") for item in previous_candidates))
    previous_group: list[dict[str, Any]] = []
    for image_id in image_order:
        candidate_group = [
            item for item in previous_candidates if item.get("image_id") == image_id
        ][:group_size]
        if len(candidate_group) >= group_size:
            previous_group = candidate_group
            break
        if not previous_group:
            previous_group = candidate_group

    if len(previous_group) < group_size:
        print(
            f"Timing comparison withheld: prior image has {len(previous_group)} of "
            f"{group_size} compatible runs."
        )
        return

    comparison_group = current_group + previous_group
    reversals = sum(progress_backsteps(result) for result in comparison_group)
    if reversals:
        print(f"Timing comparison withheld: {reversals} progress reversal(s) need inspection.")
        return

    print(f"{group_size}-run image comparison:")
    print(comparison_line("50% response", current_group, previous_group, "t50"))
    print(comparison_line("90% response", current_group, previous_group, "t90"))
    print(scalar_comparison_line("50-90% draw", current_group, previous_group, "draw_ms"))
    print("  Visual content correctness is not determined by this measurement.")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--history-count", type=int, default=3)
    parser.add_argument("--ledger", type=Path)
    parser.add_argument("--rebuild", action="store_true")
    return parser.parse_args()


def comparison_ready(
    current: dict[str, Any], history: list[dict[str, Any]], group_size: int
) -> bool:
    current_count = 1 + sum(
        item.get("image_id") == current.get("image_id") for item in history
    )
    previous_counts: dict[Any, int] = {}
    for item in history:
        image_id = item.get("image_id")
        if image_id != current.get("image_id"):
            previous_counts[image_id] = previous_counts.get(image_id, 0) + 1
    return current_count >= group_size and any(
        count >= group_size for count in previous_counts.values()
    )


def main() -> int:
    args = parse_args()
    run_dir = args.run_dir.resolve()
    if args.history_count < 3:
        raise VisualCheckError("history count must be at least 3")
    metadata = run_metadata(run_dir)
    ledger_path = (
        args.ledger.resolve()
        if args.ledger
        else run_dir.parents[2] / "visual_latency_ledger.json"
    )
    ledger = load_ledger(ledger_path)
    current = None if args.rebuild else cached_run(ledger, metadata)
    if current is None:
        print(f"Analyzing camera evidence for {run_dir.name}...", file=sys.stderr)
        current = analyze_run(run_dir, metadata)
        add_to_ledger(ledger, current)

    history = [] if args.rebuild else [
        item
        for item in ledger["runs"]
        if item.get("run_id") < current["run_id"]
        and item.get("compatibility_id") == current["compatibility_id"]
    ]
    history.sort(key=lambda item: item["run_id"], reverse=True)
    if not comparison_ready(current, history, args.history_count):
        discovered: list[tuple[Path, dict[str, Any]]] = []
        for candidate in sorted(run_dir.parent.iterdir(), reverse=True):
            if (
                not candidate.is_dir()
                or candidate.name >= run_dir.name
                or any(item["run_id"] == candidate.name for item in history)
            ):
                continue
            try:
                candidate_metadata = run_metadata(candidate)
            except VisualCheckError:
                continue
            if candidate_metadata["compatibility_id"] != current["compatibility_id"]:
                continue
            discovered.append((candidate, candidate_metadata))
            available = history + [metadata for _path, metadata in discovered]
            if comparison_ready(current, available, args.history_count):
                break

        available = history + [metadata for _path, metadata in discovered]
        available.sort(key=lambda item: item["run_id"], reverse=True)
        current_candidates = [
            item for item in available if item.get("image_id") == current.get("image_id")
        ]
        selected_ids = {
            item["run_id"] for item in current_candidates[: args.history_count - 1]
        }
        current_count = 1 + len(current_candidates)
        if current_count >= args.history_count:
            previous = [
                item for item in available if item.get("image_id") != current.get("image_id")
            ]
            for image_id in dict.fromkeys(item.get("image_id") for item in previous):
                image_group = [item for item in previous if item.get("image_id") == image_id]
                if len(image_group) >= args.history_count:
                    selected_ids.update(
                        item["run_id"] for item in image_group[: args.history_count]
                    )
                    break

        for candidate, candidate_metadata in discovered:
            if candidate_metadata["run_id"] not in selected_ids:
                continue
            print(f"Analyzing compatible history {candidate.name}...", file=sys.stderr)
            candidate_result = analyze_run(candidate, candidate_metadata)
            add_to_ledger(ledger, candidate_result)
            history.append(candidate_result)
    history.sort(key=lambda item: item["run_id"], reverse=True)
    save_ledger(ledger_path, ledger)
    print_result(current, history, args.history_count)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VisualCheckError as exc:
        print(f"visual check unavailable: {exc}", file=sys.stderr)
        raise SystemExit(2)
