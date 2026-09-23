"""Focused reader checks; the retained recordings supply the real negative control."""

import unittest

import numpy as np

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


if __name__ == "__main__":
    unittest.main()
