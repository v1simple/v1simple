#!/usr/bin/env python3
"""Focused checks for expectation-blind redraw measurements."""
from __future__ import annotations

import sys
from pathlib import Path
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import encounter_redraw_probe as probe


class FakePixels:
    def __init__(self, rgb, width, height, registration):
        del rgb, width, height, registration

    def level(self, box):
        value = sum(box) % 200 + 20
        return np.full((12, 48), value, dtype=float)


class RedrawProbeTests(unittest.TestCase):
    def test_measurements_have_fixed_shapes_and_no_expected_input(self):
        original = probe.Pixels
        probe.Pixels = FakePixels
        try:
            result = probe.observe(b"pixels", 1, 1, {"landmark_bounds": [0, 0, 1, 1]})
        finally:
            probe.Pixels = original
        self.assertEqual(result["method_version"], 1)
        self.assertEqual(len(result["main_bars"]["bars"]), 6)
        self.assertTrue(all(len(item["profile"]) == 16
                            for item in result["main_bars"]["bars"]))
        self.assertEqual(len(result["muted_badge"]["profile"]), 8)
        self.assertEqual(len(result["primary_frequency"]["cells"]), 5)
        self.assertTrue(all(len(cell["segments"]) == 7
                            and len(cell["hole_ink_fractions"]) == 2
                            for cell in result["primary_frequency"]["cells"]))
        self.assertIn(result["primary_frequency"]["decimal"]["state"],
                      {"on", "off", "partial"})

    def test_profile_rejects_regions_smaller_than_grid(self):
        with self.assertRaisesRegex(ValueError, "too small"):
            probe._profile(np.ones((1, 3)), 2, 4)


if __name__ == "__main__":
    unittest.main()
