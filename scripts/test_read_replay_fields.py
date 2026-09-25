"""Focused reader checks; the retained recordings supply the real negative control."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np
from PIL import Image

from bench import read_replay_fields as reader
from bench.read_replay_fields import CARD_TEMPLATES, read_pixels, secondary_identities


class ReadReplayFieldsTest(unittest.TestCase):
    @staticmethod
    def radar_frame():
        image = np.zeros((720, 1280, 3), dtype=np.uint8)
        orange = (240, 100, 40)

        def digit(character, x, y):
            # Independent segment drawing: top, middle, bottom, then the sides.
            segments = {"1": (4, 6), "2": (0, 1, 2, 4, 5),
                        "3": (0, 1, 2, 4, 6), "4": (1, 3, 4, 6),
                        "5": (0, 1, 2, 3, 6), "0": (0, 2, 3, 4, 5, 6)}
            rectangles = ((0, 8, 0, 40), (32, 40, 0, 40), (64, 72, 0, 40),
                          (8, 32, 0, 8), (8, 32, 32, 40),
                          (40, 64, 0, 8), (40, 64, 32, 40))
            for index in segments[character]:
                y0, y1, x0, x1 = rectangles[index]
                image[y + y0:y + y1, x + x0:x + x1] = orange

        for character, x in zip("24150", (465, 520, 600, 660, 715)):
            digit(character, x, 280)
        image[342:352, 580:590] = orange
        digit("3", 250, 195)
        image[335:375, 335:390] = (240, 240, 240)  # K field.
        image[210:285, 1030:1150] = orange  # FRONT field.
        for index, character in enumerate("MRCT"):
            x = 460 + 19 * index
            image[385:409, x:x + 16][CARD_TEMPLATES[character]] = (200, 40, 170)
        image[393:401, 424:444] = (240, 240, 240)
        return image

    @staticmethod
    def camera_pose(image, scale=1., dx=0, dy=0):
        moved = np.asarray(Image.fromarray(image).transform(
            (1280, 720), Image.Transform.AFFINE,
            (1 / scale, 0, -dx / scale, 0, 1 / scale, -dy / scale),
            resample=Image.Resampling.BILINEAR))
        x, y, width, height = reader.READER_REFERENCE_CROP
        registration = {"result": "PASS", "transform": {
            "kind": "dynamic_similarity",
            "crop_fractions": [scale * x + dx / 1280, scale * y + dy / 720,
                               scale * width, scale * height]}}
        return moved, registration

    def compare_frame(self, image, registration):
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary)
            camera = run / "camera"
            camera.mkdir()
            (run / "window_result.json").write_text(json.dumps({
                "result": "COMPLETE", "runtime_qualification": {"status": "qualified"},
                "camera": {"result": "CAPTURED", "capture_id": "fixture",
                           "video_timing_verification_result": {"status": "verified"}},
                "git_sha": "fixture", "runtime_identity": {"image_id": "fixture"},
                "artifacts": {"replay_display_contract": {"sha256": "fixture"}}}))
            expected = {"alerts": [
                {"priority": True, "band": "k", "frequencyMHz": 24150, "direction": "FRONT"},
                {"priority": False, "band": "k", "frequencyMHz": 24125,
                 "direction": "SIDE", "photoType": 1}],
                "muted": False, "bogeyCounterChar": "3", "bandBlink": False, "arrowBlink": False}
            (run / "replay_stimulus.ndjson").write_text("".join(json.dumps({
                "stimulusSequence": index, "requestedHostMonotonicNs": 1_000_000_000,
                "expected": expected}) + "\n" for index in range(1, 4)))
            (camera / "frame_timing.ndjson").write_text("".join(json.dumps({
                "status": "written", "host_capture_ns": when,
                "video_pts_value": when, "video_pts_timescale": 1_000_000_000}) + "\n"
                for when in (1_000_000_000, 1_180_000_000)))
            (camera / "camera_preflight.json").write_text(json.dumps({"registration": registration}))
            (camera / "evidence_fixture.mov").touch()
            with mock.patch.object(reader, "frame_at", return_value=(1.18, image)):
                return reader.compare_run(run)

    def test_comparison_reads_registered_translation_and_scale(self):
        image = self.radar_frame()
        for scale, dx, dy in ((1., 0, 0), (1., 0, 60), (1., -80, -60), (.8, 80, 80)):
            with self.subTest(scale=scale, dx=dx, dy=dy):
                moved, registration = self.camera_pose(image, scale, dx, dy)
                report = self.compare_frame(moved, registration)
                self.assertEqual("PASS", report["result"], report)
                self.assertEqual(1, report["secondaryCardTextDirection"]["matched"])

    def test_registered_comparison_still_detects_wrong_visible_band(self):
        image = self.radar_frame()
        image[335:375, 335:390] = 0
        image[405:445, 335:390] = (240, 240, 240)  # X instead of the requested K.
        moved, registration = self.camera_pose(image, dy=60)
        report = self.compare_frame(moved, registration)
        self.assertEqual("FAIL", report["result"])
        self.assertIn({"sequence": 2, "videoSeconds": 1.18, "field": "band",
                       "expected": "k", "observed": "x", "category": "mismatched"},
                      report["exceptions"])

    def test_comparison_requires_usable_registration_transform(self):
        image = self.radar_frame()
        for crop in (None, [0, 0, 0, .5], [0, 0, 2, .5], [0, 0, float("nan"), .5]):
            with self.subTest(crop=crop), self.assertRaisesRegex(ValueError, "transform|crop"):
                self.compare_frame(image, {"result": "PASS", "transform": {
                    "kind": "dynamic_similarity", "crop_fractions": crop}})

    def test_photo_subtype_is_read_from_pixels_without_expected_input(self):
        image = np.zeros((720, 1280, 3), dtype=np.uint8)
        for index, character in enumerate("MRCT"):
            x = 460 + 19 * index
            image[385:409, x:x + 16][CARD_TEMPLATES[character]] = (200, 40, 170)
        image[393:401, 424:444] = (240, 240, 240)  # Card's SIDE arrow.

        self.assertEqual((True, "MRCT", "SIDE"), read_pixels(image)["cards"][0])

    def test_photo_expectation_uses_subtype_not_frequency(self):
        stimulus = {"expected": {"alerts": [
            {"band": "k", "frequencyMHz": 24125, "photoType": 1,
             "direction": "REAR", "priority": False}]}}
        self.assertEqual([("MRCT", "REAR")], secondary_identities(stimulus))

    def test_numeric_photo_card_cannot_match_subtype(self):
        image = np.zeros((720, 1280, 3), dtype=np.uint8)
        for index, character in enumerate("24.125"):
            x = 488 + 19 * index
            image[385:409, x:x + 16][CARD_TEMPLATES[character]] = (240, 240, 240)
        image[393:401, 424:444] = (240, 240, 240)

        observed = read_pixels(image)["cards"][0]
        self.assertEqual((True, "24.125", "SIDE"), observed)
        self.assertNotEqual(("MRCT", "SIDE"), observed[1:])

    def test_numeric_card_recovers_vertical_camera_shift(self):
        image = np.zeros((720, 1280, 3), dtype=np.uint8)
        for index, character in enumerate("24.150"):
            x = 488 + 19 * index
            image[388:412, x:x + 16][CARD_TEMPLATES[character]] = (240, 240, 240)
        image[393:401, 424:444] = (240, 240, 240)

        self.assertEqual((True, "24.150", "SIDE"), read_pixels(image)["cards"][0])

    def test_post_capture_report_has_distinct_exit_statuses(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "visual_fields_result.json"
            for result, status in (("PASS", 0), ("FAIL", 1), ("INCONCLUSIVE", 2)):
                with (mock.patch.object(sys, "argv", ["reader", "unused", "--output", str(output)]),
                      mock.patch.object(reader.shutil, "which", return_value="ffmpeg"),
                      mock.patch.object(reader, "compare_run", return_value={"result": result})):
                    with self.assertRaises(SystemExit) as exit_result:
                        reader.main()
                self.assertEqual(status, exit_result.exception.code)
                self.assertEqual(result, json.loads(output.read_text())["result"])

    def test_incomplete_raw_capture_cannot_receive_visual_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary)
            (run / "window_result.json").write_text('{"result":"COLLECTION_FAILED"}')
            with self.assertRaisesRegex(ValueError, "not complete and qualified"):
                reader.compare_run(run)


if __name__ == "__main__":
    unittest.main()
