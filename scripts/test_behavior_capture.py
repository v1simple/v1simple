#!/usr/bin/env python3
"""Host-only failure-boundary tests; no camera, serial, BLE or DUT is opened."""
from __future__ import annotations

import json
import hashlib
from copy import deepcopy
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import behavior_capture as subject
import camera_capture
import camera_timing


def clear_event(when=1_000_000_000):
    return {"state": "notification_accepted", "characteristic": "B2CE",
            "payloadHex": "AAD8EA430800000000000000B7AB", "hostMonotonicNs": when,
            "globalTxSequence": 3}


def idle_event(when=1_010_000_000):
    return {**clear_event(when), "payloadHex": "AAD8EA310977770000000C0440E4AB", "globalTxSequence": 4}


class Clock:
    def __init__(self):
        self.now = 1.0

    def __call__(self):
        return self.now

    def sleep(self, delay):
        self.now += delay


class Lease:
    def __init__(self):
        self.fd = None
        self.closed = False

    def __enter__(self):
        self.fd = os.open(os.devnull, os.O_RDONLY)
        return self

    def __exit__(self, *args):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        self.closed = True


class Process:
    def __init__(self, command, **kwargs):
        self.command, self.output = command, kwargs["stdout"]
        self.code = None
        self.stop_count = 0

    def feed(self, *events):
        for event in events:
            self.output.write(b"V1REPLAY_EVENT " + json.dumps(event).encode() + b"\n")
        self.output.flush()

    def poll(self):
        return self.code

    def send_signal(self, sig):
        self.stop_count += 1
        self.feed({"state": "stopped"})
        self.code = 0

    def wait(self, timeout):
        return self.code


