"""Focused reader checks; the retained recordings supply the real negative control."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

from bench import read_replay_fields as reader
from bench.read_replay_fields import CARD_TEMPLATES, read_pixels, secondary_identities


class ReadReplayFieldsTest(unittest.TestCase):
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
