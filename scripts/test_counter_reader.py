#!/usr/bin/env python3
"""RGB-only controls for the registered counter instrument."""
import copy
from pathlib import Path
import sys
import unittest

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from counter_reader import observe


WIDTH, HEIGHT = 1280, 720
ORANGE = bytes((200, 90, 20))
# Complete synthetic strokes, independently specified rather than painting
# the reader's sampling rectangles. Their tips surround the sampled interiors.
STROKES = {
    "a": (213, 196, 241, 205), "b": (238, 205, 249, 226),
    "c": (237, 239, 247, 261), "d": (205, 261, 240, 272),
    "e": (201, 237, 214, 260), "f": (203, 204, 215, 227),
    "g": (212, 226, 239, 237),
}


def registration(x=350, y=192, w=220, h=78):
    return {"result": "PASS", "normalized_still_size": "960x540",
            "landmark_bounds": [x, y, x + w - 1, y + h - 1],
            "transform": {"kind": "dynamic_similarity", "scale_xy": [w / 138, h / 51]}}


def paint(rgb, box, color=ORANGE):
    for y in range(box[1], box[3]):
        start = (y * WIDTH + box[0]) * 3
        rgb[start:start + (box[2] - box[0]) * 3] = color * (box[2] - box[0])


def picture(mask, reg=None):
    rgb = bytearray(WIDTH * HEIGHT * 3)
    bounds = (reg or registration())["landmark_bounds"]
    for name in mask:
        box = [round((bounds[i % 2] + (v * .75 - (350 if i % 2 == 0 else 192))
                      * ((bounds[2] - bounds[0] + 1) / 220 if i % 2 == 0
                         else (bounds[3] - bounds[1] + 1) / 78)) / .75)
               for i, v in enumerate(STROKES[name])]
        paint(rgb, box)
    return rgb


def tapered_lower_strokes(mask, reg=None):
    """Whole c/e contours independent of the reader's interior rectangles.

    The six/seven-point outlines follow V1SevenX c/e: the left body leans
    inward near the middle junction and outward near the lower junction.
    Coordinates use the same reference scene as STROKES, with the left edge
    one or two pixels inside the previous c/e sampling support.
    """
    bounds = (reg or registration())["landmark_bounds"]
    image = Image.frombytes("RGB", (WIDTH, HEIGHT), bytes(picture(mask.replace("c", "").replace("e", ""), reg)))
    draw = ImageDraw.Draw(image)
    contours = {
        "c": ((244, 234), (250, 238), (248, 263), (245, 267), (238, 259), (241, 240)),
        "e": ((209, 233), (216, 238), (214, 263), (207, 267), (208, 240), (209, 235), (209, 234)),
    }
    for name in mask:
        if name in contours:
            points = [tuple(round((bounds[i] + (v * .75 - (350 if i == 0 else 192))
                                   * ((bounds[2] - bounds[0] + 1) / 220 if i == 0
                                      else (bounds[3] - bounds[1] + 1) / 78)) / .75)
                            for i, v in enumerate(point)) for point in contours[name]]
            draw.polygon(points, fill=tuple(ORANGE))
    return image.tobytes()


