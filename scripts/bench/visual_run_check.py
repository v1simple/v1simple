#!/usr/bin/env python3
"""Measure alert display timing from retained emulator and camera evidence."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable

from PIL import Image, ImageChops, ImageFilter, ImageStat


FRAME_WIDTH = 480
FRAME_HEIGHT = 200
FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 3
SIGNATURE_WIDTH = 120
SIGNATURE_HEIGHT = 50
MIN_VISUAL_SIGNAL_RMS = 10.0
LEDGER_SCHEMA = 1


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
        "camera_transform": transform,
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
        "framerate": framerate,
        "crop_fractions": crop,
        "video": video,
        "replay": replay,
    }


def alert_events(replay: Path) -> list[dict[str, Any]]:
    stimuli = list(read_ndjson(replay / "replay_stimulus.ndjson"))
    accepted = {
        (item.get("stimulusSequence"), item.get("emissionOrdinal")): item
        for item in read_ndjson(replay / "replay_delivery.ndjson")
        if item.get("state") == "notification_accepted"
    }
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
            delivery = accepted.get(key)
            if not delivery:
                raise VisualCheckError("alert transition has no accepted display-frame emission")
            events.append(
                {
                    "kind": kind,
                    "sequence": int(stimulus["stimulusSequence"]),
                    "before": previous,
                    "expected": expected,
                    "accepted_ns": int(delivery["hostMonotonicNs"]),
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
            if event["accepted_ns"] - 30_000_000
            <= item["capture_ns"]
            <= event["accepted_ns"] + 250_000_000
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


def rms_difference(first: Image.Image, second: Image.Image, mask: Image.Image) -> float:
    return ImageStat.Stat(ImageChops.difference(first, second).convert("L"), mask=mask).rms[0]


def sustained_bracket(
    frames: list[dict[str, int]], predicate: Any
) -> tuple[dict[str, int], dict[str, int]] | None:
    first_true: int | None = None
    for index, frame in enumerate(frames):
        if predicate(frame):
            first_true = index if first_true is None else first_true
            if index - first_true + 1 >= 2:
                lower = frames[first_true - 1] if first_true else frames[0]
                return lower, frames[first_true]
        else:
            first_true = None
    return None


def visual_signature(image: Image.Image) -> str:
    reduced = image.convert("RGB").resize(
        (SIGNATURE_WIDTH, SIGNATURE_HEIGHT), Image.Resampling.BOX
    )
    signature = bytearray()
    for red, green, blue in reduced.getdata():
        brightest = max(red, green, blue)
        if brightest < 10:
            signature.append(0)
        elif red >= green * 1.3 and red >= blue * 1.3:
            signature.append(1)
        elif green >= red * 1.2 and green >= blue * 1.2:
            signature.append(2)
        elif blue >= red * 1.2 and blue >= green * 1.2:
            signature.append(3)
        else:
            signature.append(4)
    return base64.b64encode(signature).decode("ascii")


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
        and event["accepted_ns"] - 30_000_000
        <= item["capture_ns"]
        <= event["accepted_ns"] + 250_000_000
    ]
    before = [item for item in window if item["capture_ns"] < event["accepted_ns"]]
    after = [item for item in window if item["capture_ns"] >= event["accepted_ns"]]
    if not before or not after:
        raise VisualCheckError("camera frames do not bracket alert acceptance")
    old = images[before[-1]["index"]]
    reference_timing = min(
        after, key=lambda item: abs(item["capture_ns"] - (event["accepted_ns"] + 200_000_000))
    )
    target = images[reference_timing["index"]]
    mask = (
        ImageChops.difference(old, target)
        .convert("L")
        .point(lambda value: 255 if value >= 12 else 0)
        .filter(ImageFilter.MaxFilter(5))
    )
    full_signal = rms_difference(old, target, mask)
    if full_signal < MIN_VISUAL_SIGNAL_RMS:
        raise VisualCheckError(f"no visible {event['kind']} transition for stimulus {event['sequence']}")
    old_noise = max(
        (rms_difference(old, images[item["index"]], mask) for item in before[:-1]),
        default=0.0,
    )
    settled_frames = [
        item
        for item in after
        if 170_000_000 <= item["capture_ns"] - event["accepted_ns"] <= 240_000_000
    ]
    target_noise = max(
        (rms_difference(target, images[item["index"]], mask) for item in settled_frames),
        default=0.0,
    )
    onset_threshold = max(old_noise + 1.0, full_signal * 0.02)
    complete_threshold = max(target_noise * 3.0 + 1.0, full_signal * 0.08)
    onset = sustained_bracket(
        after,
        lambda item: rms_difference(old, images[item["index"]], mask) >= onset_threshold,
    )
    complete = sustained_bracket(
        after,
        lambda item: rms_difference(target, images[item["index"]], mask) <= complete_threshold,
    )
    if not onset or not complete:
        raise VisualCheckError(f"visible {event['kind']} transition did not settle")

    def bracket(pair: tuple[dict[str, int], dict[str, int]]) -> dict[str, Any]:
        return {
            "ms": [
                round((item["capture_ns"] - event["accepted_ns"]) / 1_000_000.0, 3)
                for item in pair
            ],
            "frames": [item["frame_seq"] for item in pair],
        }

    old_label = format_alert(event["before"])
    new_label = format_alert(event["expected"])
    return {
        "key": f"{event['kind']}:{event['sequence']}",
        "kind": event["kind"],
        "sequence": event["sequence"],
        "label": f"{old_label} to {new_label}",
        "accepted_ns": event["accepted_ns"],
        "onset": bracket(onset),
        "complete": bracket(complete),
        "settled_signature": visual_signature(target),
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
        "framerate": metadata["framerate"],
        "events": measured,
    }


def load_ledger(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"schema_version": LEDGER_SCHEMA, "runs": []}
    ledger = read_json(path)
    if ledger.get("schema_version") != LEDGER_SCHEMA or not isinstance(ledger.get("runs"), list):
        raise VisualCheckError("visual ledger has an unsupported schema")
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


def midpoint(event: dict[str, Any], field: str) -> float:
    lower, upper = event[field]["ms"]
    return (float(lower) + float(upper)) / 2.0


def run_average(result: dict[str, Any], field: str) -> float:
    return statistics.fmean(midpoint(event, field) for event in result["events"])


def signature_difference(first: str, second: str) -> float:
    left = base64.b64decode(first)
    right = base64.b64decode(second)
    if len(left) != len(right):
        return 100.0
    different = sum(a != b for a, b in zip(left, right))
    return different / (SIGNATURE_WIDTH * SIGNATURE_HEIGHT) * 100.0


def visual_mismatches(
    current: dict[str, Any], history: list[dict[str, Any]]
) -> tuple[list[str], list[str]]:
    if not history:
        return [], []
    history_by_key = {
        result["run_id"]: {event["key"]: event for event in result["events"]}
        for result in history
    }
    mismatches: list[str] = []
    inconsistent_history: list[str] = []
    for event in current["events"]:
        prior = [events[event["key"]] for events in history_by_key.values() if event["key"] in events]
        if not prior:
            continue
        internal = [
            signature_difference(prior[left]["settled_signature"], prior[right]["settled_signature"])
            for left in range(len(prior))
            for right in range(left + 1, len(prior))
        ]
        history_difference = max(internal, default=0.0)
        if history_difference > 1.0:
            inconsistent_history.append(event["label"])
            continue
        tolerance = max(1.0, history_difference * 3.0)
        observed = max(
            signature_difference(event["settled_signature"], item["settled_signature"])
            for item in prior
        )
        if observed > tolerance:
            mismatches.append(f"{event['label']} ({observed:.1f}% visual difference)")
    return mismatches, inconsistent_history


def print_result(current: dict[str, Any], history: list[dict[str, Any]]) -> None:
    onset = run_average(current, "onset")
    complete = run_average(current, "complete")
    worst = max(current["events"], key=lambda event: midpoint(event, "complete"))
    print(f"Display evidence for {current['run_id']}")
    print(f"Firmware {str(current.get('git_sha') or '')[:7]} | image {current.get('image_id') or 'unknown'}")
    print()
    print("Timing starts when the Mac accepts the emulator display packet.")
    print(f"Measured {len(current['events'])} alert appearance/clear transitions.")
    print(f"Average first-visible response: {onset:.1f} ms")
    print(f"Average fully-settled response: {complete:.1f} ms")
    print(
        f"Slowest settled response: {midpoint(worst, 'complete'):.1f} ms "
        f"({worst['label']})"
    )
    print()
    if not history:
        print("No compatible visual history exists; this run is the baseline.")
        return
    mismatches, inconsistent_history = visual_mismatches(current, history)
    if inconsistent_history:
        print(
            f"Compatible history was visually inconsistent in "
            f"{len(inconsistent_history)} transition(s); no visual match is claimed."
        )
    elif mismatches:
        print(f"Settled display differed from history in {len(mismatches)} transition(s):")
        for mismatch in mismatches:
            print(f"  - {mismatch}")
    else:
        print(f"All settled displays matched the last {len(history)} compatible run(s).")
    historical = [run_average(item, "complete") for item in history]
    historical_average = statistics.fmean(historical)
    delta = complete - historical_average
    half_spread = (max(historical) - min(historical)) / 2.0 if len(historical) > 1 else 0.0
    frame_interval = 1000.0 / max(int(current.get("framerate") or 0), 1)
    unchanged_limit = max(frame_interval, half_spread)
    direction = "slower" if delta > 0 else "faster"
    print()
    print(f"Compared with the last {len(history)} compatible run(s):")
    print(f"  Average change: {abs(delta):.1f} ms {direction}")
    if len(historical) > 1:
        print(f"  Prior run-to-run spread: {max(historical) - min(historical):.1f} ms")
    if abs(delta) <= unchanged_limit:
        print("  Meaning: effectively unchanged")
    else:
        print(f"  Meaning: measurably {direction}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--history-count", type=int, default=3)
    parser.add_argument("--ledger", type=Path)
    parser.add_argument("--rebuild", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    run_dir = args.run_dir.resolve()
    if args.history_count < 0:
        raise VisualCheckError("history count cannot be negative")
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
    if len(history) < args.history_count:
        for candidate in sorted(run_dir.parent.iterdir(), reverse=True):
            if (
                len(history) >= args.history_count
                or not candidate.is_dir()
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
            candidate_result = None if args.rebuild else cached_run(ledger, candidate_metadata)
            if candidate_result is None:
                print(f"Analyzing compatible history {candidate.name}...", file=sys.stderr)
                candidate_result = analyze_run(candidate, candidate_metadata)
                add_to_ledger(ledger, candidate_result)
            history.append(candidate_result)
    history.sort(key=lambda item: item["run_id"], reverse=True)
    history = history[: args.history_count]
    save_ledger(ledger_path, ledger)
    print_result(current, history)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VisualCheckError as exc:
        print(f"visual check unavailable: {exc}", file=sys.stderr)
        raise SystemExit(2)
