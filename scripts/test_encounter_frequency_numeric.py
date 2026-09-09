#!/usr/bin/env python3
"""Numeric-decimal fallback controls; synthetic images are not camera proof."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

import cv2
import numpy as np
from PIL import ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent / 'bench'))
import encounter_frequency_geometry as geometry
import encounter_frequency_numeric as numeric
from encounter_reader import Pixels, _frequency
from test_encounter_reader import display, REGISTRATION, ORANGE


class FrequencyNumericTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reference = geometry.model_metadata()['reference_registration']
        original = Pixels(bytes(1280 * 720 * 3), 1280, 720, REGISTRATION)
        reference = Pixels(bytes(1280 * 720 * 3), 1280, 720, cls.reference)
        cls.to_reference = np.eye(3)
        for axis in (0, 1):
            cls.to_reference[axis, axis] = reference.scale[axis] / original.scale[axis]
            cls.to_reference[axis, 2] = (reference.anchor[axis] - original.anchor[axis]
                                       * cls.to_reference[axis, axis])
        # A known four-image-pixel coarse horizontal offset puts the whole-dot
        # sample across its circular edge. All five broad numeric glyphs remain
        # canonical. The independent shape renderer paints complete strokes,
        # not the reader's sampling rectangles. No event-dependent fit occurs.
        cls.context = {**deepcopy(cls.reference), 'landmark_bounds': [
            value + (3 if index % 2 == 0 else 0)
            for index, value in enumerate(cls.reference['landmark_bounds'])],
            'primary_frequency_calibration': {
                'qualified': True, 'matrix_reference_to_observed': np.eye(3).tolist(),
                'model_sha256': geometry.MODEL_SHA256}}

    def image(self, value='34.700', operation=None):
        image = display(value)
        draw = ImageDraw.Draw(image)
        # A complete circular decimal, drawn independently of sampled interiors.
        draw.rectangle((584, 346, 603, 362), fill='black')
        draw.ellipse((587, 347, 601, 361), fill=ORANGE)
        if operation:
            operation(draw)
        return cv2.warpAffine(np.asarray(image), self.to_reference[:2], (1280, 720),
                              flags=cv2.INTER_LINEAR)

    def original(self, image):
        return _frequency(Pixels(image.tobytes(), 1280, 720, self.context))

    def read(self, image=None, baseline=None, context=None):
        image = self.image() if image is None else image
        baseline = self.original(image) if baseline is None else baseline
        return numeric.refine(baseline, image.tobytes(), 1280, 720,
                              self.context if context is None else context)

    def test_startup_alignment_resolves_only_decimal_and_repeats_original_digits(self):
        image = self.image()
        baseline = self.original(image)
        frozen = deepcopy(baseline)
        self.assertEqual(baseline['reason'], numeric.ELIGIBLE_REASON)
        self.assertEqual(numeric.original_literal(baseline), '34.700')
        observed = self.read(image, baseline)
        self.assertEqual((observed['state'], observed['value']), ('readable', '34.700'))
        self.assertTrue(observed['calibrated_numeric_decimal']['accepted'])
        self.assertEqual(baseline, frozen)

    def test_definite_and_other_refusal_routes_do_not_enter_pixel_fallback(self):
        base = self.original(self.image())
        reasons = ('incomplete dim numeric stroke contrast',
                   'frequency does not form five canonical numeric glyphs',
                   'Five complete dash strokes and the decimal are observed; additional pixel contrast prevents confirming a cleared frequency field.',
                   'dim numeric ink enters a glyph background interior',
                   'registered display visibility witnesses are dark or occluded')
        readings = [{**base, 'reason': reason} for reason in reasons]
        readings += [{**base, 'state': state, 'value': value}
                     for state, value in (('readable', '34.700'), ('readable', '--.---'),
                                          ('absent', None), ('unreadable', None))]
        image = self.image()
        with patch.object(cv2, 'warpAffine', side_effect=AssertionError('must not sample')):
            for baseline in readings:
                with self.subTest(state=baseline['state'], reason=baseline.get('reason')):
                    self.assertIs(self.read(image, baseline), baseline)

    def test_original_masks_must_establish_all_five_digits_without_partial_strokes(self):
        original = self.original(self.image())
        mutations = []
        short = deepcopy(original)
        short['cells'].pop()
        mutations.append(short)
        partial = deepcopy(original)
        partial['cells'][0]['segments']['a']['state'] = 'partial'
        mutations.append(partial)
        contradictory = deepcopy(original)
        contradictory['cells'][0]['mask'] = 'abcdefg'
        mutations.append(contradictory)
        missing = deepcopy(original)
        del missing['cells'][0]['segments']['a']
        mutations.append(missing)
        for baseline in mutations:
            with self.subTest(cells=baseline['cells']):
                self.assertIsNone(numeric.original_literal(baseline))
                self.assertIs(self.read(baseline=baseline), baseline)

    def test_corrected_pixels_cannot_change_original_digits(self):
        baseline = self.original(self.image('34.700'))
        # The aligned image itself clearly says 34.708. A previous literal
        # supplied to this boundary must not be used to erase that difference.
        changed = self.image('34.708')
        aligned = _frequency(Pixels(changed.tobytes(), 1280, 720, self.reference))
        self.assertEqual((aligned['state'], aligned['value']), ('readable', '34.708'))
        self.assertIs(self.read(changed, baseline), baseline)

    def test_missing_or_partial_decimal_is_never_inserted(self):
        operations = {
            'whole': (584, 345, 604, 363),
            'top_half': (584, 345, 604, 354),
            'left_half': (584, 345, 594, 363),
            'quarter': (584, 345, 594, 354),
        }
        for name, box in operations.items():
            with self.subTest(missing=name):
                image = self.image(operation=lambda draw: draw.rectangle(box, fill='black'))
                baseline = self.original(image)
                self.assertIn(baseline['state'], ('ambiguous', 'unreadable'))
                self.assertIs(self.read(image, baseline), baseline)

    def test_partial_numeric_stroke_and_extra_glyph_hole_ink_stay_refused(self):
        operations = {
            'partial_top': lambda draw: draw.rectangle((621, 255, 645, 273), fill='black'),
            'hole_ink': lambda draw: draw.rectangle((482, 278, 494, 292), fill=ORANGE),
        }
        for name, operation in operations.items():
            with self.subTest(operation=name):
                image = self.image(operation=operation)
                baseline = self.original(image)
                self.assertIn(baseline['state'], ('ambiguous', 'unreadable'))
                self.assertIs(self.read(image, baseline), baseline)

    def test_new_canonical_stroke_is_observed_instead_of_borrowing_parent_literal(self):
        image = self.image(operation=lambda draw: draw.rectangle(
            (769, 300, 824, 314), fill=ORANGE))
        baseline = self.original(image)
        self.assertEqual(numeric.original_literal(baseline), '34.708')
        observed = self.read(image, baseline)
        self.assertEqual((observed['state'], observed['value']), ('readable', '34.708'))

    def test_refused_missing_and_malformed_contexts_preserve_original(self):
        image = self.image()
        baseline = self.original(image)
        contexts = [{}]
        contexts += [{'primary_frequency_calibration': value}
                     for value in (None, True, 'invalid', [], {'qualified': False})]
        changes = [
            {'model_sha256': '0' * 64},
            {'matrix_reference_to_observed': [[1, 0], [0, 1]]},
            {'matrix_reference_to_observed': [[1, 0, 0], [0, 1, 0], [0, 0, float('nan')]]},
            {'matrix_reference_to_observed': [[1, 0, 0], [0, 1, 0], [.1, 0, 1]]},
            {'matrix_reference_to_observed': [[1, 0, 900], [0, 1, 0], [0, 0, 1]]},
        ]
        for change in changes:
            context = deepcopy(self.context)
            context['primary_frequency_calibration'].update(change)
            contexts.append(context)
        for context in contexts:
            with self.subTest(context=context):
                self.assertIs(self.read(image, baseline, context), baseline)
        self.assertIs(numeric.refine(baseline, b'short', 1280, 720, self.context), baseline)
        self.assertIs(numeric.refine(baseline, image.tobytes(), 960, 540, self.context), baseline)
        with patch.object(geometry, 'model_metadata', side_effect=FileNotFoundError('missing model')):
            self.assertIs(self.read(image, baseline), baseline)


if __name__ == '__main__':
    unittest.main()
