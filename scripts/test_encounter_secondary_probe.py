#!/usr/bin/env python3
"""Focused checks for the expectation-blind secondary text probe."""
from __future__ import annotations

import base64
import hashlib
from pathlib import Path
import sys
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import encounter_secondary_probe as probe


class FakePixels:
    def __init__(self, rgb, width, height, registration):
        del rgb, width, height, registration

    def crop(self, box):
        value = box[0] % 251
        result = np.zeros((36, 181, 3), dtype=np.uint8)
        result[:, :, 0] = value
        result[:, :, 1] = value + 1
        result[:, :, 2] = value + 2
        return result


class SecondaryProbeTests(unittest.TestCase):
    def test_fixed_profiles_have_canonical_bound_shape_and_hash(self):
        original = probe.Pixels
        probe.Pixels = FakePixels
        try:
            result = probe.observe(b"pixels", 1, 1, {"landmark_bounds": [0, 0, 1, 1]})
        finally:
            probe.Pixels = original
        self.assertEqual(set(result), {"schema_version", "method_version",
                                       "profile_schema", "cards"})
        self.assertEqual(result["profile_schema"]["normalization"], "none")
        self.assertEqual([card["reference_bounds"] for card in result["cards"]],
                         [list(box) for box in probe.TEXT_BOXES])
        for card in result["cards"]:
            raw = base64.b64decode(card["profile_b64"], validate=True)
            self.assertEqual(len(raw), probe.PROFILE_BYTE_COUNT)
            self.assertEqual(hashlib.sha256(raw).hexdigest(), card["profile_sha256"])
            self.assertEqual(base64.b64encode(raw).decode("ascii"), card["profile_b64"])

    def test_probe_signature_accepts_no_expected_or_timeline_input(self):
        self.assertEqual(probe.observe.__code__.co_varnames[:4],
                         ("rgb", "width", "height", "registration"))

    def test_profile_refuses_regions_smaller_than_frozen_grid(self):
        with self.assertRaisesRegex(ValueError, "too small"):
            probe._profile(np.zeros((8, 46, 3), dtype=np.uint8))


if __name__ == "__main__":
    unittest.main()
