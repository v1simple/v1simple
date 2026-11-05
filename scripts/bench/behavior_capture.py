"""No-flash camera and generated-BLE adapters for the behavior bench.

The caller owns USB and DUT configuration. This module never opens a serial
port, builds firmware, uploads, or changes a DUT setting. Notification acceptance
is a host observation; only the camera establishes the visible DUT response.
"""
from __future__ import annotations

import bisect
import hashlib
import json
import math
import os
from pathlib import Path
import pty
import signal
import subprocess
import sys
import time
from typing import Any, Callable

BENCH_DIR = Path(__file__).resolve().parent
if str(BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(BENCH_DIR))

from camera_artifacts import (build_capture_manifest, load_capture_manifest,
                              publish_capture_manifest, verify_capture_files)
from camera_capture import CameraCapture
from camera_preflight import run_camera_preflight
from camera_timing import load_frame_sidecar
from run_window import V1RadioLease


class CaptureError(RuntimeError):
    """Acquisition is incomplete; this does not by itself establish a DUT fault."""


def _write_json(path: Path, value: Any) -> None:
    with path.open("x", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, sort_keys=True, allow_nan=False)
        handle.write("\n")


class CaptureSession:
    """One admitted camera session, retained through mode changes and reboots."""

    def __init__(self, out_dir: Path, expected_duration_s: int = 1, *,
                 camera_factory: Callable[..., Any] = CameraCapture,
                 preflight: Callable[..., Any] = run_camera_preflight,
                 clock_ns: Callable[[], int] = time.monotonic_ns):
        self.out_dir = Path(out_dir).resolve()
        self.out_dir.mkdir(parents=True, mode=0o700, exist_ok=False)
        self.clock_ns = clock_ns
        self.camera = camera_factory(self.out_dir, expected_duration_s)
        self._preflight = preflight
        self.preflight: dict[str, Any] | None = None
        self.result: dict[str, Any] | None = None
        self._markers = (self.out_dir / "behavior_markers.ndjson").open("x", encoding="utf-8")
        self._started = False

    def start(self) -> dict[str, Any]:
        if self._started or self.result is not None:
            raise CaptureError("camera session cannot be started twice")
        self._started = True
        try:
            self.preflight = self._preflight(self.camera)
            if self.preflight.get("result") != "PASS":
                raise CaptureError("camera registration/profile did not pass; DUT must show SCAN")
            self.mark("camera_admitted")
            return self.preflight
        except BaseException:
            self.finish(False)
            raise

    def mark(self, name: str, **metadata: Any) -> dict[str, Any]:
        if self.result is not None or self._markers.closed:
            raise CaptureError("cannot mark a finished camera session")
        if not isinstance(name, str) or not name or "host_monotonic_ns" in metadata or "event" in metadata:
            raise ValueError("invalid camera marker")
        record = {"event": name, "host_monotonic_ns": self.clock_ns(), **metadata}
        self._markers.write(json.dumps(record, sort_keys=True, allow_nan=False) + "\n")
        self._markers.flush()
        return record

    def health_problem(self) -> str:
        if self.result is not None:
            return "camera session has finished"
        return self.camera.health_problem() if self._started else "camera session has not started"

    def finish(self, completed: bool, *, after_recording: Callable[[], None] | None = None) -> dict[str, Any]:
        if self.result is not None:
            if after_recording:
                after_recording()
            return self.result
        try:
            result = (self.camera.stop(completed, after_recording=after_recording)
                      if after_recording else self.camera.stop(completed))
            if result.get("result") == "CAPTURED":
                manifest = build_capture_manifest(camera_dir=self.out_dir,
                                                  camera_result=result, suite="replay")
                path, _ = publish_capture_manifest(self.out_dir, manifest)
                result = {**result, "capture_manifest": path.name,
                          "capture_id": manifest["capture_id"],
                          "preflight": "camera_preflight.json"}
            self.result = {**result, "collection_completed": bool(completed)}
        except Exception as exc:
            self.result = {"result": "CAPTURE_FAILED", "errors": [str(exc)]}
        finally:
            self._markers.close()
        _write_json(self.out_dir / "behavior_capture_result.json", self.result)
        return self.result


def _nearest(values: list[float | int], value: float | int) -> int:
    at = bisect.bisect_left(values, value)
    candidates = [index for index in (at - 1, at) if 0 <= index < len(values)]
    if not candidates:
        raise CaptureError("camera has no written frames")
    return min(candidates, key=lambda index: abs(values[index] - value))


