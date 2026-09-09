#!/usr/bin/env python3
"""Startup identity and conservative fallback around calibrated idle reading."""
from copy import deepcopy
import hashlib
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_frequency_geometry as geometry
import encounter_frequency_idle as idle
import encounter_frequency_residual as residual


class FrequencyIdleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.metadata = geometry.model_metadata()
        cls.baseline = {"state": "ambiguous", "value": None, "reason": "original unresolved observation",
                        "original_diagnostics": {"kept": True}}
        cls.context = {**deepcopy(cls.metadata["reference_registration"]),
                       "primary_frequency_calibration": {
                           "qualified": True, "matrix_reference_to_observed": np.eye(3).tolist(),
                           "model_sha256": geometry.MODEL_SHA256}}
        # Place the recorded template into original-frame coordinates. This
        # deliberately reconstructed fixture tests integration, not camera
        # accuracy or a second independent qualification recording.
        x, y = np.meshgrid(np.arange(1280, dtype=np.float32), np.arange(720, dtype=np.float32))
        anchor, scale = cls.metadata["reference_anchor"], cls.metadata["reference_scale"]
        sx = 376 * 4 / 3 + (x + .5 - anchor[0]) / scale[0] - .5 - 450
        sy = 192 * 4 / 3 + (y + .5 - anchor[1]) / scale[1] - .5 - 252
        cls.original = cv2.remap(geometry.frequency_template(), sx.astype(np.float32),
                                 sy.astype(np.float32), cv2.INTER_LINEAR)

    def reading(self, image=None, registration=None, baseline=None):
        return idle.refine(self.baseline if baseline is None else baseline,
                           (self.original if image is None else image).tobytes(), 1280, 720,
                           self.context if registration is None else registration)

    def assert_preserved(self, result, baseline=None):
        baseline = self.baseline if baseline is None else baseline
        self.assertEqual({key: result[key] for key in baseline}, baseline)
        self.assertFalse(result.get("calibrated_idle", {}).get("accepted", False))

    def startup(self, folder):
        observed = np.zeros((720, 1280, 3), np.uint8)
        model = geometry._model()
        for name, region in (("scan_rgb", "scan_roi"), ("counter_rgb", "counter_validation_roi")):
            x1, y1, x2, y2 = self.metadata[region]
            observed[y1:y2, x1:x2] = model[name]
        path = Path(folder) / "startup.png"
        Image.fromarray(observed).save(path)
        payload = path.read_bytes()
        preflight = {"result": "PASS", "registration": deepcopy(self.metadata["reference_registration"]),
                     "source_still": {"name": path.name, "sha256": hashlib.sha256(payload).hexdigest(),
                                      "size_bytes": len(payload)}}
        return path, preflight

    def test_startup_identity_is_checked_before_fitting_and_input_is_unchanged(self):
        with tempfile.TemporaryDirectory() as folder:
            path, preflight = self.startup(folder)
            frozen = deepcopy(preflight)
            result = idle.registration_for_camera(preflight, path)
            self.assertTrue(result["primary_frequency_calibration"]["qualified"])
            self.assertEqual(preflight, frozen)
            mutations = [
                {"result": "FAIL"},
                {"registration": {**preflight["registration"], "result": "FAIL"}},
                {"registration": {**preflight["registration"], "primary_frequency_calibration": {}}},
                {"source_still": {**preflight["source_still"], "sha256": "0" * 64}},
                {"source_still": {**preflight["source_still"], "name": "other.png"}},
                {"source_still": {**preflight["source_still"], "size_bytes": 1}},
            ]
            with patch.object(geometry, "calibrate", side_effect=AssertionError("must not fit invalid input")):
                for mutation in mutations:
                    with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                        idle.registration_for_camera({**preflight, **mutation}, path)
                path.write_bytes(b"changed original image")
                with self.assertRaisesRegex(ValueError, "identity differs"):
                    idle.registration_for_camera(preflight, path)

    def test_missing_or_changed_geometry_model_rejects_startup_method(self):
        with tempfile.TemporaryDirectory() as folder:
            path, preflight = self.startup(folder)
            for failure in (FileNotFoundError("geometry model unavailable"),
                            ValueError("geometry model identity changed")):
                with self.subTest(failure=type(failure).__name__), \
                     patch.object(geometry, "calibrate", side_effect=failure):
                    # A damaged installed calibration model invalidates startup
                    # qualification; it is not an ordinary unreadable frame.
                    with self.assertRaises(type(failure)):
                        idle.registration_for_camera(preflight, path)

    def test_no_context_refused_context_and_definite_readings_keep_original(self):
        self.assertIs(self.reading(registration={}), self.baseline)
        refused = {**self.context, "primary_frequency_calibration": {
            "qualified": False, "reason": "independent counter disagrees"}}
        self.assert_preserved(self.reading(registration=refused))
        for state, value in (("readable", "24.150"), ("readable", "--.---"), ("absent", None)):
            original = {"state": state, "value": value, "reason": "existing observation"}
            with self.subTest(state=state, value=value), \
                 patch.object(geometry, "canonical", side_effect=AssertionError("definite read must not rerun")):
                self.assertIs(self.reading(baseline=original), original)

    def test_intact_template_requires_complete_dash_and_decimal_witness(self):
        baseline = deepcopy(self.baseline)
        accepted = self.reading()
        self.assertEqual((accepted["state"], accepted["value"]), ("readable", "--.---"))
        self.assertEqual(self.baseline, baseline)
        # Force only the upstream matcher positive. The actual pixel witness
        # must still reject a missing first dash and a missing decimal.
        import stbt_core
        from encounter_reader import Pixels
        for box in ((466, 293, 527, 321), (600, 339, 631, 365)):
            changed = self.original.copy()
            pixels = Pixels(changed.tobytes(), 1280, 720, self.metadata["reference_registration"])
            x1, y1, x2, y2 = pixels._bounds(box)
            changed[y1:y2, x1:x2] = 0
            with self.subTest(box=box), patch.object(stbt_core, "match", return_value=SimpleNamespace(
                    match=True, first_pass_result=1.0)):
                result = self.reading(image=changed)
                self.assert_preserved(result)
                self.assertFalse(result["calibrated_idle"]["complete_dash_decimal_witness"])

    def test_template_match_does_not_override_remaining_gray_strokes(self):
        from encounter_reader import Pixels
        changed = self.original.copy()
        pixels = Pixels(changed.tobytes(), 1280, 720, self.metadata["reference_registration"])
        x1, y1, x2, y2 = pixels._bounds((486, 270, 491, 294))
        changed[y1:y2, x1:x2] = np.clip(changed[y1:y2, x1:x2].astype(int) + 20, 0, 255)
        result = self.reading(image=changed)
        self.assert_preserved(result)
        self.assertTrue(result["calibrated_idle"]["template_match"])
        self.assertTrue(result["calibrated_idle"]["complete_dash_decimal_witness"])
        self.assertFalse(result["calibrated_idle"]["residual_ink"]["clear"])

    def test_missing_matcher_or_residual_model_does_not_promote_unknown(self):
        with patch.dict(sys.modules, {"stbt_core": None}):
            self.assert_preserved(self.reading())
        with patch.object(residual, "observe", side_effect=FileNotFoundError("residual model unavailable")):
            self.assert_preserved(self.reading())

    def test_changed_model_or_invalid_matrix_preserves_original_uncertainty(self):
        for changes in ({"model_sha256": "0" * 64},
                        {"matrix_reference_to_observed": [[1, 0, 0], [0, 1, 0], [0, 0, float("nan")]]},
                        {"matrix_reference_to_observed": [[1, 0, 900], [0, 1, 0], [0, 0, 1]]}):
            registration = deepcopy(self.context)
            registration["primary_frequency_calibration"].update(changes)
            with self.subTest(changes=changes):
                self.assert_preserved(self.reading(registration=registration))

    def test_malformed_per_frame_context_preserves_original_field(self):
        for malformed in (True, "not a calibration", ["not a calibration"]):
            with self.subTest(calibration=malformed):
                self.assert_preserved(self.reading(registration={
                    **self.context, "primary_frequency_calibration": malformed}))


if __name__ == "__main__":
    unittest.main()
