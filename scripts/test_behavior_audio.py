#!/usr/bin/env python3
"""Independent acoustic-oracle controls; no microphone, playback or DUT access."""
from pathlib import Path
import json
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch
import wave

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts/bench"))
import behavior_audio as audio


def recording(clips, *, gain=.3, noise=.00005, echo=False, duplicate=False):
    source = np.concatenate(clips).astype(np.float64) / 32768
    if echo:
        impulse = np.zeros(180)
        impulse[0], impulse[43], impulse[129] = 1., .3, -.12
        source = np.convolve(source, impulse)
    phrase = gain * source
    parts = [np.zeros(audio.SAMPLE_RATE * 3), phrase, np.zeros(audio.SAMPLE_RATE * 3)]
    if duplicate:
        parts += [phrase, np.zeros(audio.SAMPLE_RATE * 3)]
    data = np.concatenate(parts)
    return data + np.random.default_rng(77).normal(0, noise, len(data))


class AudioOracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.assets = ROOT / "tools/freq_audio/mulaw"
        cls.ka, cls.hashes = audio.reference_clips(cls.assets, "ka")
        cls.k, _ = audio.reference_clips(cls.assets, "k")
        cls.x, _ = audio.reference_clips(cls.assets, "x")

    def test_independent_mulaw_formula_known_endpoints(self):
        self.assertEqual(audio.decode_mulaw(bytes([0, 128, 127, 255])).tolist(), [-32124, 32124, 0, 0])

    def test_identifies_every_clip_through_gain_noise_and_room_echo(self):
        for phrase in (self.ka, self.k, self.x):
            with self.subTest(phrase=phrase[0].size):
                result = audio.phrase_observation(recording(phrase, gain=.16, echo=True), phrase)
                self.assertEqual(result["result"], "PASS", result)
                self.assertEqual(len(result["clip_scores"]), 5)
                self.assertAlmostEqual(result["start_s"], 3., delta=.05)

    def test_wrong_band_and_frequency_cannot_pass(self):
        result = audio.phrase_observation(recording(self.x), self.ka)
        self.assertNotEqual(result["result"], "PASS", result)

    def test_single_wrong_digit_cannot_hide_in_correct_phrase(self):
        wrong = list(self.ka)
        wrong[2] = audio.decode_mulaw((self.assets / "digit_1.mul").read_bytes())
        result = audio.phrase_observation(recording(wrong), self.ka)
        self.assertNotEqual(result["result"], "PASS", result)

    def test_isolated_wrong_band_or_direction_cannot_hide_in_correct_phrase(self):
        replacements = {
            0: ("band_k", "band_ka", "band_x", "band_ku", "band_photo", "band_laser"),
            4: ("dir_ahead", "dir_side", "dir_behind"),
        }
        for phrase, clips in (("ka", self.ka), ("k", self.k), ("x", self.x)):
            for position, names in replacements.items():
                for name in names:
                    if name == audio.CLIPS[phrase][position]:
                        continue
                    with self.subTest(phrase=phrase, wrong_clip=name):
                        wrong = list(clips)
                        wrong[position] = audio.decode_mulaw((self.assets / (name + ".mul")).read_bytes())
                        result = audio.phrase_observation(recording(wrong), clips)
                        self.assertNotEqual(result["result"], "PASS", result)

    def test_similar_wrong_frequency_digit_cannot_pass(self):
        # Both previously exceeded the phrase and expected-clip similarity
        # thresholds even though the actual frequency was different.
        for name in ("digit_5", "digit_9"):
            with self.subTest(wrong_clip=name):
                wrong = list(self.ka)
                wrong[2] = audio.decode_mulaw((self.assets / (name + ".mul")).read_bytes())
                result = audio.phrase_observation(recording(wrong), self.ka)
                self.assertNotEqual(result["result"], "PASS", result)

    def test_missing_direction_cannot_pass(self):
        result = audio.phrase_observation(recording(self.ka[:-1]), self.ka)
        self.assertNotEqual(result["result"], "PASS", result)

    def test_duplicate_complete_announcement_fails(self):
        result = audio.phrase_observation(recording(self.ka, duplicate=True), self.ka)
        self.assertEqual(result["result"], "FAIL", result)
        self.assertIn("another complete announcement was detected", result["failures"])

    def test_correct_phrase_then_different_unrequested_announcement_cannot_pass(self):
        extra = np.concatenate([.3 * np.concatenate(self.x).astype(np.float64) / 32768,
                                np.zeros(audio.SAMPLE_RATE * 3)])
        extra += np.random.default_rng(78).normal(0, .00005, len(extra))
        result = audio.phrase_observation(np.concatenate([recording(self.ka), extra]), self.ka)
        self.assertNotEqual(result["result"], "PASS", result)

    def test_dead_microphone_cannot_pass_audible_or_silent_cases(self):
        dead = np.zeros(audio.SAMPLE_RATE * 12)
        positive = audio.phrase_observation(dead, self.ka)
        self.assertEqual(positive["result"], "INCOMPLETE")
        self.assertEqual(audio.silence_observation(dead, [positive, positive])["result"], "INCOMPLETE")

    def test_zero_volume_requires_two_good_positive_controls(self):
        positive = audio.phrase_observation(recording(self.ka), self.ka)
        quiet = recording([np.zeros(audio.SAMPLE_RATE * 2, dtype=np.int16)])
        self.assertEqual(audio.silence_observation(quiet, [positive])["result"], "INCOMPLETE")
        self.assertEqual(audio.silence_observation(quiet, [positive, positive])["result"], "PASS")

    def test_flat_microphone_only_during_silence_cannot_use_adjacent_positive_controls(self):
        positive = audio.phrase_observation(recording(self.ka), self.ka)
        self.assertEqual(positive["result"], "PASS", positive)
        # A recorder can remain alive and produce correctly sized PCM while
        # its input disappears. Other captures cannot validate these samples.
        for level in (0., 3 / 32768):
            with self.subTest(level=level):
                dead = np.full(audio.SAMPLE_RATE * 12, level)
                result = audio.silence_observation(dead, [positive, positive])
                self.assertEqual(result["result"], "INCOMPLETE", result)

    def test_audible_output_in_zero_volume_case_fails(self):
        positive = audio.phrase_observation(recording(self.ka), self.ka)
        result = audio.silence_observation(recording(self.k, gain=.06), [positive, positive])
        self.assertEqual(result["result"], "FAIL", result)

    def test_high_noise_rejects_test_setup(self):
        result = audio.phrase_observation(recording(self.ka, gain=.03, noise=.03), self.ka)
        self.assertEqual(result["result"], "INCOMPLETE", result)

    def test_wav_contract_rejects_wrong_rate_and_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.wav"
            with wave.open(str(path), "wb") as stream:
                stream.setnchannels(1); stream.setsampwidth(2); stream.setframerate(48000)
                stream.writeframes(bytes(96000))
            with self.assertRaises(audio.Incomplete):
                audio.read_recording(path)
            with wave.open(str(path), "wb") as stream:
                stream.setnchannels(1); stream.setsampwidth(2); stream.setframerate(audio.SAMPLE_RATE)
                stream.writeframes(bytes(audio.SAMPLE_RATE * 2))
            path.write_bytes(path.read_bytes()[:-100])
            with self.assertRaises(audio.Incomplete):
                audio.read_recording(path)