def decode_original_frame(video: Path, when: float) -> tuple[float, Any]:
    """Use the existing reader's decode contract with a bounded media process."""
    import numpy as np
    from read_replay_fields import PTS_TIME
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "info",
               "-ss", f"{when:.6f}", "-i", str(video), "-frames:v", "1",
               "-vf", "showinfo", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    result = subprocess.run(command, check=True, capture_output=True, timeout=30)
    match = PTS_TIME.search(result.stderr.decode(errors="replace"))
    if not match or len(result.stdout) != 1280 * 720 * 3:
        raise CaptureError("original camera frame could not be decoded")
    actual = when + float(match.group(1))
    if not math.isfinite(actual) or abs(actual - when) > .020:
        raise CaptureError("decoded camera frame is more than 20 ms from selection")
    return actual, np.frombuffer(result.stdout, dtype=np.uint8).reshape((720, 1280, 3))


class RecordedFrames:
    """Read original frames only after the retained capture has been verified."""

    def __init__(self, camera_dir: Path, *, decoder: Callable[..., Any] | None = None):
        self.camera_dir = Path(camera_dir)
        self.manifest = load_capture_manifest(self.camera_dir / "capture_manifest.json")
        verify_capture_files(self.camera_dir, self.manifest)
        artifacts = self.manifest["identity"]["artifacts"]
        self.video = self.camera_dir / artifacts["video"]["path"]
        self.rows = [row for row in load_frame_sidecar(
            self.camera_dir / artifacts["frame_timing"]["path"]) if row["status"] == "written"]
        self.host = [row["host_capture_ns"] for row in self.rows]
        self.pts = [row["video_pts_value"] / row["video_pts_timescale"] for row in self.rows]
        if not self.rows or any(b <= a for a, b in zip(self.host, self.host[1:])):
            raise CaptureError("camera has no strictly increasing written-frame timeline")
        if decoder is None:
            decoder = decode_original_frame
        self.decoder = decoder
        self.registration = self.manifest["preflight"]["registration"]

    def decoded_samples(self, start_ns: int, end_ns: int, step_s: float = .2, *,
                        progress: Callable[[int, int], None] | None = None) -> list[dict[str, Any]]:
        if not (type(start_ns) is int and type(end_ns) is int and 0 <= start_ns < end_ns):
            raise ValueError("frame interval must be increasing integer host nanoseconds")
        if not math.isfinite(step_s) or not 0 < step_s <= .2:
            raise ValueError("frame step must be positive and at most 0.2 seconds")
        step_ns = round(step_s * 1e9)
        if step_ns < 1 or (end_ns - start_ns) / step_ns > 2000:
            raise ValueError("requested frame window is too large")
        if start_ns < self.host[0] or end_ns > self.host[-1]:
            raise CaptureError("requested camera interval is outside the capture")
        count = math.ceil((end_ns - start_ns) / step_ns)
        requested = [start_ns + round(index * (end_ns - start_ns) / count)
                     for index in range(count + 1)]
        samples = []
        for target in requested:
            index = _nearest(self.host, target)
            if abs(self.host[index] - target) > 20_000_000:
                raise CaptureError("camera has a gap at a requested observation")
            actual_pts, rgb = self.decoder(self.video, self.pts[index])
            actual_index = _nearest(self.pts, actual_pts)
            if abs(actual_pts - self.pts[actual_index]) > .00001:
                raise CaptureError("decoded frame does not match the verified camera timeline")
            actual_ns = self.host[actual_index]
            if abs(actual_ns - target) > 20_000_000:
                raise CaptureError("decoded frame is more than 20 ms from the requested observation")
            if samples and actual_ns <= samples[-1]["host_monotonic_ns"]:
                raise CaptureError("decoder returned a repeated or backwards frame")
            samples.append({"host_monotonic_ns": actual_ns, "requested_host_monotonic_ns": target,
                            "video_pts_seconds": self.pts[actual_index], "rgb": rgb})
            if progress:
                progress(len(samples), len(requested))
        return samples


def is_clear_notification(event: dict[str, Any]) -> bool:
    """Accept only checksum-valid canonical broadcast count-zero row acceptance."""
    if event.get("state") != "notification_accepted" or event.get("characteristic") != "B2CE":
        return False
    try:
        raw = bytes.fromhex(event.get("payloadHex", ""))
    except (TypeError, ValueError):
        return False
    return (len(raw) == 14 and raw[:5] == bytes.fromhex("AAD8EA4308")
            and raw[5:12] == bytes(7) and raw[-1] == 0xAB
            and raw[-2] == sum(raw[:-2]) % 256
            and type(event.get("hostMonotonicNs")) is int and event["hostMonotonicNs"] > 0)


