"""Five isolated speaker cases, with independently recorded sound and pixels.

The normal parser, coordinator and installed voice clips remain in the path.
Configuration belongs to the caller's recovery journal. This module never
flashes firmware, touches the panel or substitutes a listening acknowledgement.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import re
import time

from behavior_audio import (ANALYSIS_RATE, HOP, MicrophoneCapture, SAMPLE_RATE, file_sha, phrase_observation,
                            read_recording, reference_clips, silence_observation, verify_native_recording)
from behavior_capture import (CaptureError, RecordedFrames, ReplaySession, accepted_capture_replies, is_idle_notification,
                              terminal_clear_bundle, verified_replay_delivery)
from behavior_state import Incomplete, atomic_json
from behavior_visual import evaluate_clearance, observe_frame

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "tools/freq_audio/mulaw"
CASES = (
    {"label": "ka", "phrase": "ka", "band": "Ka", "frequency": 34700, "speaker": 97, "detector": 4},
    {"label": "detector_zero", "phrase": "k", "band": "K", "frequency": 24150, "speaker": 97, "detector": 0},
    {"label": "x", "phrase": "x", "band": "X", "frequency": 10525, "speaker": 97, "detector": 4},
    {"label": "speaker_zero", "phrase": "ka", "band": "Ka", "frequency": 34700, "speaker": 0, "detector": 4},
    {"label": "recovery", "phrase": "ka", "band": "Ka", "frequency": 34700, "speaker": 97, "detector": 4},
)
ARTIFACTS = ("replay/audio-input.json", "replay/replay_scenario.json", "replay/behavior_replay_result.json",
             "replay/launch-intent.json", "replay/v1replay.log", "microphone/capture.json",
             "microphone/intent.json", "microphone/microphone.wav", "microphone/ready.json",
             "microphone/native_summary.json", "microphone/audio_timing.ndjson")


def audio_patch(case):
    return {"voiceAlertMode": 3, "voiceVolume": case["speaker"], "voiceDirectionEnabled": True,
            "announceBogeyCount": False, "announceSecondaryAlerts": False,
            "muteVoiceIfVolZero": True, "alertVolumeFadeEnabled": False,
            "speedMuteEnabled": False, "speedMuteVoice": False, "stealthEnabled": False}


def authored_input(case):
    # Twenty-eight repeated live tables span nine seconds. The owned paused
    # session supplies the preceding three-second connected-idle control.
    return {"band": case["band"], "frequencyMHz": case["frequency"], "samples": [
        {"offsetSeconds": index / 3, "strength": 6, "direction": "FRONT", "muteState": "off"}
        for index in range(28)]}


def one_write_readback(http, endpoint, patch, expected, *, read_endpoint=None):
    """A lost response cannot trigger a duplicate mutation."""
    try:
        http.request(endpoint, patch)
    except Exception:
        pass
    actual = http.request(read_endpoint or endpoint)
    if actual != expected:
        raise Incomplete("Audio phase write did not produce its exact authoritative readback: " + endpoint)
    return actual


def configure(runner, case):
    runner.mode("maintenance")
    audio = runner.http.request("/api/audio/settings")
    patch = audio_patch(case)
    runner.journal.audio_intent(audio, patch)
    actual = one_write_readback(runner.http, "/api/audio/settings", patch, {**audio, **patch})
    slots = runner.http.request("/api/autopush/slots")
    runner.journal.autopush_intent(slots, False)
    if slots["enabled"]:
        one_write_readback(runner.http, "/api/autopush/activate",
                           {"slot": slots["activeSlot"], "enable": False}, {**slots, "enabled": False},
                           read_endpoint="/api/autopush/slots")
    slots = runner.http.request("/api/autopush/slots")
    if slots["enabled"] is not False:
        raise Incomplete("Audio phases require AutoPush disabled")
    slot = slots["activeSlot"]
    if slots["slots"][slot]["alertPersist"] != 0:
        runner.journal.validate_persistence_change(slots, slot)
        runner.journal.persistence_intent(slot, 0)
        runner.http.persistence(slot, slots, 0)
        slots = runner.http.request("/api/autopush/slots")
    if slots["slots"][slot]["alertPersist"] != 0:
        raise Incomplete("Audio phases require zero alert persistence")
    runner.http.request("/api/display/preview/clear", {})
    return {"audio": actual, "slots": slots}


def run_audio_cases(runner):
    """Collect five fresh normal boots; analyze only after capture and recovery."""
    device = getattr(runner.args, "microphone", None)
    if not device or getattr(runner.args, "real_v1_off", False) is not True:
        raise Incomplete("Audio acceptance requires an explicit microphone and real-V1-off admission")
    if runner.camera is None or runner.capture_finished:
        raise Incomplete("Audio acceptance requires the admitted continuous camera")
    runner.plan["audio"] = []
    previous_boot = None
    for case in CASES:
        label = case["label"]
        directory = runner.directory / ("audio-" + label)
        directory.mkdir(mode=0o700)
        item = {"label": label, "bundle_file": f"audio-{label}/phase.json", "completed": False}
        runner.plan["audio"].append(item)
        atomic_json(runner.directory / "observations.json", runner.plan)
        configured = configure(runner, case)
        maintenance = runner.status()
        runner.mode("normal")
        normal = runner.status()
        if (normal["mode"] != "normal" or normal["boot"] in (maintenance["boot"], previous_boot)
                or normal["persist"] != 0 or normal["enabled"] is not False):
            raise Incomplete("Audio phase did not enter a fresh normal boot with admitted settings")
        previous_boot = normal["boot"]
        microphone = MicrophoneCapture(directory / "microphone", device)
        microphone_complete = False
        try:
            microphone.start(runner.pump)
            def pump():
                runner.pump()
                problem = microphone.health_problem()
                if problem:
                    raise Incomplete(problem)
            # Recorder admission proves real samples; this additional interval
            # precedes even the emulator and supplies the acoustic noise control.
            deadline = time.monotonic() + 2.1
            while time.monotonic() < deadline:
                pump()
                time.sleep(.05)
            runner.replay = ReplaySession(ROOT / "tools/v1replay/.build/v1replay",
                directory / "replay", "audio", authored=authored_input(case), detector_volume=case["detector"])
            runner.replay.start()
            idle = runner.replay.release_after_idle(3, pump=pump)
            runner.replay.wait_complete(pump=pump)
            tail = runner.replay.hold_quiet(8, pump=pump)
            after = runner.status()
            if any(after[key] != normal[key] for key in ("git", "image", "boot", "mode", "slot", "persist", "enabled")):
                raise Incomplete("DUT identity or audio configuration changed during the cue")
            replay_result = runner.replay.finish(True)
            if not replay_result["lifecycle_completed"]:
                raise Incomplete("Audio replay did not stop cleanly")
            runner.replay = None
            microphone_complete = True
        finally:
            microphone_result = microphone.finish(microphone_complete)
        if microphone_result["result"] != "CAPTURED":
            raise Incomplete("Audio microphone recording did not finalize")
        # This captures the actual detector session from the preceding normal
        # boot and admits its last-connected address to restoration accounting.
        runner.observe_replay_target()
        target = json.loads((runner.directory / f"replay-target-{normal['boot']}.json").read_text())
        retained = {"audio": runner.http.request("/api/audio/settings"),
                    "slots": runner.http.request("/api/autopush/slots")}
        if retained != configured:
            raise Incomplete("Audio phase configuration did not survive the normal boot")
        _, assets = reference_clips(ASSETS, case["phrase"])
        proof = {"schema": 1, "case": case, "identity": runner.expected,
                 "configured": configured, "retained": retained, "normal_before": normal,
                 "normal_after": after, "maintenance_before": maintenance,
                 "microphone_device": device, "microphone_ready_ns": microphone.ready_ns,
                 "idle": idle, "tail": tail, "target_capture": target,
                 "admitted_target": dict(runner.journal.value["replay_target"]),
                 "reference_sha256": assets,
                 "artifacts": {name: file_sha(directory / name) for name in ARTIFACTS}}
        atomic_json(directory / "phase.json", proof)
        item.update(completed=True, bundle_sha256=file_sha(directory / "phase.json"))
        atomic_json(runner.directory / "observations.json", runner.plan)
    return []


def require(condition, reason):
    if not condition:
        raise Incomplete(reason)


def load_json(path, limit=8_000_000):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= limit,
            "Audio evidence is missing, linked or exceeds its read bound")
    return json.loads(path.read_text())


def accepted_display_volume(events, volume):
    displays = []
    for event in events:
        if event.get("state") != "notification_accepted" or event.get("characteristic") != "B2CE":
            continue
        try:
            raw = bytes.fromhex(event.get("payloadHex", ""))
        except (TypeError, ValueError) as error:
            raise Incomplete("Invalid accepted audio packet hex") from error
        if len(raw) < 4 or raw[3] != 0x31:
            continue
        require(len(raw) == 15 and raw[:5] == bytes.fromhex("AAD8EA3109")
                and raw[-1] == 0xAB and raw[-2] == sum(raw[:-2]) % 256,
                "Accepted detector display is not canonical")
        require(raw[12] == volume << 4, "Accepted detector volume differs from the requested audio control")
        displays.append(event)
    require(bool(displays), "No accepted display carries the detector audio control")
    return displays


def validate_phase(directory, item, case, identity):
    label = case["label"]
    require(item.get("label") == label and item.get("completed") is True
            and item.get("bundle_file") == f"audio-{label}/phase.json", "Audio case is incomplete or out of order")
    base = directory / ("audio-" + label)
    require(not base.is_symlink(), "Audio evidence directory must not be linked")
    path = base / "phase.json"
    proof = load_json(path)
    require(file_sha(path) == item.get("bundle_sha256"), "Audio phase metadata hash changed")
    require(proof.get("schema") == 1 and proof.get("case") == case and proof.get("identity") == identity,
            "Audio phase expectation or firmware identity changed")
    require(set(proof.get("artifacts", {})) == set(ARTIFACTS), "Audio artifact inventory is incomplete")
    for name in ARTIFACTS:
        artifact = base / name
        require(artifact.is_file() and not artifact.is_symlink() and not artifact.parent.is_symlink()
                and file_sha(artifact) == proof["artifacts"][name], "Audio artifact hash mismatch: " + name)
    require(load_json(base / "replay/audio-input.json") == authored_input(case), "Authored audio input changed")
    clips, assets = reference_clips(ASSETS, case["phrase"])
    require(proof.get("reference_sha256") == assets, "Audio reference assets changed")
    configured = proof.get("configured", {})
    require(configured == proof.get("retained") and isinstance(configured.get("audio"), dict),
            "Saved audio configuration was not retained")
    require(all(type(configured["audio"].get(key)) is type(value) and configured["audio"][key] == value
                for key, value in audio_patch(case).items()), "Wrong actual audio settings")
    slots = configured["slots"]
    require(slots["enabled"] is False and slots["slots"][slots["activeSlot"]]["alertPersist"] == 0,
            "AutoPush or persistence invalidates the audio control")
    before, after = proof["normal_before"], proof["normal_after"]
    require(all(before[key] == after[key] for key in ("git", "image", "boot", "mode", "slot", "persist", "enabled"))
            and before["mode"] == "normal" and before["persist"] == 0 and before["enabled"] is False
            and before["slot"] == slots["activeSlot"] and before["git"] == identity["git"]
            and before["image"] == identity["image"] and before["boot"] != proof["maintenance_before"]["boot"],
            "Audio boot identity or runtime configuration is inconsistent")
    target, admitted = proof["target_capture"], proof["admitted_target"]
    provenance = target.get("provenance", {})
    require(target.get("available") is True and provenance.get("capturedBootId") == before["boot"]
            and provenance.get("captureTimedOut") is False
            and type(provenance.get("sessionGeneration")) is int and provenance["sessionGeneration"] > 0
            and isinstance(target.get("address"), str)
            and re.fullmatch(r"[0-9A-F]{2}(?::[0-9A-F]{2}){5}", target["address"])
            and target["address"] == admitted.get("address")
            and admitted.get("captured_boot") == before["boot"]
            and admitted.get("session_generation") == provenance["sessionGeneration"],
            "Audio cue is not bound to the freshly captured replay session")
    replay = load_json(base / "replay/behavior_replay_result.json")
    delivery = verified_replay_delivery(replay, base / "replay/replay_scenario.json")
    require(delivery["completed_runs"] == 1 and delivery["requested_stimuli"] == 28,
            "Audio phase must contain exactly one complete authored encounter")
    resolved = load_json(base / "replay/replay_scenario.json")
    for index, sample in enumerate(resolved["samples"]):
        alerts = sample.get("alerts", [])
        require(sample.get("offsetSeconds") == index / 3 and sample.get("muted") is False and len(alerts) == 1
                and alerts[0].get("frequencyMHz") == case["frequency"] and alerts[0].get("strength") == 6
                and alerts[0].get("direction") == "FRONT" and alerts[0].get("priority") is True
                and alerts[0].get("junk") is False, "Resolved audio stimulus differs from its authored control")
    events = replay["events"]
    accepted_display_volume(events, case["detector"])
    requests = [row for row in events if row.get("state") == "stimulus_requested"]
    start = requests[0]["requestedHostMonotonicNs"]
    idle, tail = proof["idle"], proof["tail"]
    bundle = terminal_clear_bundle(events)
    require(bundle is not None and bundle[0]["hostMonotonicNs"] == tail["clear_accepted_ns"]
            and bundle[1]["hostMonotonicNs"] == tail["idle_accepted_ns"]
            and tail["observed_until_ns"] - tail["idle_accepted_ns"] >= 8_000_000_000
            and tail.get("later_accepted_notifications") == 0,
            "Audio phase lacks its complete eight-second connected quiet tail")
    require(not any(event.get("state") == "notification_accepted" for event in events[events.index(bundle[1])+1:]),
            "Audio quiet tail contained later notifications")
    active_start = next(index for index, row in enumerate(events) if row.get("state") == "session_transport"
                        and row.get("active") is True)
    # Graceful shutdown may report inactive after the observation endpoint.
    losses = [event for event in events[active_start+1:] if event.get("state") == "session_transport"
              and event.get("active") is False
              and event.get("observerHostMonotonicNs", 0) < tail["observed_until_ns"]]
    require(not losses, "Audio phase lost its connected transport")
    require(any(is_idle_notification(event) and event["hostMonotonicNs"] <= idle["idle_started_ns"]
                for event in events), "Audio phase has no accepted independent idle control")
    capture_replies = accepted_capture_replies(
        [event for event in events if event.get("hostMonotonicNs", idle["idle_started_ns"] + 1)
         <= idle["idle_started_ns"]], case["detector"])
    require(capture_replies is not None and capture_replies == idle.get("capture_replies"),
            "Audio release preceded complete accepted settings-capture replies")
    log = (base / "replay/v1replay.log").read_text(errors="replace")
    require("reqStartAlertData — alert table enabled" in log and "paused" in log,
            "Audio paused readiness lacks its actual START handshake")
    launch = load_json(base / "replay/launch-intent.json")
    command = launch.get("command", [])
    require("bench" in command and "--paused" in command and "--owner-pid" in command
            and "--idle-lead" in command and command[command.index("--idle-lead")+1] == "3"
            and "--volume" in command and command[command.index("--volume")+1] == f'{case["detector"]},0',
            "Audio replay launch was not owned, paused and volume-pinned")
    capture = load_json(base / "microphone/capture.json")
    intent = load_json(base / "microphone/intent.json")
    native_ready = load_json(base / "microphone/ready.json")
    require(capture.get("result") == "CAPTURED" and capture.get("collection_completed") is True
            and capture.get("returncode") == 0 and not capture.get("errors")
            and capture.get("recorder_backend") == intent.get("recorder_backend") == "avfoundation_native"
            and capture.get("device") == intent.get("device") == proof["microphone_device"]
            and capture.get("wav_sha256") == proof["artifacts"]["microphone/microphone.wav"],
            "Microphone acquisition is incomplete or its source changed")
    samples = read_recording(base / "microphone/microphone.wav")
    native = verify_native_recording(base / "microphone", len(samples))
    require(capture.get("first_host_ns") == native["first_host_ns"]
            and capture.get("last_host_ns") == native["last_host_ns"]
            and capture.get("native_summary_sha256") == proof["artifacts"]["microphone/native_summary.json"]
            and capture.get("timing_sha256") == proof["artifacts"]["microphone/audio_timing.ndjson"]
            and native.get("device") == proof["microphone_device"]
            and native.get("sample_rate") == SAMPLE_RATE and native.get("channels") == 1
            and native.get("bits_per_channel") == 16
            and abs(len(samples) / SAMPLE_RATE - capture["duration_s"]) < 1 / SAMPLE_RATE,
            "Microphone source timestamps, format, counts or hashes do not match native evidence")
    ready = proof["microphone_ready_ns"]
    require(capture.get("ready_host_ns") == ready
            and native_ready.get("schema_version") == 1 and native_ready.get("event") == "ready"
            and native_ready.get("source_clock") == native["source_clock"]
            and native_ready.get("first_host_ns") == native["first_host_ns"]
            and native_ready.get("ready_host_ns") == native["ready_host_ns"]
            and native_ready.get("device") == native["device"]
            and native_ready.get("source_format") == native["source_format"]
            and native_ready.get("output_format") == {"sample_rate": SAMPLE_RATE, "channels": 1, "bits_per_channel": 16}
            and type(capture.get("ready_media_s")) in (int, float)
            and math.isfinite(capture["ready_media_s"])
            and 0 < capture["ready_media_s"] == native_ready.get("source_duration_s") <= native["source_duration_s"],
            "Microphone admission lacks matching native source readiness")
    timestamps = [capture.get(key) for key in ("started_host_ns", "first_host_ns", "last_host_ns",
                  "stop_requested_host_ns", "finished_host_ns", "ready_host_ns")]
    require(all(type(value) is int and value > 0 for value in timestamps)
            and type(native["ready_host_ns"]) is int and type(native.get("finished_host_ns")) is int
            and capture["started_host_ns"] == intent["started_host_ns"] <= native["first_host_ns"]
            and native["first_host_ns"] + round(capture["ready_media_s"] * 1e9) <= native["ready_host_ns"] <= ready
            and launch["host_monotonic_ns"] - ready >= 2_000_000_000
            and launch["host_monotonic_ns"] <= idle["idle_started_ns"]
            and idle["release_before_ns"] - idle["idle_started_ns"] >= 3_000_000_000
            and idle["release_before_ns"] <= idle["release_after_ns"] <= start
            and start < tail["clear_accepted_ns"] < tail["observed_until_ns"] <= native["last_host_ns"]
            <= native["finished_host_ns"]
            and tail["observed_until_ns"] <= capture["stop_requested_host_ns"]
            <= native["finished_host_ns"] <= capture["finished_host_ns"]
            and capture["stop_requested_host_ns"] - native["last_host_ns"] <= 250_000_000,
            "Microphone baseline, idle, cue or quiet-tail timing is invalid")
    sample_ns = math.ceil(1e9 / SAMPLE_RATE)
    origin = [native["origin_min_host_ns"], native["origin_max_host_ns"]]
    require(all(type(value) is int and value > 0 for value in origin) and origin[0] <= origin[1],
            "Microphone source clock has no bounded origin interval")
    proof["audio_source_origin_bounds_ns"] = origin
    proof["audio_origin_bounds_ns"] = [origin[0] - sample_ns, origin[1] + sample_ns]
    require(proof["audio_origin_bounds_ns"][0] + round(len(samples) / SAMPLE_RATE * 1e9)
            >= tail["observed_until_ns"], "Microphone earliest source end does not cover the quiet tail")
    return proof, samples, clips, delivery


def audible_case(proof, samples, clips, delivery):
    result = phrase_observation(samples, clips)
    if result["result"] != "PASS":
        return result
    feature_ns = math.ceil(HOP / ANALYSIS_RATE * 1e9)
    bounds = [proof["audio_origin_bounds_ns"][0] + round(result["start_s"] * 1e9) - feature_ns,
              proof["audio_origin_bounds_ns"][1] + round(result["start_s"] * 1e9) + feature_ns]
    result = {**result, "host_onset_bounds_ns": bounds,
              "feature_alignment_allowance_ns": feature_ns,
              "timing_kind": "measured source-clock range, output-sample rounding and one feature-hop allowance; not acoustic latency"}
    live_start = delivery["phase_intervals"][0]["request_start_ns"]
    end_latest = bounds[1] + round(result["duration_s"] * 1e9)
    # Every possible onset/end must fit: mere overlap cannot identify whether
    # sound preceded the cue or outlived the retained observation.
    if bounds[0] < live_start or end_latest > proof["tail"]["observed_until_ns"]:
        result.update(result="INCOMPLETE", reason="Identified sound may lie outside the observed detector cue")
    return result


def visual_case(frames, proof):
    def observations(start, end):
        return [{"time_s": sample["host_monotonic_ns"] / 1e9,
                 "video_pts_seconds": sample["video_pts_seconds"],
                 **observe_frame(sample["rgb"], frames.registration)}
                for sample in frames.decoded_samples(round(start), round(end), step_s=.2)]
    idle = proof["idle"]["idle_started_ns"]
    control = observations(idle + 1_000_000_000, idle + 2_200_000_000)
    clear = proof["tail"]["clear_accepted_ns"]
    observed = observations(clear - 1_200_000_000, clear + 1_700_000_000)
    frequency = f'{proof["case"]["frequency"] / 1000:.3f}'
    result = evaluate_clearance(observed, control, active_window=(clear/1e9-1, clear/1e9-.4),
                               clear_time_s=clear/1e9, persistence_s=0, expected_frequency=frequency)
    return result, {"idle": control, "samples": observed}


def analyze_audio_cases(directory):
    """Always return the same ten outcomes; missing evidence cannot disappear."""
    directory = Path(directory)
    acoustic, visual, valid = {}, {}, {}
    try:
        plan = load_json(directory / "observations.json")
        require(plan.get("capture_completed") is True, "The continuous camera did not finish")
        require(isinstance(plan.get("audio"), list) and len(plan["audio"]) == len(CASES),
                "All five ordered audio cases are required")
        frames = RecordedFrames(directory / "camera")
        boot_ids, addresses = set(), set()
        for case, item in zip(CASES, plan["audio"]):
            label = case["label"]
            proof, samples, clips, delivery = validate_phase(directory, item, case, plan["identity"])
            require(proof["normal_before"]["boot"] not in boot_ids, "Audio cases reused a normal boot")
            boot_ids.add(proof["normal_before"]["boot"])
            addresses.add(proof["admitted_target"]["address"])
            require(len(addresses) == 1, "Audio cases changed replay target identity")
            valid[label] = (proof, samples, clips)
            visual[label], pixels = visual_case(frames, proof)
            atomic_json(directory / f"audio-{label}/pixels.json", pixels)
            atomic_json(directory / f"audio-{label}/delivery.json", delivery)
            if case["speaker"] and case["detector"]:
                acoustic[label] = audible_case(proof, samples, clips, delivery)
        for label, controls in (("detector_zero", ("ka", "x")), ("speaker_zero", ("x", "recovery"))):
            if visual[label]["result"] != "PASS" or any(visual[name]["result"] != "PASS"
                    or acoustic[name]["result"] != "PASS" for name in controls):
                acoustic[label] = {"result": "INCOMPLETE", "reason": "Silence requires camera-confirmed live alerts and both positive controls"}
            else:
                acoustic[label] = silence_observation(valid[label][1], [acoustic[name] for name in controls])
    except (OSError, ValueError, TypeError, KeyError, StopIteration, Incomplete, CaptureError) as error:
        reason = str(error) or type(error).__name__
        for case in CASES:
            acoustic.setdefault(case["label"], {"result": "INCOMPLETE", "reason": reason})
            visual.setdefault(case["label"], {"result": "INCOMPLETE", "reason": reason})
    checks = []
    for case in CASES:
        label = case["label"]
        checks.extend(({"check": "audio_" + label, **acoustic[label]},
                       {"check": "audio_" + label + "_visual", **visual[label]}))
    atomic_json(directory / "audio-result.json", {"checks": checks,
                "result": "FAIL" if any(row["result"] == "FAIL" for row in checks) else
                          "PASS" if all(row["result"] == "PASS" for row in checks) else "INCOMPLETE"})
    return checks