def delivery_fixture(path, runs=1):
    """Resolved two-row seed then one-row release, with independent accept times."""
    row = {"band": "K", "bandMask": 4, "frequencyMHz": 24150, "strength": 6,
           "direction": "FRONT", "directionMask": 1, "priority": True, "junk": False}
    secondary = {**row, "band": "X", "frequencyMHz": 10525, "priority": False}
    document = {"schemaVersion": 2, "origin": "synthetic_bench", "samples": [
        {"sourceIndex": 0, "offsetSeconds": 0.0, "phase": "seed", "muted": False,
         "alerts": [row, secondary]},
        {"sourceIndex": 1, "offsetSeconds": 1.0, "phase": "release", "muted": False, "alerts": [row]}]}
    raw = json.dumps(document).encode()
    path.write_bytes(raw)
    digest = hashlib.sha256(raw).hexdigest()
    events = [
        {"state": "scenario_resolved", "sha256": digest, "byteCount": len(raw),
         "sampleCount": 2, "origin": "synthetic_bench"},
        {"state": "configured", "scenarioSha256": digest, "totalSamples": 2,
         "scenarioOrigin": "synthetic_bench", "blinkProfile": "steady"}]
    global_sequence = 0
    for run in range(runs):
        for sample in document["samples"]:
            sequence = run * 2 + sample["sourceIndex"] + 1
            when = sequence * 1_000_000_000
            rows = sample["alerts"]
            notifications = []
            for ordinal in range(len(rows) + 1):
                is_row = ordinal < len(rows)
                payload = bytes([0xAA, 0xD8, 0xEA, 0x43 if is_row else 0x31, 8,
                                 (ordinal + 1) * 16 + len(rows), 0, 0, 0, 0, 0, 0])
                payload += bytes([sum(payload) % 256, 0xAB])
                notifications.append({"ordinal": ordinal, "channel": "display_short",
                    "kind": "alert_row" if is_row else "display_frame", "bytesHex": payload.hex(),
                    "alertRowIndex": ordinal + 1 if is_row else None,
                    "alertRowCount": len(rows) if is_row else None})
            expected = {"phase": sample["phase"], "muted": False, "displayOn": True,
                        "arrowBlink": False, "bandBlink": False, "alerts": [
                            {**{key: value for key, value in row.items() if key not in ("strength", "directionMask")},
                             "bars": row["strength"]} for row in rows]}
            events.append({"state": "stimulus_requested", "stimulusSequence": sequence,
                           "sourceIndex": sample["sourceIndex"], "requestedHostMonotonicNs": when,
                           "replayOffsetSeconds": sample["offsetSeconds"], "expected": expected,
                           "notifications": notifications})
            for notification in notifications:
                global_sequence += 1
                payload = bytes.fromhex(notification["bytesHex"])
                events.append({"state": "notification_accepted", "stimulusSequence": sequence,
                    "emissionOrdinal": notification["ordinal"], "globalTxSequence": global_sequence,
                    "characteristic": "B2CE", "payloadHex": payload.hex(),
                    "payloadSha256": hashlib.sha256(payload).hexdigest(),
                    "hostMonotonicNs": when + (notification["ordinal"] + 1) * 10_000_000})
        events.append({"state": "complete"})
    return {"lifecycle_completed": True, "events": events, "complete_count": runs}


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "scenario.json"
        self.result = delivery_fixture(self.path)

    def verify(self, result=None):
        return subject.verified_replay_delivery(self.result if result is None else result, self.path)

    def test_complete_delivery_uses_final_accepted_row_not_request_or_display(self):
        verified = self.verify()
        self.assertEqual(verified["requested_stimuli"], 2)
        self.assertEqual(verified["accepted_notifications"], 5)
        self.assertEqual(verified["phase_intervals"][0]["start_ns"], 1_020_000_000)
        self.assertEqual(verified["phase_intervals"][0]["request_start_ns"], 1_000_000_000)
        self.assertEqual(verified["phase_intervals"][1]["start_ns"], 2_010_000_000)
        self.assertFalse(verified["notification_acceptance_is_dut_ack"])

    def test_restart_requires_two_complete_authored_runs(self):
        self.result = delivery_fixture(self.path, 2)
        verified = self.verify()
        self.assertEqual(verified["completed_runs"], 2)
        self.assertEqual([row["run_index"] for row in verified["phase_intervals"]], [0, 0, 1, 1])
        self.result["events"] = [event for event in self.result["events"]
                                 if not (event.get("state") == "stimulus_requested"
                                         and event["stimulusSequence"] == 4)]
        with self.assertRaisesRegex(subject.CaptureError, "truncated"):
            self.verify()

    def test_wrong_scenario_hash_is_rejected_before_acceptance(self):
        self.path.write_bytes(self.path.read_bytes() + b" ")
        with self.assertRaisesRegex(subject.CaptureError, "hash/count/origin"):
            self.verify()

    def test_missing_duplicate_mismatched_and_skipped_acceptance_cannot_pass(self):
        position = next(index for index, event in enumerate(self.result["events"])
                        if event.get("state") == "notification_accepted")
        for failure in ("missing", "duplicate", "bytes", "channel", "skip", "hash"):
            result = deepcopy(self.result)
            event = result["events"][position]
            if failure == "missing":
                del result["events"][position]
            elif failure == "duplicate":
                result["events"].insert(position, deepcopy(event))
            elif failure == "bytes":
                event["payloadHex"] = clear_event()["payloadHex"]
            elif failure == "channel":
                event["characteristic"] = "B4E0"
            elif failure == "skip":
                event["state"] = "notification_skipped"
            else:
                event["payloadSha256"] = "wrong"
            with self.subTest(failure=failure), self.assertRaises(subject.CaptureError):
                self.verify(result)

    def test_filtered_authored_alerts_and_incomplete_table_cannot_pass(self):
        request = next(event for event in self.result["events"] if event.get("state") == "stimulus_requested")
        request["expected"]["alerts"].pop()
        with self.assertRaisesRegex(subject.CaptureError, "filtered"):
            self.verify()
        self.result = delivery_fixture(self.path)
        request = next(event for event in self.result["events"] if event.get("state") == "stimulus_requested")
        request["notifications"].pop(0)
        with self.assertRaisesRegex(subject.CaptureError, "complete alert table"):
            self.verify()

    def test_untagged_terminal_clear_is_not_mistaken_for_authored_delivery(self):
        terminal = clear_event(3_000_000_000)
        terminal["globalTxSequence"] = 6
        self.result["events"].append(terminal)
        self.assertEqual(self.verify()["accepted_notifications"], 5)
        terminal["stimulusSequence"] = 99
        terminal["emissionOrdinal"] = 0
        with self.assertRaisesRegex(subject.CaptureError, "unknown planned"):
            self.verify()


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.executable = self.root / "v1replay"
        self.executable.write_text("fixture")
        self.executable.chmod(0o700)
        self.clock = Clock()
        self.lease = Lease()
        self.sessions = []

    def tearDown(self):
        for session in self.sessions:
            session.finish(False)
        self.tmp.cleanup()

    def replay(self, scenario="clearance", factory=Process):
        session = subject.ReplaySession(self.executable, self.root / f"run-{len(self.sessions)}",
            scenario, lease_factory=lambda: self.lease, process_factory=factory,
            clock=self.clock, sleep=self.clock.sleep)
        self.sessions.append(session)
        session.start()
        return session

    def ready(self, session):
        session.process.feed({"state": "session_transport", "active": True},
                             {"state": "stimulus_requested", "requestedHostMonotonicNs": 900_000_000,
                              "sourceIndex": 15, "expected": {"phase": "external"}}, clear_event(), idle_event(),
                             {"state": "complete"})
        session.wait_complete(1)

    def test_canonical_clear_requires_acceptance_exact_payload_channel_checksum_and_time(self):
        self.assertTrue(subject.is_clear_notification(clear_event()))
        for patch in ({"state": "notification_requested"}, {"characteristic": "B8D2"},
                      {"payloadHex": "AAD8EA430800000000000000B8AB"},
                      {"payloadHex": "AAD8EA430811000000000000C8AB"},
                      {"payloadHex": "broken"}, {"hostMonotonicNs": True}):
            with self.subTest(patch=patch):
                self.assertFalse(subject.is_clear_notification({**clear_event(), **patch}))

    def test_private_scenario_is_live_and_leaves_clear_to_existing_player(self):
        scenario = subject.clearance_scenario()
        self.assertEqual(scenario["samples"][-1]["offsetSeconds"], 5)
        self.assertTrue(all(sample["strength"] == 6 for sample in scenario["samples"]))
        replay = self.replay()
        command = replay.process.command
        self.assertIn("--quiet-after-complete", command)
        self.assertNotIn("--exit-on-complete", command)
        self.assertNotIn("--always-alerts", command)
        self.assertNotIn("--no-wait", command)

    def test_persistence_uses_existing_multi_alert_scenario(self):
        replay = self.replay("persistence")
        self.assertIn("--persistence-coverage", replay.process.command)
        self.assertNotIn("--scenario", replay.process.command)

    def test_replay_refuses_evidence_under_git_checkout(self):
        (self.root / ".git").write_text("gitdir: nowhere")
        with self.assertRaisesRegex(ValueError, "outside Git"):
            subject.ReplaySession(self.executable, self.root / "nested")

    def test_start_failure_releases_lease_and_pty(self):
        with self.assertRaisesRegex(OSError, "launch refused"):
            self.replay(factory=mock.Mock(side_effect=OSError("launch refused")))
        self.assertTrue(self.lease.closed)
        self.assertIsNone(self.sessions[-1].master_fd)
        self.assertFalse(self.sessions[-1].result["lifecycle_completed"])

    def test_partial_machine_line_waits_for_completion(self):
        replay = self.replay()
        replay.log_handle.write(b'noise\nV1REPLAY_EVENT {"state":"complete"')
        replay.log_handle.flush()
        self.assertEqual(replay.poll(), [])
        replay.log_handle.write(b'}\n')
        replay.log_handle.flush()
        self.assertEqual([row["state"] for row in replay.poll()], ["complete"])
        self.assertEqual(len(replay.poll()), 1)

    def test_malformed_machine_evidence_prevents_lifecycle_pass_and_still_cleans_up(self):
        replay = self.replay()
        replay.log_handle.write(b'V1REPLAY_EVENT {broken}\n')
        replay.log_handle.flush()
        result = replay.finish(True)
        self.assertFalse(result["lifecycle_completed"])
        self.assertIn("malformed", result["errors"][0])
        self.assertTrue(self.lease.closed)
        self.assertEqual(replay.process.stop_count, 1)
        self.assertEqual(replay.finish(True), result)

    def test_complete_deadline_is_bounded_and_pumps_single_usb_owner(self):
        replay = self.replay()
        pump = mock.Mock()
        with self.assertRaisesRegex(subject.CaptureError, "deadline"):
            replay.wait_complete(.2, pump=pump)
        self.assertGreater(pump.call_count, 0)
        self.assertLessEqual(self.clock(), 1.25)

    def test_early_exit_is_not_treated_as_complete(self):
        replay = self.replay()
        replay.process.code = 0
        with self.assertRaisesRegex(subject.CaptureError, "exited early"):
            replay.wait_complete(.2)

    def test_quiet_tail_requires_complete_clear_and_active_transport(self):
        replay = self.replay()
        with self.assertRaisesRegex(subject.CaptureError, "completed playback"):
            replay.hold_quiet(.1)
        replay.process.feed({"state": "complete"})
        with self.assertRaisesRegex(subject.CaptureError, "count-zero"):
            replay.hold_quiet(.1)
        replay.process.feed({"state": "stimulus_requested", "requestedHostMonotonicNs": 900_000_000,
                             "expected": {"phase": "external"}}, clear_event(), idle_event())
        with self.assertRaisesRegex(subject.CaptureError, "active transport"):
            replay.hold_quiet(.1)

    def test_quiet_tail_returns_host_anchor_without_claiming_dut_ack(self):
        replay = self.replay()
        self.ready(replay)
        result = replay.hold_quiet(.2)
        self.assertEqual(result["clear_accepted_ns"], 1_000_000_000)
        self.assertEqual(result["idle_accepted_ns"], 1_010_000_000)
        self.assertEqual(result["quiet_started_ns"], result["idle_accepted_ns"])
        self.assertEqual(result["clear_bundle_skew_ns"], 10_000_000)
        self.assertEqual(result["anchor_kind"], "host_notification_acceptance")
        self.assertGreaterEqual(result["observed_until_ns"], 1_210_000_000)
        final = replay.finish(True)
        self.assertTrue(final["lifecycle_completed"])
        self.assertFalse(final["notification_acceptance_is_dut_ack"])

    def test_scenario_clear_bundle_is_not_mistaken_for_the_terminal_quiet_anchor(self):
        replay = self.replay()
        replay.process.feed({"state": "session_transport", "active": True},
            {"state": "stimulus_requested", "requestedHostMonotonicNs": 900_000_000,
             "sourceIndex": 15, "expected": {"phase": "final_clear"}},
            {**clear_event(), "stimulusSequence": 16}, {"state": "complete"})
        replay.wait_complete(.1)
        calls = 0
        def deliver_pending():
            nonlocal calls
            calls += 1
            if calls == 1:
                replay.process.feed({**idle_event(1_050_000_000), "stimulusSequence": 16})
            if calls == 2:
                replay.process.feed(clear_event(1_100_000_000))
            if calls == 3:
                replay.process.feed(idle_event(1_110_000_000))
        result = replay.hold_quiet(.2, pump=deliver_pending)
        self.assertEqual(result["clear_accepted_ns"], 1_100_000_000)

    def test_terminal_count_zero_without_idle_display_cannot_qualify_silence(self):
        replay = self.replay()
        replay.process.feed({"state": "session_transport", "active": True},
                            {"state": "stimulus_requested", "requestedHostMonotonicNs": 900_000_000},
                            clear_event(), {"state": "complete"})
        with self.assertRaisesRegex(subject.CaptureError, "count-zero/idle-display bundle"):
            replay.hold_quiet(.1, delivery_timeout_s=.1)

    def test_terminal_idle_requires_checksum_zero_leds_resting_glyph_and_untagged_acceptance(self):
        self.assertTrue(subject.is_idle_notification(idle_event()))
        for patch in ({"state": "notification_requested"}, {"characteristic": "B4E0"},
                      {"payloadHex": "AAD8EA310906063F24240C044089AB"},
                      {"payloadHex": "AAD8EA310977770000000C0440E5AB"},
                      {"stimulusSequence": 7}, {"emissionOrdinal": 1}):
            with self.subTest(patch=patch):
                self.assertFalse(subject.is_idle_notification({**idle_event(), **patch}))

    def test_late_idle_bundle_is_incomplete_and_repeated_idle_breaks_silence(self):
        replay = self.replay()
        replay.process.feed({"state": "session_transport", "active": True},
                            {"state": "stimulus_requested", "requestedHostMonotonicNs": 900_000_000},
                            clear_event(), idle_event(1_251_000_000), {"state": "complete"})
        with self.assertRaisesRegex(subject.CaptureError, "0.25 second"):
            replay.hold_quiet(.1)
        replay = self.replay()
        self.ready(replay)
        replay.process.feed(idle_event(1_020_000_000))
        with self.assertRaisesRegex(subject.CaptureError, "later traffic"):
            replay.hold_quiet(.1)

    def test_old_unattributed_clear_does_not_qualify_a_new_run(self):
        events = [clear_event(), {"state": "stimulus_requested",
                                 "requestedHostMonotonicNs": 2_000_000_000}]
        self.assertIsNone(subject.terminal_clear_event(events))

    def test_late_notification_invalidates_quiet_claim(self):
        replay = self.replay()
        self.ready(replay)
        replay.process.feed({**clear_event(1_000_000_001), "payloadHex": "AAD8EA3109"})
        with self.assertRaisesRegex(subject.CaptureError, "later traffic"):
            replay.hold_quiet(.1)

    def test_notification_during_final_tail_sleep_is_not_missed(self):
        replay = self.replay()
        self.ready(replay)
        def sleep_and_deliver(seconds):
            self.clock.sleep(seconds)
            if self.clock() >= 1.11:
                replay.process.feed(idle_event(round(self.clock() * 1e9)))
        replay.sleep = sleep_and_deliver
        with self.assertRaisesRegex(subject.CaptureError, "later traffic"):
            replay.hold_quiet(.1)

    def test_disconnect_and_reconnect_during_tail_cannot_pass_as_connected_quiet(self):
        replay = self.replay()
        self.ready(replay)
        replay.process.feed({"state": "session_transport", "active": False},
                             {"state": "session_transport", "active": True})
        with self.assertRaisesRegex(subject.CaptureError, "lost transport"):
            replay.hold_quiet(.1)

    def test_restart_requires_a_new_complete_event(self):
        replay = self.replay()
        self.ready(replay)
        with mock.patch.object(subject.os, "write") as write:
            replay.restart()
        write.assert_called_once_with(replay.master_fd, b"r")
        with self.assertRaisesRegex(subject.CaptureError, "deadline"):
            replay.wait_complete(.1)
        replay.process.feed({"state": "complete"})
        replay.wait_complete(.1)
        self.assertTrue(replay.finish(True)["lifecycle_completed"])

    def test_phase_intervals_preserve_restart_and_reject_backwards_time(self):
        def event(when, phase, source_index):
            return {"state": "stimulus_requested", "requestedHostMonotonicNs": when,
                    "sourceIndex": source_index, "expected": {"phase": phase}}
        phases = subject.phase_intervals([event(1, "a", 0), event(2, "a", 1),
                                         event(3, "b", 2), event(4, "b", 0)])
        self.assertEqual([(row["start_ns"], row["end_ns"]) for row in phases], [(1, 3), (3, 4), (4, None)])
        with self.assertRaisesRegex(subject.CaptureError, "backwards"):
            subject.phase_intervals([event(4, "a", 0), event(3, "a", 1)])

    def test_camera_preflight_failure_stops_and_never_restarts_camera(self):
        camera = mock.Mock()
        camera.stop.return_value = {"result": "CAPTURE_FAILED"}
        session = subject.CaptureSession(self.root / "camera", camera_factory=lambda *args: camera,
            preflight=lambda _: {"result": "INCONCLUSIVE"})
        with self.assertRaisesRegex(subject.CaptureError, "SCAN"):
            session.start()
        camera.stop.assert_called_once_with(False)
        with self.assertRaisesRegex(subject.CaptureError, "twice"):
            session.start()

    def test_camera_markers_use_monotonic_clock_and_cannot_override_identity(self):
        camera = mock.Mock()
        camera.stop.return_value = {"result": "CAPTURE_FAILED"}
        session = subject.CaptureSession(self.root / "camera", camera_factory=lambda *args: camera,
            preflight=lambda _: {"result": "PASS"}, clock_ns=lambda: 123)
        session.start()
        marker = session.mark("saved", case="palette-a")
        self.assertEqual(marker["host_monotonic_ns"], 123)
        with self.assertRaises(ValueError):
            session.mark("bad", host_monotonic_ns=1)
        session.finish(False)
        with self.assertRaises(subject.CaptureError):
            session.mark("late")
        self.assertEqual(len((session.out_dir / "behavior_markers.ndjson").read_text().splitlines()), 2)

    def test_recorder_stop_calls_recovery_before_slow_video_verification(self):
        camera = camera_capture.CameraCapture(self.root / "camera", 1)
        camera.process = object()
        events = []

        def stopped():
            events.append("recorder_stopped")
            camera.process = None

        def verify():
            events.append("video_verification")
            raise RuntimeError("fixture verification failed after recovery")

        with mock.patch.object(camera, "_stop_process", side_effect=stopped), \
                mock.patch.object(camera, "_verify_video_timing", side_effect=verify):
            with self.assertRaisesRegex(RuntimeError, "after recovery"):
                camera.stop(False, after_recording=lambda: events.append("recovered"))
        self.assertEqual(events, ["recorder_stopped", "recovered", "video_verification"])

    def test_capture_adapter_forwards_recovery_and_handles_already_finished_capture(self):
        camera = mock.Mock()
        events = []

        def stop(completed, *, after_recording=None):
            events.append("stopped")
            after_recording()
            events.append("verified")
            return {"result": "CAPTURE_FAILED"}

        camera.stop.side_effect = stop
        session = subject.CaptureSession(self.root / "camera", camera_factory=lambda *args: camera,
                                        preflight=lambda _: {"result": "PASS"})
        session.start()
        recovered = mock.Mock(side_effect=lambda: events.append("recovered"))
        result = session.finish(False, after_recording=recovered)
        self.assertEqual(events, ["stopped", "recovered", "verified"])
        self.assertEqual(session.finish(False, after_recording=recovered), result)
        self.assertEqual(camera.stop.call_count, 1)
        self.assertEqual(recovered.call_count, 2)  # Coordinator's restore_once owns idempotence.

    def test_media_subprocesses_have_explicit_time_bounds(self):
        camera = camera_capture.CameraCapture(self.root / "camera", 1)
        camera.ffprobe, camera.ffmpeg = "fixture-ffprobe", "fixture-ffmpeg"
        for operation in (camera._probe_video,
                          lambda: camera._decode_profile_frame(self.root / "frame.png"),
                          lambda: camera._extract_video_still(self.root / "movie.mov", self.root / "still.png", 1)):
            with self.subTest(operation=operation), mock.patch.object(camera_capture.subprocess, "run",
                    side_effect=subject.subprocess.TimeoutExpired("fixture", 30)) as run:
                with self.assertRaises(subject.subprocess.TimeoutExpired):
                    operation()
                self.assertEqual(run.call_args.kwargs["timeout"], 30)
        with mock.patch.object(camera_timing.subprocess, "run",
                side_effect=subject.subprocess.TimeoutExpired("fixture", 1200)) as run:
            with self.assertRaises(subject.subprocess.TimeoutExpired):
                camera_timing.probe_all_video_frames("fixture-ffprobe", self.root / "movie.mov")
            self.assertEqual(run.call_args.kwargs["timeout"], 1200)

    def test_full_video_probe_timeout_is_retained_as_failed_evidence(self):
        output = self.root / "timing-result.json"
        with mock.patch.object(camera_timing, "load_frame_sidecar", return_value=[
                {"status": "written", "frame_seq": 1}]), \
                mock.patch.object(camera_timing, "probe_all_video_frames",
                    side_effect=subject.subprocess.TimeoutExpired("fixture-ffprobe", 1200)):
            result = camera_timing.verify_video_file("fixture-ffprobe", self.root / "movie.mov",
                                                    self.root / "sidecar.ndjson", output)
        self.assertEqual(result["status"], "probe_error")
        self.assertEqual(result["missing_encoded_frame_count"], 1)
        self.assertEqual(json.loads(output.read_text())["status"], "probe_error")

    def frames(self, *, missing=(), decoder_shift=0):
        # Tests selection against a known verified timeline; constructor's
        # integrity admission is tested separately below.
        frames = subject.RecordedFrames.__new__(subject.RecordedFrames)
        frames.video = self.root / "video.mov"
        frames.host = [1_000_000_000 + index * 5_000_000 for index in range(201) if index not in missing]
        frames.pts = [(value - 1_000_000_000) / 1e9 for value in frames.host]
        frames.decoder = lambda video, when: (when + decoder_shift, "original-rgb")
        return frames

    def test_frame_reader_requires_manifest_before_decoding(self):
        decoder = mock.Mock()
        with self.assertRaises((OSError, RuntimeError)):
            subject.RecordedFrames(self.root, decoder=decoder)
        decoder.assert_not_called()

    def test_frame_selection_returns_actual_row_times_and_bounds_step(self):
        samples = self.frames().decoded_samples(1_100_000_000, 1_700_000_001, .2)
        self.assertEqual(samples[0]["host_monotonic_ns"], 1_100_000_000)
        self.assertEqual(samples[-1]["host_monotonic_ns"], 1_700_000_000)
        self.assertTrue(all(sample["rgb"] == "original-rgb" for sample in samples))
        self.assertTrue(all(b["host_monotonic_ns"] - a["host_monotonic_ns"] <= 200_000_000
                            for a, b in zip(samples, samples[1:])))

    def test_capture_gap_cannot_be_interpolated_into_success(self):
        with self.assertRaisesRegex(subject.CaptureError, "gap"):
            self.frames(missing=range(35, 46)).decoded_samples(1_200_000_000, 1_800_000_000)

    def test_decoder_drift_and_outside_capture_are_rejected(self):
        with self.assertRaisesRegex(subject.CaptureError, "20 ms"):
            self.frames(decoder_shift=.025).decoded_samples(1_200_000_000, 1_800_000_000)
        with self.assertRaisesRegex(subject.CaptureError, "outside"):
            self.frames().decoded_samples(900_000_000, 1_800_000_000)
        with self.assertRaisesRegex(subject.CaptureError, "verified camera timeline"):
            self.frames(decoder_shift=.001).decoded_samples(1_200_000_000, 1_800_000_000)


if __name__ == "__main__":
    unittest.main()