def phase_intervals(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Actual request-time phase intervals; delivery acceptance remains separate."""
    result: list[dict[str, Any]] = []
    for event in events:
        if event.get("state") != "stimulus_requested":
            continue
        name = event.get("expected", {}).get("phase")
        now = event.get("requestedHostMonotonicNs")
        if not isinstance(name, str) or type(now) is not int:
            raise CaptureError("stimulus event has no phase/time")
        if result and now < result[-1]["last_request_ns"]:
            raise CaptureError("stimulus request time moved backwards")
        # A restart has new source index zero, even when the phase name is equal.
        if not result or result[-1]["phase"] != name or event.get("sourceIndex") == 0:
            if result:
                result[-1]["end_ns"] = now
            result.append({"phase": name, "start_ns": now, "last_request_ns": now,
                           "first_stimulus_sequence": event.get("stimulusSequence"),
                           "end_ns": None})
        else:
            result[-1]["last_request_ns"] = now
    return result


def verified_replay_delivery(result: dict[str, Any], scenario_path: Path) -> dict[str, Any]:
    """Pin complete authored runs to accepted bytes and table-completion times.

    This is host notification acceptance, never a DUT receipt acknowledgement.
    The resolved scenario is checked against its recorded hash and every requested
    sample, including alert projection: a detector setting that filters an authored
    band makes this observation incomplete instead of silently reducing coverage.
    """
    def require(condition: bool, reason: str) -> None:
        if not condition:
            raise CaptureError("Replay delivery evidence incomplete: " + reason)

    require(result.get("lifecycle_completed") is True, "lifecycle did not complete")
    raw = Path(scenario_path).read_bytes()
    require(len(raw) <= 8_000_000, "resolved scenario exceeds read bound")
    document = json.loads(raw)
    require(isinstance(document, dict) and document.get("schemaVersion") == 2,
            "unsupported resolved scenario")
    samples = document.get("samples")
    require(isinstance(samples, list) and 0 < len(samples) <= 10_000, "invalid scenario samples")
    digest = hashlib.sha256(raw).hexdigest()
    events = result.get("events")
    require(isinstance(events, list) and 0 < len(events) <= 200_000
            and all(isinstance(event, dict) for event in events), "invalid event stream")
    resolved = [event for event in events if event.get("state") == "scenario_resolved"]
    configured = [event for event in events if event.get("state") == "configured"]
    require(len(resolved) == len(configured) == 1, "scenario identity is missing or repeated")
    require(resolved[0].get("sha256") == configured[0].get("scenarioSha256") == digest
            and resolved[0].get("byteCount") == len(raw)
            and resolved[0].get("sampleCount") == configured[0].get("totalSamples") == len(samples)
            and resolved[0].get("origin") == configured[0].get("scenarioOrigin") == document.get("origin"),
            "resolved scenario hash/count/origin does not match configuration")
    require(configured[0].get("blinkProfile") == "steady", "steady blink policy was not recorded")
    require(not any(event.get("state") in ("notification_dropped", "notification_skipped")
                    for event in events), "notification was dropped or skipped")
    requests = [event for event in events if event.get("state") == "stimulus_requested"]
    require(bool(requests) and len(requests) % len(samples) == 0, "authored run is truncated")
    runs = len(requests) // len(samples)
    complete_count = sum(event.get("state") == "complete" for event in events)
    require(complete_count == result.get("complete_count") == runs, "completion count does not match authored runs")

    accepted: dict[tuple[int, int], dict[str, Any]] = {}
    accepted_global: set[int] = set()
    previous_accept_ns = 0
    for event in events:
        if event.get("state") != "notification_accepted":
            continue
        when, global_sequence = event.get("hostMonotonicNs"), event.get("globalTxSequence")
        require(type(when) is int and when > 0 and when >= previous_accept_ns
                and type(global_sequence) is int and global_sequence > 0
                and global_sequence not in accepted_global, "invalid acceptance time or duplicate identity")
        previous_accept_ns = when
        accepted_global.add(global_sequence)
        sequence, ordinal = event.get("stimulusSequence"), event.get("emissionOrdinal")
        if sequence is None and ordinal is None:
            continue  # Session replies and the automatic terminal clear are not authored samples.
        require(type(sequence) is int and sequence > 0 and type(ordinal) is int and ordinal >= 0,
                "partial stimulus acceptance identity")
        key = (sequence, ordinal)
        require(key not in accepted, "repeated stimulus acceptance")
        accepted[key] = event

    phases: list[dict[str, Any]] = []
    previous_request_ns = previous_table_ns = 0
    matched: set[tuple[int, int]] = set()
    for index, request in enumerate(requests):
        source_index, run_index = index % len(samples), index // len(samples)
        sample = samples[source_index]
        require(isinstance(sample, dict) and sample.get("sourceIndex") == source_index,
                "resolved source indices are not contiguous")
        sequence, when = request.get("stimulusSequence"), request.get("requestedHostMonotonicNs")
        require(sequence == index + 1 and type(sequence) is int and request.get("sourceIndex") == source_index
                and type(when) is int and when > previous_request_ns,
                "requested sample sequence or timestamp is discontinuous")
        previous_request_ns = when
        offset = request.get("replayOffsetSeconds")
        require(type(offset) in (float, int) and math.isfinite(offset)
                and type(sample.get("offsetSeconds")) in (float, int)
                and abs(offset - sample["offsetSeconds"]) <= 0.000001,
                "requested sample offset differs from resolved scenario")
        expected = request.get("expected", {})
        require(isinstance(expected, dict) and isinstance(sample.get("phase"), str)
                and expected.get("phase") == sample["phase"]
                and expected.get("muted") is sample.get("muted")
                and expected.get("displayOn") is True and expected.get("arrowBlink") is False
                and expected.get("bandBlink") is False, "requested phase/display policy differs from scenario")
        authored_alerts = sample.get("alerts")
        require(isinstance(authored_alerts, list) and all(isinstance(row, dict) for row in authored_alerts),
                "invalid authored alert table")
        fields = ("band", "bandMask", "frequencyMHz", "direction", "priority", "junk", "photoType")
        authored = [{**{key: row.get(key) for key in fields}, "bars": row.get("strength")}
                    for row in authored_alerts]
        actual_alerts = expected.get("alerts")
        require(isinstance(actual_alerts, list) and all(isinstance(row, dict) for row in actual_alerts)
                and [{key: row.get(key) for key in (*fields, "bars")} for row in actual_alerts] == authored,
                "requested alert table was filtered or changed from the authored scenario")
        notifications = request.get("notifications")
        table_rows = max(1, len(authored))
        require(isinstance(notifications, list) and len(notifications) == table_rows + 1,
                "complete alert table and display packet were not requested")
        table_ns = None
        for ordinal, notification in enumerate(notifications):
            require(isinstance(notification, dict) and notification.get("ordinal") == ordinal,
                    "notification ordinals are not contiguous")
            key = (sequence, ordinal)
            event = accepted.get(key)
            require(event is not None, "planned notification has no acceptance")
            try:
                payload = bytes.fromhex(notification.get("bytesHex", ""))
                received_payload = bytes.fromhex(event.get("payloadHex", ""))
            except (ValueError, TypeError) as exc:
                raise CaptureError("Replay delivery evidence has malformed packet hex") from exc
            require(len(payload) >= 7 and payload[0] == 0xAA and payload[-1] == 0xAB
                    and payload[-2] == sum(payload[:-2]) % 256, "planned packet checksum is invalid")
            require(notification.get("channel") == "display_short" and event.get("characteristic") == "B2CE"
                    and payload == received_payload and event.get("payloadSha256") == hashlib.sha256(payload).hexdigest()
                    and event["hostMonotonicNs"] >= when, "accepted bytes/channel/time differ from request")
            if ordinal < table_rows:
                require(notification.get("kind") == "alert_row" and notification.get("alertRowCount") == len(authored)
                        and notification.get("alertRowIndex") == (ordinal + 1 if authored else 0)
                        and payload[3] == 0x43, "accepted table row identity is incomplete")
                table_ns = event["hostMonotonicNs"]
            else:
                require(notification.get("kind") == "display_frame" and payload[3] == 0x31,
                        "accepted display frame is missing")
            matched.add(key)
        require(table_ns is not None and table_ns > previous_table_ns, "accepted alert tables are out of order")
        previous_table_ns = table_ns
        if source_index == 0 or phases[-1]["phase"] != sample["phase"]:
            if phases:
                phases[-1]["end_ns"] = table_ns
            phases.append({"phase": sample["phase"], "run_index": run_index,
                           "start_ns": table_ns, "request_start_ns": when, "last_request_ns": when,
                           "first_stimulus_sequence": sequence, "end_ns": None})
        else:
            phases[-1]["last_request_ns"] = when
    require(matched == set(accepted), "acceptance references an unknown planned emission")
    return {"scenario_sha256": digest, "completed_runs": runs, "requested_stimuli": len(requests),
            "accepted_notifications": len(matched), "phase_intervals": phases,
            "anchor_kind": "host_notification_acceptance", "notification_acceptance_is_dut_ack": False}


def terminal_clear_event(events: list[dict[str, Any]]) -> dict[str, Any] | None:
    """The player's final clear is untagged, after the last authored sample.

Scenario clears include a following display packet. They must not be used to
claim a packet-free tail while the terminal clear is still queued asynchronously.
"""
    stimuli = [event for event in events if event.get("state") == "stimulus_requested"]
    if not stimuli:
        return None
    last_request = stimuli[-1].get("requestedHostMonotonicNs")
    if type(last_request) is not int:
        raise CaptureError("last stimulus has no request timestamp")
    candidates = [event for event in events if is_clear_notification(event)
                  and event.get("stimulusSequence") is None
                  and event["hostMonotonicNs"] >= last_request]
    return candidates[-1] if candidates else None


def is_idle_notification(event: dict[str, Any]) -> bool:
    """A checksum-valid, untagged terminal idle display, not an alert-table clear."""
    if (event.get("state") != "notification_accepted" or event.get("characteristic") != "B2CE"
            or event.get("stimulusSequence") is not None or event.get("emissionOrdinal") is not None):
        return False
    try:
        raw = bytes.fromhex(event.get("payloadHex", ""))
    except (TypeError, ValueError):
        return False
    mode_bits = {0x77: 0x04, 0x18: 0x08, 0x38: 0x0C, 0x39: 0x04, 0x1C: 0x08, 0x3E: 0x04}
    return (len(raw) == 15 and raw[:5] == bytes.fromhex("AAD8EA3109")
            and raw[5] in mode_bits and raw[6] == raw[5] and raw[7:10] == bytes(3)
            and raw[10] in (0x0C, 0x0D) and raw[11] == mode_bits[raw[5]]
            and raw[12] >> 4 <= 9 and raw[12] & 0x0F <= 9
            and raw[-1] == 0xAB and raw[-2] == sum(raw[:-2]) % 256
            and type(event.get("hostMonotonicNs")) is int and event["hostMonotonicNs"] > 0)


def terminal_clear_bundle(events: list[dict[str, Any]], *, max_skew_s: float = .25
                          ) -> tuple[dict[str, Any], dict[str, Any]] | None:
    """Require the terminal table-clear then idle-display pair before silence."""
    if not math.isfinite(max_skew_s) or not 0 < max_skew_s <= .25:
        raise ValueError("terminal clear bundle allowance must be positive and at most 0.25 seconds")
    clear = terminal_clear_event(events)
    if clear is None:
        return None
    after = [event for event in events[events.index(clear) + 1:]
             if event.get("state") == "notification_accepted"]
    if not after:
        return None
    idle = after[0]
    if not is_idle_notification(idle):
        raise CaptureError("terminal count-zero was not followed by the required accepted idle display")
    skew = idle["hostMonotonicNs"] - clear["hostMonotonicNs"]
    if not 0 <= skew <= round(max_skew_s * 1e9):
        raise CaptureError("terminal clear/idle bundle exceeds the 0.25 second observation allowance")
    return clear, idle


def clearance_scenario() -> dict[str, Any]:
    # Player emits the empty table plus one idle display after the last live
    # sample, retiring both independent protocol owners. --quiet-after-complete
    # then holds the BLE connection without further packets.
    return {"band": "K", "frequencyMHz": 24150, "samples": [
        {"offsetSeconds": index / 3, "strength": 6, "direction": "FRONT", "muteState": "off"}
        for index in range(16)]}


def warning_scenario() -> dict[str, Any]:
    """The measured zero-volume encounter, followed by connected packet silence."""
    return {"band": "K", "frequencyMHz": 24150, "samples": [
        {"offsetSeconds": index / 3, "strength": 6, "direction": "FRONT", "muteState": "off"}
        for index in range(28)]}


def accepted_capture_replies(events: list[dict[str, Any]], detector_volume: int) -> dict[str, Any] | None:
    """The paused audio lead must deliver the emulator's complete settings reply set.

    This is transport evidence, not a DUT acknowledgement. The subsequent
    maintenance capture independently requires a complete matching boot/session.
    """
    replies: dict[int, list[tuple[dict[str, Any], bytes]]] = {}
    for event in events:
        if event.get("state") != "notification_accepted" or event.get("characteristic") != "B2CE":
            continue
        try:
            raw = bytes.fromhex(event.get("payloadHex", ""))
        except (TypeError, ValueError):
            continue
        if (len(raw) >= 8 and raw[:3] == bytes.fromhex("AAD6EA") and raw[-1] == 0xAB
                and raw[4] == len(raw)-6 and raw[-2] == sum(raw[:-2]) % 256
                and type(event.get("hostMonotonicNs")) is int and event["hostMonotonicNs"] > 0):
            replies.setdefault(raw[3], []).append((event, raw[5:-2]))
    if not {0x02, 0x3D, 0x12, 0x23, 0x20, 0x17} <= replies.keys():
        return None
    version, volume, user, sections, maximum = [replies[key][-1] for key in (0x02, 0x3D, 0x12, 0x23, 0x20)]
    if (version[1] != b"v4.1038" or volume[1] != bytes([detector_volume, 0, detector_volume, 0])
            or len(user[1]) != 6 or len(sections[1]) != 10 or maximum[1] != b"\x01"):
        return None
    definitions = {raw[0] & 0x3F: event for event, raw in replies[0x17]
                   if len(raw) == 5 and raw[0] & 0xC0 == 0x80}
    if set(definitions) != {0, 1}:
        return None
    selected = [version[0], volume[0], user[0], sections[0], maximum[0], *definitions.values()]
    return {"accepted_reply_sequences": [event["globalTxSequence"] for event in selected],
            "latest_accepted_ns": max(event["hostMonotonicNs"] for event in selected),
            "notification_acceptance_is_dut_ack": False}


class ReplaySession:
    """One owned emulator, with bounded cleanup and optional same-link restart."""

    def __init__(self, executable: Path, out_dir: Path, scenario: str = "clearance", *,
                 authored: dict[str, Any] | None = None, detector_volume: int = 4,
                 event_callback: Callable[[dict[str, Any]], None] | None = None,
                 lease_factory: Callable[..., Any] = V1RadioLease,
                 process_factory: Callable[..., Any] = subprocess.Popen,
                 clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep):
        if scenario not in {"clearance", "persistence", "audio", "warning"}:
            raise ValueError("unknown behavior replay scenario")
        if ((scenario in {"audio", "warning"}) != (authored is not None)
                or type(detector_volume) is not int or not 0 <= detector_volume <= 9):
            raise ValueError("audio replay requires an authored input and valid detector volume")
        self.executable, self.out_dir = Path(executable).resolve(), Path(out_dir).resolve()
        if any((ancestor / ".git").exists() for ancestor in (self.out_dir, *self.out_dir.parents)):
            raise ValueError("behavior replay evidence must be outside Git checkouts")
        self.out_dir.mkdir(parents=True, mode=0o700, exist_ok=False)
        self.scenario, self.event_callback = scenario, event_callback
        self.authored, self.detector_volume = authored, detector_volume
        self._released = False
        self.lease_factory, self.process_factory = lease_factory, process_factory
        self.clock, self.sleep = clock, sleep
        self.lease = self.process = self.log_handle = None
        self.master_fd: int | None = None
        self.events: list[dict[str, Any]] = []
        self._offset, self._pending = 0, b""
        self._completion_target = 1
        self.result: dict[str, Any] | None = None
        self.log_path = self.out_dir / "v1replay.log"

    def start(self) -> None:
        if self.process is not None or self.result is not None:
            raise CaptureError("replay session cannot be started twice")
        if not self.executable.is_file() or not os.access(self.executable, os.X_OK):
            raise CaptureError("v1replay executable is missing or not executable")
        self.lease = self.lease_factory()
        self.lease.__enter__()
        slave_fd = None
        try:
            self.log_handle = self.log_path.open("xb")
            command = [str(self.executable), "bench", "--machine-events", "--quiet-after-complete",
                       "--blink-profile", "steady", "--owner-pid", str(os.getpid()),
                       "--state-file", str(self.out_dir / "detector-state.json"),
                       "--scenario-evidence", str(self.out_dir / "replay_scenario.json")]
            if self.scenario == "clearance":
                path = self.out_dir / "clearance-input.json"
                _write_json(path, clearance_scenario())
                command += ["--scenario", str(path)]
            elif self.scenario == "persistence":
                command += ["--persistence-coverage"]
            else:
                path = self.out_dir / (self.scenario + "-input.json")
                _write_json(path, self.authored)
                command += ["--scenario", str(path), "--paused", "--idle-lead", "3",
                            "--volume", f"{self.detector_volume},0"]
            _write_json(self.out_dir / "launch-intent.json", {
                "command": command, "executable_sha256": hashlib.sha256(self.executable.read_bytes()).hexdigest(),
                "host_monotonic_ns": round(self.clock() * 1e9)})
            self.master_fd, slave_fd = pty.openpty()
            self.process = self.process_factory(command, cwd=self.executable.parent.parent,
                stdin=slave_fd, stdout=self.log_handle, stderr=subprocess.STDOUT,
                start_new_session=True, pass_fds=(self.lease.fd,))
        except BaseException:
            self.finish(False)
            raise
        finally:
            if slave_fd is not None:
                os.close(slave_fd)

    def poll(self, *, final: bool = False) -> list[dict[str, Any]]:
        if self.log_path.exists():
            with self.log_path.open("rb") as log:
                log.seek(self._offset)
                chunk = log.read()
                self._offset += len(chunk)
            self._pending += chunk
        lines = self._pending.split(b"\n")
        self._pending = lines.pop()
        if final and self._pending:
            lines.append(self._pending)
            self._pending = b""
        for line in lines:
            prefix = b"V1REPLAY_EVENT "
            if prefix not in line:
                continue
            try:
                event = json.loads(line.split(prefix, 1)[1])
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise CaptureError("replay emitted malformed machine evidence") from exc
            if not isinstance(event, dict) or not isinstance(event.get("state"), str):
                raise CaptureError("replay machine evidence has no state")
            event["observerHostMonotonicNs"] = round(self.clock() * 1e9)
            self.events.append(event)
            if self.event_callback:
                self.event_callback(dict(event))
        return list(self.events)

    def health_problem(self) -> str:
        self.poll()
        if self.process is None:
            return "replay did not start"
        code = self.process.poll()
        return "" if code is None else f"replay exited early with code {code}"

    def wait_complete(self, timeout_s: float = 150, *, pump: Callable[[], None] | None = None) -> dict[str, Any]:
        if not math.isfinite(timeout_s) or timeout_s <= 0:
            raise ValueError("replay timeout must be positive")
        deadline = self.clock() + timeout_s
        while self.clock() < deadline:
            if pump:
                pump()
            problem = self.health_problem()
            if problem:
                raise CaptureError(problem)
            complete = [event for event in self.events if event["state"] == "complete"]
            if len(complete) >= self._completion_target:
                return complete[self._completion_target - 1]
            self.sleep(.05)
        raise CaptureError("replay did not complete before its deadline")

    def paused_ready(self) -> bool:
        """Require the real START handshake and accepted idle pixels' stimulus."""
        if self.scenario not in {"audio", "warning"} or self._released:
            raise CaptureError("readiness requires the owned unreleased audio replay")
        problem = self.health_problem()
        if problem:
            raise CaptureError(problem)
        if any(event["state"] == "stimulus_requested" for event in self.events):
            raise CaptureError("audio stimulus began before automatic release")
        transports = [event for event in self.events if event["state"] == "session_transport"]
        log = self.log_path.read_text(errors="replace")
        return bool(transports and transports[-1].get("active") is True
                    and "reqStartAlertData — alert table enabled" in log and "paused" in log
                    and any(is_idle_notification(event) for event in self.events)
                    and accepted_capture_replies(self.events, self.detector_volume) is not None)

    def release_after_idle(self, seconds: float = 3, *, timeout_s: float = 120,
                           pump: Callable[[], None] | None = None) -> dict[str, Any]:
        """Automatically release once a continuous connected idle is observed."""
        if not math.isfinite(seconds) or seconds < 3 or not math.isfinite(timeout_s) or timeout_s <= 0:
            raise ValueError("audio readiness requires at least three idle seconds and a bounded timeout")
        deadline = self.clock() + timeout_s
        while self.clock() < deadline:
            if pump:
                pump()
            if self.paused_ready():
                break
            self.sleep(.05)
        else:
            raise CaptureError("audio replay did not reach paused handshake readiness")
        ready = round(self.clock() * 1e9)
        capture_replies = accepted_capture_replies(self.events, self.detector_volume)
        event_index = len(self.events)
        while self.clock() < ready / 1e9 + seconds:
            if pump:
                pump()
            if not self.paused_ready() or any(event.get("state") == "session_transport"
                    and event.get("active") is False for event in self.events[event_index:]):
                raise CaptureError("audio connected-idle control lost transport")
            self.sleep(.05)
        if pump:
            pump()
        if not self.paused_ready() or any(event.get("state") == "session_transport"
                and event.get("active") is False for event in self.events[event_index:]):
            raise CaptureError("audio connected-idle control lost transport")
        if self.master_fd is None:
            raise CaptureError("audio replay has no owned input channel")
        before = round(self.clock() * 1e9)
        if self.scenario == "warning":
            zero = [event["hostMonotonicNs"] for event in self.events if is_idle_notification(event)
                    and bytes.fromhex(event["payloadHex"])[12] == 0]
            if seconds < 7 or not zero or before >= min(zero) + 14_000_000_000:
                raise CaptureError("warning replay could not release within its pre-warning idle window")
        os.write(self.master_fd, b" ")
        self._released = True
        return {"idle_started_ns": ready, "release_before_ns": before,
                "release_after_ns": round(self.clock() * 1e9),
                "capture_replies": capture_replies,
                "readiness": "accepted idle display and complete capture replies, active transport, parsed START log, paused timeline"}

    def hold_quiet(self, seconds: float = 6, *, pump: Callable[[], None] | None = None,
                   delivery_timeout_s: float = 2) -> dict[str, Any]:
        if not math.isfinite(seconds) or seconds <= 0:
            raise ValueError("quiet observation duration must be positive")
        if not math.isfinite(delivery_timeout_s) or delivery_timeout_s <= 0:
            raise ValueError("terminal delivery timeout must be positive")
        self.poll()
        if sum(event["state"] == "complete" for event in self.events) < self._completion_target:
            raise CaptureError("quiet observation requires completed playback")
        delivery_deadline = self.clock() + delivery_timeout_s
        bundle = terminal_clear_bundle(self.events)
        while bundle is None and self.clock() < delivery_deadline:
            if pump:
                pump()
            problem = self.health_problem()
            if problem:
                raise CaptureError(problem)
            bundle = terminal_clear_bundle(self.events)
            if bundle is None:
                self.sleep(.05)
        if bundle is None:
            raise CaptureError("no complete accepted terminal count-zero/idle-display bundle was observed")
        anchor, idle = bundle
        transports = [event for event in self.events if event.get("state") == "session_transport"]
        if not transports or transports[-1].get("active") is not True:
            raise CaptureError("quiet observation has no active transport evidence")
        started = self.clock()
        # Persistence belongs to the count-zero; silence begins only once the
        # following idle packet has retired the independently owned LED display.
        deadline = max(started, idle["hostMonotonicNs"] / 1e9) + seconds
        while self.clock() < deadline:
            if pump:
                pump()
            problem = self.health_problem()
            if problem:
                raise CaptureError(problem)
            self.sleep(.05)
        # Include traffic arriving during the final bounded sleep before
        # certifying the observation endpoint.
        problem = self.health_problem()
        if problem:
            raise CaptureError(problem)
        following = [event for event in self.events[self.events.index(idle) + 1:]
                     if event.get("state") == "notification_accepted"]
        anchor_index = self.events.index(anchor)
        transport_losses = [event for event in self.events[anchor_index + 1:]
                            if event.get("state") == "session_transport" and event.get("active") is False]
        if following or transport_losses:
            raise CaptureError("quiet connected tail had later traffic or lost transport")
        return {"clear_accepted_ns": anchor["hostMonotonicNs"],
                "clear_global_tx_sequence": anchor.get("globalTxSequence"),
                "idle_accepted_ns": idle["hostMonotonicNs"],
                "idle_global_tx_sequence": idle.get("globalTxSequence"),
                "quiet_started_ns": idle["hostMonotonicNs"],
                "clear_bundle_skew_ns": idle["hostMonotonicNs"] - anchor["hostMonotonicNs"],
                "max_clear_bundle_skew_s": .25,
                "observed_until_ns": round(self.clock() * 1e9),
                "anchor_kind": "host_notification_acceptance", "later_accepted_notifications": 0}

    def restart(self) -> None:
        self.poll()
        if self.master_fd is None or sum(event["state"] == "complete" for event in self.events) < self._completion_target:
            raise CaptureError("restart requires the owned completed replay")
        if self.health_problem():
            raise CaptureError("cannot restart an exited replay")
        self._completion_target += 1
        os.write(self.master_fd, b"r")

    def finish(self, completed: bool) -> dict[str, Any]:
        if self.result is not None:
            return self.result
        errors = []
        was_running = self.process is not None and self.process.poll() is None
        try:
            if was_running:
                self.process.send_signal(signal.SIGTERM)
                try:
                    self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(self.process.pid, signal.SIGKILL)
                    self.process.wait(timeout=2)
            if self.log_handle:
                self.log_handle.close()
            self.poll(final=True)
        except Exception as exc:
            errors.append(str(exc))
        finally:
            if self.log_handle and not self.log_handle.closed:
                self.log_handle.close()
            if self.master_fd is not None:
                os.close(self.master_fd)
                self.master_fd = None
            if self.lease:
                self.lease.__exit__(None, None, None)
                self.lease = None
        code = self.process.poll() if self.process else None
        graceful = bool(self.events and self.events[-1]["state"] == "stopped" and code == 0)
        complete_count = sum(event["state"] == "complete" for event in self.events)
        try:
            phases = phase_intervals(self.events)
        except CaptureError as exc:
            errors.append(str(exc))
            phases = []
        self.result = {"scenario": self.scenario, "lifecycle_completed": bool(
            completed and was_running and graceful and complete_count >= self._completion_target and not errors),
            "returncode": code, "graceful_stop_confirmed": graceful, "errors": errors,
            "complete_count": complete_count, "events": self.events,
            "accepted_clear_events": [event for event in self.events if is_clear_notification(event)],
            "phase_intervals": phases, "notification_acceptance_is_dut_ack": False}
        _write_json(self.out_dir / "behavior_replay_result.json", self.result)
        return self.result