class CounterReaderTests(unittest.TestCase):
    def read(self, rgb, reg=None):
        return observe(bytes(rgb), WIDTH, HEIGHT, reg or registration())

    def assert_unknown(self, result):
        self.assertIn(result["state"], ("unreadable", "ambiguous"))
        self.assertIsNone(result["glyph"])
        self.assertTrue(result["reason"])
        self.assertTrue(result["anomalies"])
        for field in ("count", "mode"):
            self.assertIsNone(result["fields"][field]["value"])
            self.assertNotEqual(result["fields"][field]["state"], "readable")

    def test_digits_and_modes_are_observations_with_explicit_other_field_absence(self):
        for mask, glyph in (("abcdef", "0"), ("bc", "1"), ("abdeg", "2"),
                            ("abcdg", "3"), ("bcfg", "4"), ("acdfg", "5"),
                            ("acdefg", "6"), ("abc", "7"), ("abcdefg", "8"),
                            ("abcdfg", "9"), ("abcefg", "A"), ("def", "L"), ("de", "l")):
            with self.subTest(glyph=glyph):
                result = self.read(picture(mask))
                self.assertEqual(result["state"], "readable", result["reason"])
                self.assertEqual(result["glyph"], glyph)
                self.assertEqual(result["mask"], mask)
                self.assertEqual(result["anomalies"], [])
                self.assertEqual(result["fields"]["count"]["value"], int(glyph) if glyph.isdigit() else None)
                self.assertEqual(result["fields"]["mode"]["value"], None if glyph.isdigit() else glyph)

    def test_shift_and_axis_scale_come_from_registration(self):
        for reg in (registration(x=368, y=201), registration(w=198, h=78)):
            with self.subTest(registration=reg):
                result = self.read(picture("abdeg", reg), reg)
                self.assertEqual(result["glyph"], "2", result["reason"])
                self.assertEqual(result["alignment"]["landmark_bounds"], reg["landmark_bounds"])
        # Moving the camera image without corresponding registration is not a
        # reason to search nearby pixels for a canonical answer.
        self.assert_unknown(self.read(picture("bc", registration(x=368))))

    def test_blank_dim_and_unsupported_mask_abstain(self):
        for rgb in (picture(""), bytearray(round(v * .1) for v in picture("bc")), picture("a")):
            self.assert_unknown(self.read(rgb))

    def test_partial_sampled_segment_abstains(self):
        rgb = picture("bc")
        paint(rgb, (241, 210, 246, 217), bytes((0, 0, 0)))
        result = self.read(rgb)
        self.assert_unknown(result)
        self.assertIn("interiors", result["reason"])
        self.assertIn("b", result["segments"])

    def test_top_stroke_taper_keeps_the_positive_witness_inside_its_body(self):
        # Complete top contour, shaped after V1SevenX's five-point a glyph.
        # Its upper edge is below the old patch's first row. Paint the whole
        # tapered stroke, not the reader's sampling rectangle.
        image = Image.frombytes("RGB", (WIDTH, HEIGHT), bytes(picture("bdeg")))
        ImageDraw.Draw(image).polygon(((216, 199), (243, 200), (236, 209),
                                     (217, 209), (211, 204)), fill=tuple(ORANGE))
        result = self.read(image.tobytes())
        self.assertEqual(result["glyph"], "2", result["reason"])
        self.assertEqual(result["segments"]["a"]["active_ratio"], 1)
        # A real break through the body still refuses the numeric reading.
        ImageDraw.Draw(image).rectangle((220, 201, 232, 203), fill=(0, 0, 0))
        damaged = self.read(image.tobytes())
        self.assert_unknown(damaged)
        self.assertIn("a", damaged["reason"])

    def test_top_only_remnant_cannot_disappear_when_the_interior_moves(self):
        rgb = picture("bc")
        # Only the top strip remains. The corrected body is dark, but this
        # visible fragment on the original support must prevent a clean1.
        paint(rgb, (213, 198, 241, 200))
        result = self.read(rgb)
        self.assert_unknown(result)
        self.assertIn("a", result["reason"])

    def test_upper_right_stroke_edge_stays_inside_b_interior(self):
        # A complete upper-right stroke starts two columns inside the old
        # sampling rectangle. Those dark edge columns are not segment damage.
        rgb = picture("c")
        paint(rgb, (243, 205, 249, 226))
        result = self.read(rgb)
        self.assertEqual(result["glyph"], "1", result["reason"])
        self.assertEqual(result["segments"]["b"]["active_ratio"], 1)
        self.assertEqual(result["alignment"]["patches_xyxy"]["b"], [243, 210, 246, 222])

    def test_lower_stroke_tapers_keep_complete_glyphs_readable_across_registration_rounding(self):
        for reg in (registration(), registration(h=79), registration(h=80),
                    registration(x=382, w=222, h=79), registration(x=382, y=194, w=222, h=80)):
            for mask, glyph in (("bc", "1"), ("abdeg", "2"), ("acdfg", "5"),
                                ("acdefg", "6"), ("abcdfg", "9"), ("abcdefg", "8"),
                                ("def", "L"), ("de", "l")):
                with self.subTest(registration=reg, glyph=glyph):
                    result = self.read(tapered_lower_strokes(mask, reg), reg)
                    self.assertEqual(result["state"], "readable", result["reason"])
                    self.assertEqual(result["glyph"], glyph)

    def test_lower_stroke_body_breaks_still_refuse(self):
        for mask, name, gap in (("bc", "c", (239, 248, 248, 252)),
                                ("abdeg", "e", (207, 248, 215, 252))):
            with self.subTest(segment=name):
                rgb = bytearray(tapered_lower_strokes(mask))
                paint(rgb, gap, bytes((0, 0, 0)))
                result = self.read(rgb)
                self.assert_unknown(result)
                self.assertIn(name, result["reason"])

    def test_lower_left_edge_remnants_cannot_become_absent_segments(self):
        for mask, name, remnant in (("abdeg", "c", (239, 243, 241, 257)),
                                    ("acdfg", "e", (207, 243, 208, 255))):
            with self.subTest(segment=name):
                rgb = picture(mask)
                paint(rgb, remnant)
                result = self.read(rgb)
                self.assert_unknown(result)
                self.assertIn(name, result["reason"])

    def test_missing_and_extra_lower_strokes_do_not_preserve_the_old_literal(self):
        # A complete extra e changes 5 to 6; removing it changes 6 to 5.
        # Observation reports the visible content, never the previous value.
        self.assertEqual(self.read(tapered_lower_strokes("acdfg"))["glyph"], "5")
        self.assertEqual(self.read(tapered_lower_strokes("acdefg"))["glyph"], "6")
        # Missing c from 1 and an extra c on 2 are noncanonical masks.
        for mask in ("b", "abcdeg"):
            self.assert_unknown(self.read(tapered_lower_strokes(mask)))

    def test_left_stroke_edge_does_not_overlap_middle_interior(self):
        # A narrower upper-left stroke has a tapered-end allowance reaching
        # the middle row. Its dark left margin and its right tip are neither a
        # broken f segment nor an illuminated g segment.
        rgb = picture("de")
        paint(rgb, (210, 204, 218, 234))
        result = self.read(rgb)
        self.assertEqual(result["glyph"], "L", result["reason"])
        self.assertEqual(result["segments"]["f"]["active_ratio"], 1)
        self.assertEqual(result["segments"]["g"]["active_ratio"], 0)
        # Damage inside the corrected f interior must still be refused.
        paint(rgb, (211, 209, 215, 216), bytes((0, 0, 0)))
        self.assert_unknown(self.read(rgb))

    def test_top_stroke_junction_does_not_light_upper_left_body(self):
        # The top stroke's tapered junction can occupy a few pixels above
        # the upper-left stem. It is not an illuminated vertical f body.
        rgb = picture("abdeg")
        paint(rgb, (211, 209, 213, 212))
        self.assertEqual(self.read(rgb)["glyph"], "2")
        # A fragment in the actual vertical body remains indeterminate.
        paint(rgb, (211, 214, 213, 220))
        self.assert_unknown(self.read(rgb))

    def test_whole_cell_foreign_ink_abstains(self):
        rgb = picture("bc")
        # A second narrow '1' in the same cell misses every sampled interior.
        # The center patches still say 'bc'; the whole-cell guard must refuse
        # to report a clean single count1 over that additional visible ink.
        paint(rgb, (194, 205, 204, 226))
        paint(rgb, (194, 239, 204, 261))
        result = self.read(rgb)
        self.assert_unknown(result)
        self.assertEqual(result["mask"], "bc")
        self.assertGreater(result["cell"]["border_orange_pixels"], 0)
        self.assertEqual(result["alignment"]["cell_xyxy"], [194, 190, 260, 279])

    def test_invalid_registration_and_rgb_do_not_decode(self):
        valid = registration()
        bad = [None, {}, {**valid, "result": "FAIL"}, {**valid, "landmark_bounds": [True, 192, 569, 269]},
               {**valid, "landmark_bounds": [350, 192, 570, 269]},
               {**valid, "normalized_still_size": "1280x720"}, registration(x=10),
               registration(h=20)]
        for reg in bad:
            with self.subTest(registration=reg):
                self.assert_unknown(observe(bytes(picture("bc")), WIDTH, HEIGHT, reg))
        for rgb, width, height in ((b"", WIDTH, HEIGHT), (bytes(picture("bc")), True, HEIGHT),
                                   (bytes(picture("bc")), WIDTH, -1)):
            self.assert_unknown(observe(rgb, width, height, valid))

    def test_inputs_are_not_mutated_and_no_expected_argument_exists(self):
        reg = registration()
        before = copy.deepcopy(reg)
        rgb = bytes(picture("bc"))
        self.read(rgb, reg)
        self.assertEqual(reg, before)
        with self.assertRaises(TypeError):
            observe(rgb, WIDTH, HEIGHT, reg, expected="1")


if __name__ == "__main__":
    unittest.main()
