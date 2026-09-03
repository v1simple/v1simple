#!/usr/bin/env python3
"""Exercise evidence joins and refusals; media decoding is also run on retained recordings."""
from __future__ import annotations

import copy
from fractions import Fraction
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import counter_check as check
from camera_artifacts import build_capture_manifest, sha256_file
from test_camera_artifacts import fixture as camera_fixture
from test_counter_expectation import fixture as packet_fixture


class CounterCheckTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.run = self.root / "run"
        self.camera, self.camera_result, _ = camera_fixture(self.run)
        self.camera_result.update(profile={"video_size": "4x2"}, video_probe={"width": 4, "height": 2})
        self.scenario, self.stimulus, self.delivery = packet_fixture()
        # Scale independent packet fixture timestamps from nanoseconds to seconds.
        for record in self.stimulus + self.delivery:
            for key in ("requestedHostMonotonicNs", "hostMonotonicNs", "attemptedHostMonotonicNs"):
                if key in record:
                    record[key] *= 10_000_000
        self.rows = [dict(schema_version=1, phase="recording", frame_seq=166 + i,
                          source_clock="avcapture_session_synchronization_clock", callback_clock="host_monotonic",
                          source_pts_value=100 + i, source_pts_timescale=1,
                          source_duration_value=1, source_duration_timescale=1,
                          callback_host_ns=1_500_000_100 + i * 1_000_000_000,
                          host_capture_ns=1_500_000_000 + i * 1_000_000_000,
                          video_pts_value=i, video_pts_timescale=1,
                          video_duration_value=1, video_duration_timescale=1,
                          duration_ns=1_000_000_000, status="written", drop_reason=None)
                     for i in range(3)]
        self.encoded = [dict(encoded_index=i, pts=Fraction(i), duration=Fraction(1),
                             pts_value=i, time_base="1/1") for i in range(3)]
        self.window = dict(camera={}, artifacts={}, runtime_qualification={"status": "collection_only"})
        self.bind()
        self.configuration = self.root / "configuration.json"
        self.write(self.configuration, {"window_result_sha256": sha256_file(self.run / "window_result.json"),
                                        "settings": {"stealthEnabled": False}, "precondition": "fixture configuration"})
        self.out_number = 0

    @staticmethod
    def write(path, value):
        path.write_text(json.dumps(value) + "\n")

    def bind(self):
        self.write(self.run / "replay_scenario.json", self.scenario)
        for name, records in (("replay_stimulus", self.stimulus), ("replay_delivery", self.delivery)):
            path = self.run / f"{name}.ndjson"
            path.write_text("".join(json.dumps(row) + "\n" for row in records))
            self.window["artifacts"][name] = dict(path=path.name, sha256=sha256_file(path), size_bytes=path.stat().st_size)
        (self.camera / "frame_timing.ndjson").write_text("".join(json.dumps(row) + "\n" for row in self.rows))
        manifest = build_capture_manifest(camera_dir=self.camera, camera_result=self.camera_result, suite="replay")
        self.write(self.camera / "capture_manifest.json", manifest)
        self.window["camera"]["capture_id"] = manifest["capture_id"]
        self.write(self.run / "window_result.json", self.window)

    def analyze(self, offsets=(.5, 1.5), *, reader=None, configuration=True, decoder=None):
        self.out_number += 1
        output = self.root / f"result-{self.out_number}"
        output.mkdir()
        def decode(_ffmpeg, _video, indices, width, height):
            self.assertEqual((width, height), (4, 2))
            return {i: bytes([i + 1]) * width * height * 3 for i in indices}
        def observe(rgb, width, height, registration):
            self.assertEqual((width, height), (4, 2))
            self.assertEqual(registration, {"result": "PASS", "display_crop_x": .1})
            values = (1, None) if rgb[0] == 1 else (None, "L")
            return {"anomalies": [], "fields": {name: {"state": "readable", "value": value}
                                                 for name, value in zip(("count", "mode"), values)}}
        with patch.object(check.shutil, "which", side_effect=lambda name: name), \
             patch.object(check, "probe_all_video_frames", return_value=self.encoded), \
             patch.object(check, "decode_frames", side_effect=decoder or decode) as decoding, \
             patch.object(check, "observe", side_effect=reader or observe) as observing:
            result = check.analyze(self.run, list(offsets), output, self.configuration if configuration else None)
        self.assertEqual(json.loads((output / "result.json").read_text())["result"], result["result"])
        self.assertTrue((output / "report.md").is_file())
        return result, output, decoding, observing

    def test_matching_input_and_pixels_bind_exact_frames_and_narrow_scope(self):
        result, output, decoding, observing = self.analyze()
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["counts"]["matched"], 4)
        self.assertEqual(result["counts"]["excluded"], 14)
        self.assertEqual(result["full_run_correctness"], "not_evaluated")
        self.assertEqual(decoding.call_args.args[2], [0, 1])  # Source sequences are 166,167.
        self.assertEqual([s["source_frame_seq"] for s in result["samples"]], [166, 167])
        for call in observing.call_args_list:
            self.assertEqual(len(call.args), 4)  # No expected value enters the reader.
            self.assertFalse(call.kwargs)
        for sample in result["samples"]:
            self.assertEqual(sample["image_sha256"], sha256_file(output / sample["image"]))
        self.assertEqual(result["evidence"]["runtime_qualification"]["status"], "collection_only")

    def test_mismatch_is_a_sampled_discrepancy_and_survives_another_unknown(self):
        def wrong_or_unknown(rgb, *_):
            return {"anomalies": [], "fields": {"count": {"state": "readable", "value": 2},
                                                 "mode": {"state": "readable", "value": None}}}
        result, _, _, _ = self.analyze((.5, 100), reader=wrong_or_unknown)
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["counts"]["mismatched"], 1)
        self.assertEqual(result["counts"]["unresolved"], 2)
        self.assertEqual(result["counts"]["required"], 4)

    def test_missing_idle_configuration_remains_unknown(self):
        result, _, _, _ = self.analyze(configuration=False)
        self.assertEqual((result["result"], result["counts"]["matched"], result["counts"]["unresolved"]),
                         ("INCONCLUSIVE", 2, 2))

    def test_failed_frame_retention_keeps_other_samples_and_supported_mismatch(self):
        original = check.write_png
        def failing_second(path, *args):
            if path.name == "0002.png":
                raise OSError("fixture frame retention failure")
            return original(path, *args)
        def wrong_first(rgb, *_):
            values = (2, None) if rgb[0] == 1 else (None, "L")
            return {"anomalies": [], "fields": {name: {"state": "readable", "value": value}
                                                 for name, value in zip(("count", "mode"), values)}}
        with patch.object(check, "write_png", side_effect=failing_second):
            result, _, _, observing = self.analyze((.5, 1.5, 2.5), reader=wrong_first)
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["counts"]["mismatched"], 1)
        self.assertEqual(result["counts"]["matched"], 3)
        self.assertEqual(result["counts"]["unresolved"], 2)
        self.assertEqual(observing.call_count, 2)

    def test_anomalies_are_not_dropped(self):
        def anomalous(*_):
            return {"anomalies": ["foreign stroke"], "fields": {
                "count": {"state": "readable", "value": 1}, "mode": {"state": "readable", "value": None}}}
        result, _, _, _ = self.analyze((.5,), reader=anomalous)
        self.assertEqual(result["result"], "INCONCLUSIVE")

    def test_every_out_of_range_or_duplicate_resolved_request_stays_in_denominator(self):
        result, _, _, observing = self.analyze((.5, .500000001, 100))
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertEqual(result["counts"]["required"], 6)
        self.assertEqual(result["counts"]["unresolved"], 4)
        self.assertEqual(observing.call_count, 1)
        self.assertEqual(len(result["samples"]), 3)

    def test_changed_or_missing_owned_inputs_refuse_before_pixel_reading(self):
        for relative in ("replay_stimulus.ndjson", "replay_delivery.ndjson", "camera/evidence_exp50.mov",
                         "camera/frame_timing.ndjson"):
            with self.subTest(relative=relative):
                path = self.run / relative
                original = path.read_bytes()
                path.write_bytes(original + b"changed")
                result, _, _, observing = self.analyze()
                self.assertEqual(result["result"], "INCONCLUSIVE")
                self.assertEqual(result["counts"]["unresolved"], 4)
                observing.assert_not_called()
                path.write_bytes(original)

    def test_camera_swap_and_null_camera_fail_as_evidence_errors(self):
        for camera in ({"capture_id": "0" * 64}, None):
            with self.subTest(camera=camera):
                changed = copy.deepcopy(self.window); changed["camera"] = camera
                self.write(self.run / "window_result.json", changed)
                result, _, _, observing = self.analyze()
                self.assertEqual(result["result"], "INCONCLUSIVE")
                self.assertEqual(result["counts"]["unresolved"], 4)
                observing.assert_not_called()

    def test_scenario_swap_cannot_relabel_recorded_wire_input(self):
        self.scenario["samples"][0]["alerts"] = []
        self.write(self.run / "replay_scenario.json", self.scenario)
        result, _, _, observing = self.analyze()
        self.assertEqual(result["result"], "INCONCLUSIVE")
        observing.assert_not_called()

    def test_unbound_or_malformed_configuration_refuses(self):
        for value in ({"window_result_sha256": "0" * 64, "settings": {"stealthEnabled": False}},
                      {"window_result_sha256": sha256_file(self.run / "window_result.json"), "settings": None}):
            self.write(self.configuration, value)
            result, _, _, observing = self.analyze()
            self.assertEqual(result["result"], "INCONCLUSIVE")
            observing.assert_not_called()

    def test_timing_mismatch_preserves_unknown_samples(self):
        self.encoded.pop()
        result, _, decoding, observing = self.analyze()
        self.assertEqual(result["counts"]["unresolved"], 4)
        decoding.assert_not_called(); observing.assert_not_called()

    def test_decode_failure_preserves_unknown_samples(self):
        def bad_decode(*_):
            raise ValueError("failed original frame decode")
        result, _, _, observing = self.analyze(decoder=bad_decode)
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertEqual(result["counts"]["unresolved"], 4)
        observing.assert_not_called()

    def test_duplicate_and_nonfinite_json_are_rejected_before_interpretation(self):
        for raw in ('{"camera":{},"camera":{}}', '{"runtime_identity":{"x":NaN}}', '{"x":Infinity}',
                    '{"runtime_identity":{"x":1e400}}'):
            (self.run / "window_result.json").write_text(raw)
            result, _, _, observing = self.analyze()
            self.assertEqual(result["result"], "INCONCLUSIVE")
            observing.assert_not_called()

    def test_invalid_offsets_have_a_saved_inconclusive_result(self):
        for offsets in ((float("nan"),), (float("inf"),), (-1,), (.5, .5)):
            with self.subTest(offsets=offsets):
                result, _, _, observing = self.analyze(offsets)
                self.assertEqual(result["result"], "INCONCLUSIVE")
                self.assertEqual(result["counts"]["required"], 2 * len(offsets))
                observing.assert_not_called()

    def test_extreme_finite_offset_does_not_drop_other_samples(self):
        result, _, _, observing = self.analyze((.5, 1e308))
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertEqual(result["counts"]["matched"], 2)
        self.assertEqual(result["counts"]["unresolved"], 2)
        self.assertEqual(observing.call_count, 1)

    def test_cli_existing_output_is_not_overwritten_and_returns_two(self):
        output = self.root / "existing"; output.mkdir()
        marker = output / "keep"; marker.write_text("original")
        process = subprocess.run([sys.executable, str(Path(check.__file__)), "--run-dir", str(self.run),
                                  "--at", ".5", "--out", str(output)], capture_output=True, text=True)
        self.assertEqual(process.returncode, 2)
        self.assertNotIn("Traceback", process.stderr)
        self.assertEqual(marker.read_text(), "original")

    def test_json_publication_is_atomic_on_serialization_failure(self):
        path = self.root / "invalid.json"
        with self.assertRaises(ValueError):
            check.save_json(path, {"bad": float("nan")})
        self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main()
