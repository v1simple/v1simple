#!/usr/bin/env python3
"""Recorded template plus paired added strokes qualify the residual veto."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent / "bench"))
import encounter_frequency_geometry as geometry
import encounter_frequency_residual as residual


class FrequencyResidualTests(unittest.TestCase):
    @staticmethod
    def recorded_template():
        import cv2
        # Only the fixed frequency crop was retained in the reference model.
        # Replicate its outer border to supply the guard's local-background
        # margin. This is a test fixture, not a second camera observation.
        return cv2.copyMakeBorder(geometry.frequency_template(), 12, 10, 15, 10,
                                  cv2.BORDER_REPLICATE)

    def test_recorded_template_and_added_gray_stroke(self):
        import numpy as np
        intact = self.recorded_template()
        self.assertTrue(residual.observe(intact)["clear"])
        changed = intact.copy()
        changed[270 - residual.BOX[1]:294 - residual.BOX[1],
                486 - residual.BOX[0]:491 - residual.BOX[0]] = np.clip(
                    changed[270 - residual.BOX[1]:294 - residual.BOX[1],
                            486 - residual.BOX[0]:491 - residual.BOX[0]].astype(int) + 3,
                    0, 255).astype(np.uint8)
        self.assertFalse(residual.observe(changed)["clear"])

    def test_thin_middle_extension_is_not_lost_between_broad_supports(self):
        import numpy as np
        intact = self.recorded_template()
        self.assertTrue(residual.observe(intact)["clear"])
        changed = intact.copy()
        x1, y1, x2, y2 = residual.THIN_SUPPORT
        patch_pixels = changed[y1 - residual.BOX[1]:y2 - residual.BOX[1],
                               x1 - residual.BOX[0]:x2 - residual.BOX[0]]
        patch_pixels[:] = np.clip(patch_pixels.astype(int) + 20, 0, 255)
        self.assertFalse(residual.observe(changed)["clear"])

    def test_red_digit_remnant_is_refused(self):
        import numpy as np
        changed = self.recorded_template()
        patch_pixels = changed[25:49, 51:56]
        patch_pixels[:, :, 0] = np.clip(patch_pixels[:, :, 0].astype(int) + 10, 0, 255)
        self.assertFalse(residual.observe(changed)["clear"])

    def test_invalid_input_and_changed_model_do_not_assert_clear(self):
        import base64
        import numpy as np
        with self.assertRaises(ValueError):
            residual.observe(np.zeros((20, 20, 3), dtype=np.uint8))
        frame = self.recorded_template()
        residual._model.cache_clear()
        try:
            with patch.object(Path, "read_bytes", return_value=base64.b64encode(b"changed model")):
                with self.assertRaisesRegex(ValueError, "model identity changed"):
                    residual.observe(frame)
        finally:
            residual._model.cache_clear()


if __name__ == "__main__":
    unittest.main()