class MicrophoneLifecycleTests(unittest.TestCase):
    def recorder(self, directory):
        with patch.object(audio.shutil, "which", return_value="/bin/echo"):
            recorder = audio.MicrophoneCapture(Path(directory) / "capture", "Test microphone")
        recorder.started_ns = 100
        end = time.monotonic_ns()
        recorder.started_ns = end - 2_100_000_000
        recorder.first_host_ns = end - 2_000_000_000
        recorder.ready_ns = recorder.first_host_ns + 200_000_000
        recorder.ready_media_s = .2
        with wave.open(str(recorder.directory / "microphone.wav"), "wb") as stream:
            stream.setnchannels(1); stream.setsampwidth(2); stream.setframerate(audio.SAMPLE_RATE)
            stream.writeframes(bytes(audio.SAMPLE_RATE * 4))
        fmt = {"sample_rate": 48000, "channels": 1, "bits_per_channel": 16}
        rows = [{"schema_version": 1, "event": "audio_buffer", "status": "written", "buffer_seq": index + 1,
                 "source_format": fmt, "source_sample_frames": 48000,
                 "source_pts_value": (100 + index) * 48000, "source_pts_timescale": 48000,
                 "source_duration_value": 48000, "source_duration_timescale": 48000,
                 "audio_pts_value": index * 48000, "audio_pts_timescale": 48000,
                 "callback_host_ns": recorder.first_host_ns + (index+1) * 1_000_000_000,
                 "host_start_ns": recorder.first_host_ns + index * 1_000_000_000,
                 "host_end_ns": recorder.first_host_ns + (index+1) * 1_000_000_000} for index in range(2)]
        (recorder.directory / "audio_timing.ndjson").write_text("".join(json.dumps(row)+"\n" for row in rows))
        summary = {"schema_version": 1, "result": "CAPTURED", "reason": "none", "shutdown_reason": "q",
                   "source_clock": "avcapture_session_synchronization_clock", "timing_errors": 0, "writer_drops": 0,
                   "encoded_sample_frames": audio.SAMPLE_RATE * 2, "source_buffers": 2,
                   "source_format": fmt, "source_sample_frames": 96000, "source_duration_s": 2., "duration_s": 2.,
                   "first_host_ns": recorder.first_host_ns, "last_host_ns": end}
        (recorder.directory / "native_summary.json").write_text(json.dumps(summary))
        return recorder

    def test_early_clean_exit_cannot_qualify_partial_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = self.recorder(directory)
            recorder.process = Mock(returncode=0)
            recorder.process.poll.return_value = 0
            result = recorder.finish(True)
            self.assertEqual(result["result"], "INCOMPLETE")
            self.assertIn("stopped before", result["errors"][0])
            recorder.process.communicate.assert_not_called()

    def test_bounded_stop_records_time_binding_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = self.recorder(directory)
            recorder.process = Mock(returncode=0)
            recorder.process.poll.return_value = None
            result = recorder.finish(True)
            self.assertEqual(result["result"], "CAPTURED")
            self.assertEqual((result["ready_host_ns"], result["ready_media_s"]), (recorder.ready_ns, .2))
            self.assertIs(recorder.finish(True), result)
            recorder.process.communicate.assert_called_once_with(b"q\n", timeout=5)

    def test_failed_case_cannot_qualify_a_good_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = self.recorder(directory)
            recorder.process = Mock(returncode=0)
            recorder.process.poll.return_value = None
            self.assertEqual(recorder.finish(False)["result"], "INCOMPLETE")

    def test_lost_native_samples_cannot_hide_behind_successful_writer(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = self.recorder(directory)
            path = recorder.directory / "audio_timing.ndjson"
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[1]["source_pts_value"] += 1
            path.write_text("".join(json.dumps(row)+"\n" for row in rows))
            with self.assertRaisesRegex(audio.Incomplete, "missing, overlapping"):
                audio.verify_native_recording(recorder.directory, audio.SAMPLE_RATE * 2)

    def test_host_clock_refinement_retains_full_measured_uncertainty(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = self.recorder(directory)
            path = recorder.directory / "audio_timing.ndjson"
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[1]["host_start_ns"] -= 100_000
            rows[1]["host_end_ns"] -= 100_000
            path.write_text("".join(json.dumps(row)+"\n" for row in rows))
            summary_path = recorder.directory / "native_summary.json"
            summary = json.loads(summary_path.read_text())
            summary["last_host_ns"] -= 100_000
            summary_path.write_text(json.dumps(summary))
            result = audio.verify_native_recording(recorder.directory, audio.SAMPLE_RATE * 2)
            self.assertEqual(result["origin_min_host_ns"], recorder.first_host_ns - 100_001)
            self.assertEqual(result["origin_max_host_ns"], recorder.first_host_ns + 1)

    def test_retimed_source_gap_cannot_pass_with_correct_wav_length(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = self.recorder(directory)
            path = recorder.directory / "audio_timing.ndjson"
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[1]["audio_pts_value"] += 1
            path.write_text("".join(json.dumps(row)+"\n" for row in rows))
            with self.assertRaisesRegex(audio.Incomplete, "missing, overlapping"):
                audio.verify_native_recording(recorder.directory, audio.SAMPLE_RATE * 2)

    def test_short_wav_cannot_pass_native_complete_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = self.recorder(directory)
            with self.assertRaisesRegex(audio.Incomplete, "differs from the WAV"):
                audio.verify_native_recording(recorder.directory, audio.SAMPLE_RATE)

    def test_unwritten_native_buffer_cannot_qualify_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = self.recorder(directory)
            path = recorder.directory / "audio_timing.ndjson"
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[0]["status"] = "failed"
            path.write_text("".join(json.dumps(row)+"\n" for row in rows))
            with self.assertRaises(audio.Incomplete):
                audio.verify_native_recording(recorder.directory, audio.SAMPLE_RATE * 2)


if __name__ == "__main__":
    unittest.main()
