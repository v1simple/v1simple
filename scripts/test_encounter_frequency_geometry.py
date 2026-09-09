#!/usr/bin/env python3
"""Startup transform direction and independent-landmark refusal controls."""

import json
from pathlib import Path
import sys
import unittest

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_frequency_geometry as geometry


class FrequencyGeometryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.metadata = geometry.model_metadata()
        cls.registration = cls.metadata["reference_registration"]
        cls.reference = np.zeros((720, 1280, 3), np.uint8)
        # Only the two published display crops are needed for geometric controls.
        model = geometry._model()
        for name, region in (("scan_rgb", "scan_roi"), ("counter_rgb", "counter_validation_roi")):
            x1, y1, x2, y2 = cls.metadata[region]
            cls.reference[y1:y2, x1:x2] = model[name]
        cls.probes = np.array([[250, 210, 1], [500, 260, 1], [840, 260, 1],
                               [500, 370, 1], [840, 370, 1]], float).T

    def test_known_nonidentity_bootstrap_composes_in_reference_to_observed_direction(self):
        center = np.array([650., 310.])
        cases = [(np.eye(2), (6., -4.)), (np.diag([1.04, .99]), (2., 1.)),
                 (np.array([[1.01, .004], [-.003, .995]]), (5., -3.))]
        for linear, shift in cases:
            with self.subTest(linear=linear.tolist(), shift=shift):
                known = np.eye(3)
                known[:2, :2] = linear
                known[:2, 2] = center - linear @ center + shift
                observed = cv2.warpAffine(self.reference, known[:2], (1280, 720), flags=cv2.INTER_LINEAR)
                bounds = self.registration["landmark_bounds"]
                corners = np.array([[bounds[0]*4/3, bounds[1]*4/3, 1],
                                    [(bounds[2]+1)*4/3, (bounds[3]+1)*4/3, 1]]).T
                measured = known @ corners
                registration = {**self.registration, "landmark_bounds": [
                    round(measured[0, 0]*3/4), round(measured[1, 0]*3/4),
                    round(measured[0, 1]*3/4)-1, round(measured[1, 1]*3/4)-1]}
                result = geometry.calibrate(observed, 1280, 720, registration)
                self.assertTrue(result["qualified"], result)
                error = np.linalg.norm((np.array(result["matrix_reference_to_observed"]) @ self.probes
                                        - known @ self.probes)[:2], axis=0).max()
                self.assertLess(error, .5)
                json.dumps(result, allow_nan=False)

    def test_counter_displacement_is_refused_without_altering_scan_fit(self):
        original = geometry.calibrate(self.reference, 1280, 720, self.registration)
        changed = self.reference.copy()
        x1, y1, x2, y2 = self.metadata["counter_validation_roi"]
        changed[y1:y2, x1:x2] = 0
        changed[y1:y2, x1+4:x2+4] = self.reference[y1:y2, x1:x2]
        result = geometry.calibrate(changed, 1280, 720, self.registration)
        self.assertTrue(original["qualified"])
        self.assertFalse(result["qualified"])
        self.assertFalse(result["checks"]["counter_translation"])
        self.assertEqual(result["matrix_reference_to_observed"], original["matrix_reference_to_observed"])

    def test_missing_landmarks_and_partial_scan_refuse(self):
        for region in ("scan_roi", "counter_validation_roi", "half_scan"):
            with self.subTest(region=region):
                changed = self.reference.copy()
                x1, y1, x2, y2 = self.metadata["scan_roi" if region == "half_scan" else region]
                if region == "half_scan":
                    x1 = (x1+x2)//2
                changed[y1:y2, x1:x2] = 0
                result = geometry.calibrate(changed, 1280, 720, self.registration)
                self.assertFalse(result["qualified"])
                json.dumps(result, allow_nan=False)

    def test_uniform_dimming_does_not_move_independent_counter(self):
        changed = np.clip(self.reference.astype(float)*.75+2, 0, 255).astype(np.uint8)
        result = geometry.calibrate(changed, 1280, 720, self.registration)
        self.assertTrue(result["qualified"], result)
        self.assertLess(result["counter_translation_residual_pixels"], .1)
        self.assertEqual(result, geometry.calibrate(changed.tobytes(), 1280, 720, self.registration))

    def test_non_display_pixels_do_not_influence_fit(self):
        changed = self.reference.copy()
        changed[:150] = [240, 0, 0]
        changed[500:] = [0, 240, 0]
        self.assertEqual(geometry.calibrate(self.reference, 1280, 720, self.registration),
                         geometry.calibrate(changed, 1280, 720, self.registration))

    def test_invalid_originals_and_registration_refuse(self):
        invalid = [None, {}, {**self.registration, "result": "FAIL"},
                   {**self.registration, "landmark_bounds": [382, 194, 382, 273]},
                   {**self.registration, "landmark_bounds": [382, 194, float("nan"), 273]}]
        for registration in invalid:
            with self.subTest(registration=registration):
                self.assertFalse(geometry.calibrate(self.reference, 1280, 720, registration)["qualified"])
        for rgb, width, height in ((b"short", 1280, 720), (self.reference, 960, 540),
                                    (self.reference.astype(float), 1280, 720)):
            self.assertFalse(geometry.calibrate(rgb, width, height, self.registration)["qualified"])

    def test_sampling_rejects_invalid_matrix_and_out_of_image_measurement(self):
        for matrix in (np.eye(2), [[1, 0, 0], [0, 1, 0], [0, 0, float("nan")]],
                       [[1, 0, 0], [0, 1, 0], [.1, 0, 1]],
                       [[1, 0, 1000], [0, 1, 0], [0, 0, 1]]):
            with self.subTest(matrix=matrix), self.assertRaises(ValueError):
                geometry.canonical(self.reference, self.metadata["frequency_box"], matrix)


if __name__ == "__main__":
    unittest.main()
