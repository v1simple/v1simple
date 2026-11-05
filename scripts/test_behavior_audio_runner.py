#!/usr/bin/env python3
"""Host-only evidence admission and orchestration tests; no device is opened."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts/bench"))
sys.path.insert(0, str(ROOT / "scripts"))
import behavior_audio_runner as subject
import behavior_capture
from test_behavior_capture import Clock, Lease, Process


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))


def packet(payload):
    raw = bytes(payload)
    return raw + bytes([sum(raw) % 256, 0xAB])


def notification(raw, when, sequence, **extra):
    return {"state": "notification_accepted", "characteristic": "B2CE", "payloadHex": raw.hex(),
            "payloadSha256": hashlib.sha256(raw).hexdigest(), "hostMonotonicNs": when,
            "globalTxSequence": sequence, **extra}


def capture_replies(volume, when, first_sequence=2):
    payloads = [(0x02, list(b"v4.1038")), (0x3D, [volume, 0, volume, 0]),
                (0x12, [0x7F, 255, 255, 255, 255, 255]),
                (0x23, [0x12, 0x61, 0xA8, 0x5D, 0xC0, 0x22, 0x8C, 0xA0, 0x80, 0xE8]),
                (0x20, [1]), (0x17, [0x80, 0x5E, 0x56, 0x5D, 0xF2]),
                (0x17, [0x81, 0x85, 0x98, 0x85, 0x34])]
    return [notification(packet([0xAA, 0xD6, 0xEA, identifier, len(payload)+1, *payload]),
                         when, first_sequence+index) for index, (identifier, payload) in enumerate(payloads)]


def native_recording_receipt(base, ns):
    directory = base / "microphone"
    source_format = {"sample_rate": 48000, "channels": 1, "bits_per_channel": 16,
                     "format_id": 1819304813, "format_flags": 12, "bytes_per_frame": 2}
    rows = [{"schema_version": 1, "event": "audio_buffer", "buffer_seq": index+1,
             "status": "written", "source_format": source_format, "source_sample_frames": 960,
             "source_pts_value": index*960, "source_pts_timescale": 48000,
             "source_duration_value": 960, "source_duration_timescale": 48000,
             "audio_pts_value": index*960, "audio_pts_timescale": 48000,
             "host_start_ns": ns(index*.02), "host_end_ns": ns((index+1)*.02),
             "callback_host_ns": ns((index+1)*.02+.001)} for index in range(1250)]
    (directory / "audio_timing.ndjson").write_text("".join(json.dumps(row)+"\n" for row in rows))
    ready = {"schema_version": 1, "event": "ready", "device": "test microphone",
             "source_clock": "avcapture_session_synchronization_clock", "first_host_ns": ns(0),
             "ready_host_ns": ns(.025), "source_duration_s": .02, "source_format": source_format,
             "output_format": {"sample_rate": subject.SAMPLE_RATE, "channels": 1, "bits_per_channel": 16}}
    save(directory / "ready.json", ready)
    summary = {"schema_version": 1, "event": "finished", "device": "test microphone",
               "result": "CAPTURED", "reason": "none", "shutdown_reason": "q",
               "source_clock": ready["source_clock"], "first_host_ns": ns(0), "last_host_ns": ns(25),
               "ready_host_ns": ready["ready_host_ns"], "finished_host_ns": ns(25.08),
               "source_format": source_format, "source_buffers": len(rows), "source_sample_frames": 25*48000,
               "source_duration_s": 25, "encoded_sample_frames": 25*subject.SAMPLE_RATE, "duration_s": 25,
               "sample_rate": subject.SAMPLE_RATE, "channels": 1, "bits_per_channel": 16,
               "timing_errors": 0, "writer_drops": 0}
    save(directory / "native_summary.json", summary)
    save(directory / "capture.json", {"result": "CAPTURED", "returncode": 0, "errors": [],
         "collection_completed": True, "device": "test microphone", "duration_s": 25,
         "recorder_backend": "avfoundation_native", "first_host_ns": ns(0), "last_host_ns": ns(25),
         "started_host_ns": ns(-.1), "finished_host_ns": ns(25.1), "stop_requested_host_ns": ns(25.05),
         "ready_host_ns": ns(1), "ready_media_s": .02,
         "native_summary_sha256": subject.file_sha(directory / "native_summary.json"),
         "timing_sha256": subject.file_sha(directory / "audio_timing.ndjson"),
         "wav_sha256": subject.file_sha(directory / "microphone.wav")})
    save(directory / "intent.json", {"device": "test microphone", "started_host_ns": ns(-.1),
                                      "recorder_backend": "avfoundation_native"})


def fixture(directory, case, number=0):
    """Complete synthetic receipt with real files, WAV contract and delivery validation."""
    base = directory / ("audio-" + case["label"])
    base.mkdir()
    epoch = (100 + number * 100) * 1_000_000_000
    ns = lambda value: epoch + round(value * 1e9)
    identity = {"git": "1234567", "image": "a" * 64}
    normal = {**identity, "boot": number * 2 + 2, "mode": "normal", "slot": 0, "persist": 0, "enabled": False}
    samples = [{"sourceIndex": index, "offsetSeconds": index / 3, "phase": "external", "muted": False,
                "alerts": [{"band": case["band"], "bandMask": {"Ka": 8, "K": 4, "X": 2}[case["band"]],
                            "frequencyMHz": case["frequency"], "strength": 6, "direction": "FRONT",
                            "directionMask": 1, "priority": True, "junk": False, "photoType": 0}]}
               for index in range(28)]
    resolved = {"schemaVersion": 2, "origin": "external_input", "samples": samples}
    save(base / "replay/replay_scenario.json", resolved)
    raw = (base / "replay/replay_scenario.json").read_bytes()
    sha = hashlib.sha256(raw).hexdigest()
    events = [{"state": "scenario_resolved", "sha256": sha, "byteCount": len(raw), "sampleCount": 28,
               "origin": "external_input"},
              {"state": "configured", "scenarioSha256": sha, "totalSamples": 28,
               "scenarioOrigin": "external_input", "blinkProfile": "steady"},
              {"state": "session_transport", "active": True, "observerHostMonotonicNs": ns(3.5)}]
    idle_packet = packet([0xAA, 0xD8, 0xEA, 0x31, 9, 0x77, 0x77, 0, 0, 0, 0xC, 4, case["detector"] << 4])
    events.append(notification(idle_packet, ns(3.8), 1))
    events.extend(capture_replies(case["detector"], ns(3.9)))
    sequence = 8
    for index, sample in enumerate(samples):
        when = ns(7.05 + index / 3)
        row = sample["alerts"][0]
        row_packet = packet([0xAA, 0xD8, 0xEA, 0x43, 8, 0x11, case["frequency"] >> 8,
                             case["frequency"] & 255, 0x61, row["bandMask"], 0, 0])
        display = packet([0xAA, 0xD8, 0xEA, 0x31, 9, 0x30, 0x30, 1, 1, 1, 0xC, 4, case["detector"] << 4])
        notifications = [
            {"ordinal": 0, "channel": "display_short", "kind": "alert_row", "bytesHex": row_packet.hex(),
             "alertRowIndex": 1, "alertRowCount": 1},
            {"ordinal": 1, "channel": "display_short", "kind": "display_frame", "bytesHex": display.hex(),
             "alertRowIndex": None, "alertRowCount": None}]
        events.append({"state": "stimulus_requested", "stimulusSequence": index+1, "sourceIndex": index,
                       "requestedHostMonotonicNs": when, "replayOffsetSeconds": index/3,
                       "expected": {"phase": "external", "muted": False, "displayOn": True,
                                    "arrowBlink": False, "bandBlink": False,
                                    "alerts": [{**row, "bars": 6}]}, "notifications": notifications})
        for ordinal, payload in enumerate((row_packet, display)):
            sequence += 1
            events.append(notification(payload, when + (ordinal+1)*1_000_000, sequence,
                                       stimulusSequence=index+1, emissionOrdinal=ordinal))
    clear = packet([0xAA, 0xD8, 0xEA, 0x43, 8, 0, 0, 0, 0, 0, 0, 0])
    events += [notification(clear, ns(16.1), sequence+1), notification(idle_packet, ns(16.11), sequence+2),
               {"state": "complete"}, {"state": "stopped"}]
    save(base / "replay/behavior_replay_result.json", {"lifecycle_completed": True, "complete_count": 1,
                                                      "events": events})
    save(base / "replay/audio-input.json", subject.authored_input(case))
    save(base / "replay/launch-intent.json", {"command": ["v1replay", "bench", "--paused", "--owner-pid", "123",
                                                         "--idle-lead", "3",
                                                         "--volume", f'{case["detector"]},0'],
                                             "host_monotonic_ns": ns(3.1)})
    (base / "replay/v1replay.log").write_text("paused\nreqStartAlertData — alert table enabled\n")
    (base / "microphone").mkdir()
    with wave.open(str(base / "microphone/microphone.wav"), "wb") as stream:
        stream.setnchannels(1); stream.setsampwidth(2); stream.setframerate(subject.SAMPLE_RATE)
        stream.writeframes(bytes(25 * subject.SAMPLE_RATE * 2))
    native_recording_receipt(base, ns)
    _, assets = subject.reference_clips(subject.ASSETS, case["phrase"])
    config = {"audio": subject.audio_patch(case), "slots": {"enabled": False, "activeSlot": 0,
              "slots": [{"alertPersist": 0}, {"alertPersist": 0}, {"alertPersist": 0}]}}
    proof = {"schema": 1, "case": case, "identity": identity, "configured": config, "retained": deepcopy(config),
             "normal_before": normal, "normal_after": deepcopy(normal),
             "maintenance_before": {**normal, "boot": normal["boot"]-1, "mode": "maintenance"},
             "microphone_device": "test microphone", "microphone_ready_ns": ns(1),
             "idle": {"idle_started_ns": ns(4), "release_before_ns": ns(7), "release_after_ns": ns(7.001),
                      "capture_replies": behavior_capture.accepted_capture_replies(events, case["detector"])},
             "tail": {"clear_accepted_ns": ns(16.1), "idle_accepted_ns": ns(16.11),
                      "observed_until_ns": ns(24.2), "later_accepted_notifications": 0},
             "target_capture": {"available": True, "address": "AA:BB:CC:DD:EE:FF",
                                "provenance": {"capturedBootId": normal["boot"], "captureTimedOut": False,
                                               "sessionGeneration": number+1}},
             "admitted_target": {"address": "AA:BB:CC:DD:EE:FF", "captured_boot": normal["boot"],
                                 "session_generation": number+1}, "reference_sha256": assets}
    item = {"label": case["label"], "bundle_file": f'audio-{case["label"]}/phase.json', "completed": True}
    reseal(base, proof, item)
    return base, proof, item, identity


def reseal(base, proof, item):
    proof["artifacts"] = {name: subject.file_sha(base / name) for name in subject.ARTIFACTS}
    save(base / "phase.json", proof)
    item["bundle_sha256"] = subject.file_sha(base / "phase.json")


class PhaseAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.case = subject.CASES[0]
        self.base, self.proof, self.item, self.identity = fixture(self.directory, self.case)

    def validate(self):
        return subject.validate_phase(self.directory, self.item, self.case, self.identity)

    def test_complete_receipt_validates_real_files_and_accepted_packets(self):
        proof, samples, clips, delivery = self.validate()
        self.assertEqual(len(samples), 25 * subject.SAMPLE_RATE)
        self.assertEqual(delivery["requested_stimuli"], 28)
        self.assertEqual(len(clips), 5)
        allowance = subject.math.ceil(1e9 / subject.SAMPLE_RATE) + 1
        self.assertEqual(proof["audio_origin_bounds_ns"], [100_000_000_000-allowance, 100_000_000_000+allowance])

    def test_legacy_progress_only_recording_cannot_claim_native_capture(self):
        path = self.base / "microphone/capture.json"
        value = json.loads(path.read_text()); value["recorder_backend"] = "ffmpeg"
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "acquisition is incomplete"):
            self.validate()

    def test_resealed_missing_source_buffer_cannot_pass_continuity(self):
        path = self.base / "microphone/audio_timing.ndjson"
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        del rows[100]
        for index, row in enumerate(rows, 1):
            row["buffer_seq"] = index
        path.write_text("".join(json.dumps(row)+"\n" for row in rows))
        capture_path = self.base / "microphone/capture.json"
        capture = json.loads(capture_path.read_text()); capture["timing_sha256"] = subject.file_sha(path)
        save(capture_path, capture); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "missing, overlapping or mistimed"):
            self.validate()

    def test_native_summary_must_account_for_actual_wav_samples(self):
        path = self.base / "microphone/native_summary.json"
        value = json.loads(path.read_text()); value["encoded_sample_frames"] -= 1
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "differs from the WAV"):
            self.validate()

    def test_capture_source_origin_cannot_override_native_timeline(self):
        path = self.base / "microphone/capture.json"
        value = json.loads(path.read_text()); value["first_host_ns"] += 1_000_000_000
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "do not match native evidence"):
            self.validate()

    def test_source_audio_must_cover_tail_even_when_process_finished_later(self):
        self.proof["tail"]["observed_until_ns"] = 125_010_000_000
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "timing is invalid"):
            self.validate()

    def test_native_ready_from_another_device_cannot_bind_phase(self):
        path = self.base / "microphone/ready.json"
        value = json.loads(path.read_text()); value["device"] = "another microphone"
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "native source readiness"):
            self.validate()

    def test_final_buffer_during_graceful_stop_remains_valid(self):
        # Preserve the actual admission-03 deltas: source end is1.028ms after
        # stop request; callback and native finish follow while q drains.
        path = self.base / "microphone/capture.json"
        value = json.loads(path.read_text())
        value["stop_requested_host_ns"] = value["last_host_ns"] - 1_027_881
        value["finished_host_ns"] = value["last_host_ns"] + 56_357_285
        summary_path = self.base / "microphone/native_summary.json"
        summary = json.loads(summary_path.read_text())
        summary["finished_host_ns"] = value["last_host_ns"] + 18_162_827
        save(summary_path, summary)
        value["native_summary_sha256"] = subject.file_sha(summary_path)
        save(path, value); reseal(self.base, self.proof, self.item)
        self.validate()

    def test_stop_request_before_tail_cannot_be_hidden_by_late_buffer(self):
        path = self.base / "microphone/capture.json"
        value = json.loads(path.read_text()); value["stop_requested_host_ns"] = 124_100_000_000
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "timing is invalid"):
            self.validate()

    def test_native_finish_before_requested_stop_is_rejected(self):
        summary_path = self.base / "microphone/native_summary.json"
        summary = json.loads(summary_path.read_text()); summary["finished_host_ns"] = 125_040_000_000
        save(summary_path, summary)
        path = self.base / "microphone/capture.json"
        value = json.loads(path.read_text()); value["native_summary_sha256"] = subject.file_sha(summary_path)
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "timing is invalid"):
            self.validate()

    def test_source_end_after_native_finish_is_rejected(self):
        summary_path = self.base / "microphone/native_summary.json"
        summary = json.loads(summary_path.read_text()); summary["finished_host_ns"] = 124_999_000_000
        save(summary_path, summary)
        path = self.base / "microphone/capture.json"
        value = json.loads(path.read_text())
        value["native_summary_sha256"] = subject.file_sha(summary_path)
        value["stop_requested_host_ns"] = 124_998_000_000
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "timing is invalid"):
            self.validate()

    def test_source_clock_variation_is_bounded_without_inventing_missing_pcm(self):
        timing = self.base / "microphone/audio_timing.ndjson"
        rows = [json.loads(line) for line in timing.read_text().splitlines()]
        for row in rows[1000:]:
            row["host_start_ns"] += 10_000_000
            row["host_end_ns"] += 10_000_000
            row["callback_host_ns"] += 10_000_000
        timing.write_text("".join(json.dumps(row)+"\n" for row in rows))
        summary_path = self.base / "microphone/native_summary.json"
        summary = json.loads(summary_path.read_text()); summary["last_host_ns"] += 10_000_000
        save(summary_path, summary)
        capture_path = self.base / "microphone/capture.json"
        capture = json.loads(capture_path.read_text()); capture["last_host_ns"] = summary["last_host_ns"]
        capture.update(native_summary_sha256=subject.file_sha(summary_path), timing_sha256=subject.file_sha(timing))
        save(capture_path, capture); reseal(self.base, self.proof, self.item)
        proof, _, _, _ = self.validate()
        self.assertEqual(proof["audio_source_origin_bounds_ns"], [99_999_999_999, 100_010_000_001])

    def test_earliest_pcm_end_must_cover_tail_despite_later_last_source_stamp(self):
        # The last converted timestamp reaches25s, but output-sample rounding
        # means that an endpoint at25s is not wholly covered.
        self.proof["tail"]["observed_until_ns"] = 125_000_000_000
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "earliest source end"):
            self.validate()

    def test_changed_recording_cannot_reuse_metadata_hash(self):
        path = self.base / "microphone/microphone.wav"
        path.write_bytes(path.read_bytes() + b"corruption")
        with self.assertRaisesRegex(subject.Incomplete, "hash mismatch"):
            self.validate()

    def test_resealed_wrong_setting_is_rejected(self):
        self.proof["configured"]["audio"]["voiceVolume"] = 31
        self.proof["retained"] = deepcopy(self.proof["configured"])
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "actual audio settings"):
            self.validate()

    def test_zero_volume_boolean_cannot_substitute_integer(self):
        self.proof["configured"]["audio"]["voiceAlertMode"] = True
        self.proof["retained"] = deepcopy(self.proof["configured"])
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "actual audio settings"):
            self.validate()

    def test_stale_target_capture_cannot_bind_fresh_normal_boot(self):
        self.proof["target_capture"]["provenance"]["capturedBootId"] -= 1
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "freshly captured"):
            self.validate()

    def test_reboot_during_cue_is_rejected(self):
        self.proof["normal_after"]["boot"] += 1
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "boot identity"):
            self.validate()

    def test_incomplete_microphone_cannot_be_promoted_by_completed_plan(self):
        path = self.base / "microphone/capture.json"
        value = json.loads(path.read_text()); value["result"] = "INCOMPLETE"
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "acquisition is incomplete"):
            self.validate()

    def test_short_noise_baseline_is_rejected(self):
        self.proof["microphone_ready_ns"] += 1_000_000_000
        path = self.base / "microphone/capture.json"
        value = json.loads(path.read_text()); value["ready_host_ns"] = self.proof["microphone_ready_ns"]
        save(path, value)
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "timing is invalid"):
            self.validate()

    def test_short_connected_idle_is_rejected(self):
        self.proof["idle"]["idle_started_ns"] += 1
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "timing is invalid"):
            self.validate()

    def test_short_quiet_tail_is_rejected(self):
        self.proof["tail"]["observed_until_ns"] -= 1_000_000_000
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "eight-second"):
            self.validate()

    def test_missing_accepted_row_cannot_pass_complete_counter(self):
        path = self.base / "replay/behavior_replay_result.json"
        value = json.loads(path.read_text())
        value["events"] = [row for row in value["events"] if row.get("globalTxSequence") != 10]
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(behavior_capture.CaptureError, "no acceptance"):
            self.validate()

    def test_start_without_max_index_reply_is_not_capture_readiness(self):
        path = self.base / "replay/behavior_replay_result.json"
        value = json.loads(path.read_text())
        value["events"] = [row for row in value["events"] if row.get("globalTxSequence") != 6]
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "capture replies"):
            self.validate()

    def test_terminal_volume_overwrite_is_detected_even_after_live_delivery(self):
        path = self.base / "replay/behavior_replay_result.json"
        value = json.loads(path.read_text())
        event = [row for row in value["events"] if row.get("state") == "notification_accepted"][-1]
        raw = bytearray.fromhex(event["payloadHex"]); raw[12] = 0; raw[-2] = sum(raw[:-2]) % 256
        event["payloadHex"] = raw.hex(); event["payloadSha256"] = hashlib.sha256(raw).hexdigest()
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "detector volume"):
            self.validate()

    def test_omitted_start_handshake_is_rejected(self):
        (self.base / "replay/v1replay.log").write_text("paused\n")
        reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "START handshake"):
            self.validate()

    def test_wrong_authored_frequency_is_rejected_even_with_resealed_hash(self):
        path = self.base / "replay/audio-input.json"
        value = json.loads(path.read_text()); value["frequencyMHz"] = 33800
        save(path, value); reseal(self.base, self.proof, self.item)
        with self.assertRaisesRegex(subject.Incomplete, "Authored audio input changed"):
            self.validate()

    def test_identified_phrase_before_replay_does_not_pass(self):
        proof, samples, clips, delivery = self.validate()
        with mock.patch.object(subject, "phrase_observation", return_value={"result": "PASS", "start_s": 3, "duration_s": 2}):
            result = subject.audible_case(proof, samples, clips, delivery)
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertIn("outside", result["reason"])

    def test_identified_phrase_inside_cue_retains_timing_bounds(self):
        proof, samples, clips, delivery = self.validate()
        with mock.patch.object(subject, "phrase_observation", return_value={"result": "PASS", "start_s": 7.5, "duration_s": 2}):
            result = subject.audible_case(proof, samples, clips, delivery)
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(len(result["host_onset_bounds_ns"]), 2)

    def test_onset_uncertainty_straddling_cue_cannot_pass(self):
        proof, samples, clips, delivery = self.validate()
        with mock.patch.object(subject, "phrase_observation", return_value={"result": "PASS", "start_s": 7.055, "duration_s": 1}):
            result = subject.audible_case(proof, samples, clips, delivery)
        # Nominal onset is inside, but one feature hop straddles the cue.
        self.assertEqual(result["result"], "INCOMPLETE")

    def test_phrase_end_uncertainty_straddling_tail_cannot_pass(self):
        proof, samples, clips, delivery = self.validate()
        with mock.patch.object(subject, "phrase_observation", return_value={"result": "PASS", "start_s": 23.195, "duration_s": 1}):
            result = subject.audible_case(proof, samples, clips, delivery)
        # Nominal end is inside, but one feature hop straddles the tail.
        self.assertEqual(result["result"], "INCOMPLETE")

    def test_contained_phrase_accepts_exact_inclusive_window_boundaries(self):
        proof, samples, clips, delivery = self.validate()
        allowance = (subject.math.ceil(1e9 / subject.SAMPLE_RATE) + 1
                     + subject.math.ceil(subject.HOP / subject.ANALYSIS_RATE * 1e9)) / 1e9
        for onset, duration in ((7.05 + allowance, 1), (23.2 - allowance, 1)):
            with self.subTest(onset=onset), mock.patch.object(subject, "phrase_observation",
                    return_value={"result": "PASS", "start_s": onset, "duration_s": duration}):
                result = subject.audible_case(proof, samples, clips, delivery)
            self.assertEqual(result["result"], "PASS")


class ChecklistTests(unittest.TestCase):
    def test_missing_plan_still_returns_all_ten_incomplete_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subject.analyze_audio_cases(Path(directory))
        self.assertEqual(len(result), 10)
        self.assertEqual(len({row["check"] for row in result}), 10)
        self.assertTrue(all(row["result"] == "INCOMPLETE" for row in result))

    def run_case(self, *, duplicate_boot=False, silence_visual="PASS", positive="PASS"):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            items = []
            for number, case in enumerate(subject.CASES):
                base, proof, item, identity = fixture(directory, case, number)
                if duplicate_boot and number == 4:
                    for key in ("normal_before", "normal_after"):
                        proof[key]["boot"] = 2
                    proof["target_capture"]["provenance"]["capturedBootId"] = 2
                    proof["admitted_target"]["captured_boot"] = 2
                    reseal(base, proof, item)
                items.append(item)
            save(directory / "observations.json", {"capture_completed": True, "audio": items, "identity": identity})
            def visual(_frames, proof):
                result = silence_visual if proof["case"]["label"] == "detector_zero" else "PASS"
                return {"result": result}, {}
            with mock.patch.object(subject, "RecordedFrames"), mock.patch.object(subject, "visual_case", side_effect=visual), \
                 mock.patch.object(subject, "phrase_observation", return_value={"result": positive, "start_s": 7.5, "duration_s": 2}) as phrase, \
                 mock.patch.object(subject, "silence_observation", return_value={"result": "PASS"}) as silence:
                result = subject.analyze_audio_cases(directory)
                return result, phrase.call_count, silence.call_count

    def test_complete_five_phase_receipts_supply_fixed_checks_and_bracketing_controls(self):
        result, phrase, silence = self.run_case()
        self.assertTrue(all(row["result"] == "PASS" for row in result), result)
        self.assertEqual((phrase, silence), (3, 2))

    def test_silent_case_without_actual_visible_alert_cannot_pass(self):
        result, _, silence = self.run_case(silence_visual="FAIL")
        checks = {row["check"]: row for row in result}
        self.assertEqual(checks["audio_detector_zero"]["result"], "INCOMPLETE")
        self.assertEqual(checks["audio_detector_zero_visual"]["result"], "FAIL")
        self.assertEqual(silence, 1)

    def test_reused_boot_invalidates_remaining_phase_instead_of_changing_checklist(self):
        result, _, _ = self.run_case(duplicate_boot=True)
        self.assertEqual(len(result), 10)
        self.assertEqual(result[-2]["result"], "INCOMPLETE")

    def test_failed_positive_controls_block_both_silence_verdicts(self):
        result, _, silence = self.run_case(positive="FAIL")
        checks = {row["check"]: row for row in result}
        self.assertEqual(checks["audio_detector_zero"]["result"], "INCOMPLETE")
        self.assertEqual(checks["audio_speaker_zero"]["result"], "INCOMPLETE")
        self.assertEqual(silence, 0)


class WriteAndReplayTests(unittest.TestCase):
    def test_failed_replay_always_finishes_microphone_before_outer_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = mock.Mock()
            runner.directory = Path(directory)
            runner.args.microphone = "test microphone"
            runner.args.real_v1_off = True
            runner.capture_finished = False
            runner.plan = {}
            runner.status.side_effect = [dict(boot=1), dict(mode="normal", boot=2, persist=0, enabled=False)]
            clock = Clock()
            microphone = mock.Mock()
            microphone.health_problem.return_value = ""
            microphone.finish.return_value = {"result": "INCOMPLETE"}
            replay = mock.Mock()
            replay.release_after_idle.side_effect = subject.Incomplete("missing START")
            with mock.patch.object(subject, "configure", return_value={}), \
                 mock.patch.object(subject, "MicrophoneCapture", return_value=microphone), \
                 mock.patch.object(subject, "ReplaySession", return_value=replay), \
                 mock.patch.object(subject.time, "monotonic", side_effect=clock), \
                 mock.patch.object(subject.time, "sleep", side_effect=clock.sleep):
                with self.assertRaisesRegex(subject.Incomplete, "missing START"):
                    subject.run_audio_cases(runner)
            microphone.finish.assert_called_once_with(False)
            self.assertIs(runner.replay, replay)  # Caller owns emulator cleanup.
            self.assertEqual(runner.plan["audio"][0]["completed"], False)

    def test_lost_write_response_gets_one_authoritative_read_no_second_write(self):
        http = mock.Mock()
        http.request.side_effect = [TimeoutError("lost response"), {"voiceVolume": 97}]
        actual = subject.one_write_readback(http, "/audio", {"voiceVolume": 97}, {"voiceVolume": 97})
        self.assertEqual(actual["voiceVolume"], 97)
        self.assertEqual(http.request.call_args_list, [mock.call("/audio", {"voiceVolume": 97}), mock.call("/audio")])

    def test_unresolved_write_cannot_proceed(self):
        http = mock.Mock()
        http.request.side_effect = [TimeoutError(), {"voiceVolume": 31}]
        with self.assertRaises(subject.Incomplete):
            subject.one_write_readback(http, "/audio", {"voiceVolume": 97}, {"voiceVolume": 97})
        self.assertEqual(http.request.call_count, 2)

    def test_activation_readback_uses_slots_endpoint(self):
        http = mock.Mock()
        http.request.side_effect = [None, {"enabled": False}]
        subject.one_write_readback(http, "/activate", {"enable": False}, {"enabled": False}, read_endpoint="/slots")
        self.assertEqual(http.request.call_args_list[-1], mock.call("/slots"))

    def replay(self, directory):
        executable = directory / "v1replay"
        executable.write_text("fixture executable"); executable.chmod(0o700)
        clock = Clock()
        replay = behavior_capture.ReplaySession(executable, directory / "replay", "audio",
            authored=subject.authored_input(subject.CASES[0]), detector_volume=4,
            lease_factory=Lease, process_factory=Process, clock=clock, sleep=clock.sleep)
        replay.start()
        self.addCleanup(lambda: replay.finish(False))
        return replay, clock

    def admit(self, replay, *, start=True, complete_capture=True):
        replay.process.output.write(b"paused\n")
        if start:
            replay.process.output.write("reqStartAlertData — alert table enabled\n".encode())
        raw = packet([0xAA, 0xD8, 0xEA, 0x31, 9, 0x77, 0x77, 0, 0, 0, 0xC, 4, 0x40])
        replay.process.feed({"state": "session_transport", "active": True}, notification(raw, 1_000_000_000, 1))
        if complete_capture:
            replay.process.feed(*capture_replies(4, 1_000_000_000))

    def test_paused_replay_keeps_owner_watchdog_and_releases_only_after_full_idle(self):
        with tempfile.TemporaryDirectory() as directory:
            replay, _ = self.replay(Path(directory)); self.admit(replay)
            with mock.patch.object(behavior_capture.os, "write", return_value=1) as write:
                record = replay.release_after_idle(3, timeout_s=1)
            write.assert_called_once_with(replay.master_fd, b" ")
            self.assertGreaterEqual(record["release_before_ns"] - record["idle_started_ns"], 3_000_000_000)
            self.assertIn("--owner-pid", replay.process.command)
            self.assertIn("--paused", replay.process.command)
            self.assertEqual(replay.process.command[replay.process.command.index("--idle-lead")+1], "3")
            self.assertTrue(replay._released)
            replay.finish(False)

    def test_missing_start_times_out_without_release(self):
        with tempfile.TemporaryDirectory() as directory:
            replay, _ = self.replay(Path(directory)); self.admit(replay, start=False)
            with self.assertRaisesRegex(behavior_capture.CaptureError, "readiness"):
                replay.release_after_idle(timeout_s=.1)
            self.assertFalse(replay._released)
            replay.finish(False)

    def test_start_and_idle_without_complete_capture_never_release(self):
        with tempfile.TemporaryDirectory() as directory:
            replay, _ = self.replay(Path(directory)); self.admit(replay, complete_capture=False)
            with self.assertRaisesRegex(behavior_capture.CaptureError, "readiness"):
                replay.release_after_idle(timeout_s=.1)
            self.assertFalse(replay._released)
            replay.finish(False)

    def test_early_alert_is_rejected_before_release(self):
        with tempfile.TemporaryDirectory() as directory:
            replay, _ = self.replay(Path(directory)); self.admit(replay)
            replay.process.feed({"state": "stimulus_requested"})
            with self.assertRaisesRegex(behavior_capture.CaptureError, "before automatic release"):
                replay.release_after_idle(timeout_s=.1)
            self.assertFalse(replay._released)
            replay.events.pop()  # finish requires complete shape for genuine authored events
            replay.finish(False)

    def test_transport_loss_during_connected_idle_never_releases(self):
        with tempfile.TemporaryDirectory() as directory:
            replay, clock = self.replay(Path(directory)); self.admit(replay)
            def pump():
                if clock.now > 2:
                    replay.process.feed({"state": "session_transport", "active": False})
            with self.assertRaisesRegex(behavior_capture.CaptureError, "lost transport"):
                replay.release_after_idle(pump=pump)
            self.assertFalse(replay._released)
            replay.finish(False)


if __name__ == "__main__":
    unittest.main()
