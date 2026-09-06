#!/usr/bin/env python3
"""Synthetic control images for the offline encounter reader.

These are image-reader checks, not camera, panel, or firmware evidence.
"""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_reader as reader
from test_counter_reader import picture, registration

WIDTH, HEIGHT = 1280, 720
REGISTRATION = registration(x=376, y=192, w=220, h=79)
ORANGE = (220, 100, 10)
# Full synthetic digit strokes, independently drawn as shapes. In particular,
# tests do not import or paint only the observer's sampling rectangles.
DIGITS = {"0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg",
          "4": "bcfg", "5": "acdfg", "6": "acdefg", "7": "abc",
          "8": "abcdefg", "9": "abcdfg"}
STROKES = {"a": (5, 255, 64, 273), "b": (46, 271, 64, 301),
           "c": (45, 315, 62, 345), "d": (0, 347, 61, 361),
           "e": (0, 316, 18, 345), "f": (0, 271, 18, 300),
           "g": (5, 300, 60, 314)}


def display(frequency="68.902"):
    im = Image.frombytes("RGB", (WIDTH, HEIGHT), bytes(picture("bc", REGISTRATION)))
    draw = ImageDraw.Draw(im)
    draw.rectangle((220, 338, 290, 355), fill=(0, 230, 20))
    draw.rectangle((1010, 420, 1140, 436), fill=(0, 230, 20))
    if frequency is not None:
        for origin, digit in zip((454, 520, 616, 688, 764), frequency.replace(".", "")):
            for segment in DIGITS[digit]:
                x1, y1, x2, y2 = STROKES[segment]
                draw.rectangle((origin + x1, y1, origin + x2, y2), fill=ORANGE)
        draw.ellipse((585, 347, 602, 361), fill=ORANGE)
    return im


def arrow(im, direction, color=ORANGE):
    draw = ImageDraw.Draw(im)
    shapes = {
        "front": ((987, 289), (1077, 187), (1165, 289), (1123, 289), (1123, 301), (1034, 301), (1034, 289)),
        "side": ((988, 328), (1029, 297), (1029, 314), (1127, 314), (1127, 297), (1166, 329), (1127, 360), (1127, 343), (1029, 343), (1029, 360)),
        "rear": ((1028, 367), (1050, 367), (1050, 358), (1103, 358), (1103, 367), (1128, 367), (1077, 398)),
    }
    draw.polygon(shapes[direction], fill=color)


def bars(im, count):
    draw = ImageDraw.Draw(im)
    for n, y in enumerate((398, 361, 324, 287, 249, 212)):
        draw.rectangle((885, y, 952, y + 18), fill=(90, 230, 20) if n < count else (12, 15, 14))


def card(im, left, direction, count):
    draw = ImageDraw.Draw(im)
    draw.rectangle((left, 366, left + 228, 451), fill=(0, 0, 80))
    draw.rectangle((left + 53, 382, left + 215, 407), fill=(210, 210, 210))
    if direction == "side":
        draw.rectangle((left + 18, 391, left + 40, 398), fill="white")
    elif direction == "front":
        draw.polygon(((left + 29, 385), (left + 17, 406), (left + 41, 406)), fill="white")
    else:
        draw.polygon(((left + 17, 385), (left + 41, 385), (left + 29, 407)), fill="white")
    draw.rectangle((left + 12, 419, left + 218, 448), fill=(10, 10, 10))
    for i in range(6):
        rect = (left + 16 + 33 * i, 426, left + 44 + 33 * i, 443)
        draw.rectangle(rect, fill=(90, 240, 30) if i < count else None,
                       outline=(35, 85, 25), width=2)


def ocr_result(*texts):
    return [{"rows": [{"candidates": [{"text": text, "confidence": 1.0}]}]} for text in texts]


class EncounterReaderTests(unittest.TestCase):
    def read(self, im):
        return reader.observe(im.tobytes(), *im.size, REGISTRATION)

    def test_arbitrary_frequency_digits_and_explicit_visible_absence(self):
        # Includes digits not present in the original labeled recordings.
        for frequency in ("68.902", "12.345", "97.681"):
            with self.subTest(frequency=frequency):
                observed = self.read(display(frequency))["primary_frequency"]
                self.assertEqual((observed["state"], observed["value"]), ("readable", frequency), observed)
        observed = self.read(display(None))
        self.assertEqual(observed["primary_frequency"]["state"], "absent")
        self.assertIsNone(observed["primary_frequency"]["value"])
        self.assertEqual(observed["secondary"]["value"], [])
        self.assertEqual(observed["main_bars"]["value"], 0)

    def test_erased_whole_stroke_changes_observed_digit(self):
        im = display("98.902")
        # Remove f completely: a true image change from 9 to 3.
        ImageDraw.Draw(im).rectangle((454, 271, 474, 299), fill="black")
        observed = self.read(im)["primary_frequency"]
        self.assertEqual(observed["state"], "readable", observed)
        self.assertEqual(observed["value"], "38.902")

    def test_fading_center_stroke_refuses_while_uniform_eight_is_readable(self):
        # Both dim centers exceed the absolute illuminated-pixel threshold.
        # A decoder that only combines on segments would wrongly certify 8.
        for brightness in (115, 175):
            im = display("34.700")
            ImageDraw.Draw(im).rectangle((693, 300, 748, 314), fill=(brightness, 15, 10))
            observed = self.read(im)["primary_frequency"]
            self.assertEqual(observed["state"], "ambiguous", observed)
            self.assertIsNone(observed["value"])
            self.assertIn("inconsistent illuminated", observed["reason"])
        im = display("34.700")
        ImageDraw.Draw(im).rectangle((693, 300, 748, 314), fill=ORANGE)
        observed = self.read(im)["primary_frequency"]
        self.assertEqual(observed["state"], "readable", observed)
        self.assertEqual(observed["value"], "34.780")

    def test_partial_stroke_and_foreign_ink_refuse(self):
        partial = display()
        ImageDraw.Draw(partial).rectangle((507, 325, 513, 330), fill="black")
        foreign = display()
        ImageDraw.Draw(foreign).rectangle((480, 276, 488, 293), fill=ORANGE)
        dim = Image.fromarray((np.asarray(display()).astype(float) * .13).astype(np.uint8))
        for im in (partial, foreign, dim):
            observed = self.read(im)["primary_frequency"]
            self.assertIn(observed["state"], ("ambiguous", "unreadable"), observed)
            self.assertIsNone(observed["value"])

    def test_black_occluded_and_white_frames_are_unknown_not_absent(self):
        occluded = display()
        ImageDraw.Draw(occluded).rectangle((180, 180, 1170, 460), fill="black")
        for im in (Image.new("RGB", (WIDTH, HEIGHT)), Image.new("RGB", (WIDTH, HEIGHT), "white"), occluded):
            observed = self.read(im)
            for name in reader.FIELDS:
                self.assertEqual(observed[name]["state"], "unreadable", (name, observed[name]))
                self.assertIsNone(observed[name]["value"])

    def test_arrows_and_bar_counts_follow_changed_pixels(self):
        for direction in ("front", "side", "rear"):
            for count in (1, 3, 6):
                im = display()
                arrow(im, direction)
                bars(im, count)
                observed = self.read(im)
                self.assertEqual(observed["main_arrows"]["value"], [direction], observed["main_arrows"])
                self.assertEqual(observed["main_bars"]["value"], count, observed["main_bars"])
        im = display()
        arrow(im, "front", (25, 10, 10))
        self.assertEqual(self.read(im)["main_arrows"]["state"], "ambiguous")

    def test_noncontiguous_and_partial_bars_refuse(self):
        im = display()
        bars(im, 4)
        ImageDraw.Draw(im).rectangle((885, 361, 952, 379), fill="black")
        self.assertEqual(self.read(im)["main_bars"]["state"], "ambiguous")
        im = display()
        bars(im, 3)
        ImageDraw.Draw(im).rectangle((885, 362, 952, 367), fill="black")
        self.assertEqual(self.read(im)["main_bars"]["state"], "ambiguous")

    def test_secondary_associations_follow_card_positions(self):
        im = display()
        card(im, 393, "side", 3)
        card(im, 640, "rear", 1)
        with patch.object(reader, "_ocr", return_value=ocr_result("K 23.456", "Ka 36.789")):
            result = self.read(im)["secondary"]
        self.assertEqual(result["state"], "readable", result)
        self.assertEqual(result["value"], [
            {"band": "K", "frequency": "23.456", "direction": "side", "bars": 3},
            {"band": "Ka", "frequency": "36.789", "direction": "rear", "bars": 1}])
        with patch.object(reader, "_ocr", return_value=ocr_result("Ka 36.789", "K 23.456")):
            swapped = self.read(im)["secondary"]["value"]
        self.assertEqual(swapped[0]["frequency"], "36.789")
        self.assertEqual(swapped[0]["direction"], "side")
        self.assertEqual(swapped[1]["frequency"], "23.456")
        ImageDraw.Draw(im).rectangle((640, 366, 870, 453), fill="black")
        with patch.object(reader, "_ocr", return_value=ocr_result("K 23.456")):
            self.assertEqual(len(self.read(im)["secondary"]["value"]), 1)

    def test_ocr_failure_or_malformed_frequency_is_unknown_not_empty(self):
        im = display()
        card(im, 393, "front", 4)
        for response in (None, ocr_result("K 23456"), ocr_result("K 2?.456")):
            with patch.object(reader, "_ocr", return_value=response):
                result = self.read(im)["secondary"]
            self.assertEqual(result["state"], "unreadable", result)
            self.assertIsNone(result["value"])
            self.assertEqual(len(result["partial_cards"]), 1)

    def test_dim_card_content_does_not_become_empty_or_invent_text(self):
        for left in (393, 640):
            for shape in ("text", "meter", "lower_border", "background"):
                with self.subTest(left=left, shape=shape):
                    im = display()
                    draw = ImageDraw.Draw(im)
                    draw.rectangle((393, 366, 870, 462), fill=(8, 8, 8))
                    if shape == "text":
                        for x in range(left + 56, left + 205, 18):
                            draw.rectangle((x, 382, x + 9, 405), fill=(20, 20, 20))
                    elif shape == "meter":
                        for x in range(left + 16, left + 205, 33):
                            draw.rectangle((x, 426, x + 27, 441), outline=(20, 20, 20), width=2)
                    elif shape == "lower_border":
                        draw.line((left + 8, 450, left + 220, 450), fill=(20, 20, 20), width=2)
                    else:
                        draw.rectangle((left + 8, 377, left + 220, 451), fill=(20, 20, 20))
                    # Even a confident OCR response cannot make this dim ink
                    # readable. Presence and identity have separate evidence.
                    with patch.object(reader, "_ocr", return_value=ocr_result("K 23.456")):
                        result = self.read(im)["secondary"]
                    self.assertEqual(result["state"], "unreadable", result)
                    self.assertIsNone(result["value"])
                    self.assertEqual(len(result["partial_cards"]), 1)
                    self.assertIsNone(result["partial_cards"][0]["frequency"])

    def test_dim_presence_uses_local_background_and_excludes_upper_light_spill(self):
        for background in (0, 8, 15):
            im = display()
            draw = ImageDraw.Draw(im)
            draw.rectangle((393, 366, 870, 462), fill=(background,) * 3)
            draw.rectangle((401, 366, 614, 373), fill=(24, 0, 0))
            with patch.object(reader, "_ocr", return_value=None):
                result = self.read(im)["secondary"]
            self.assertEqual((result["state"], result["value"]), ("readable", []), result)

    def test_presence_floor_is_explicit_for_broad_dim_content(self):
        for contrast in (7, 8):
            im = display()
            draw = ImageDraw.Draw(im)
            draw.rectangle((393, 366, 870, 462), fill=(8, 8, 8))
            draw.rectangle((410, 380, 600, 448), fill=(8 + contrast,) * 3)
            with patch.object(reader, "_ocr", return_value=None):
                result = self.read(im)["secondary"]
            self.assertEqual(result["state"], "readable" if contrast == 7 else "unreadable", result)

    def test_panel_black_and_gutter_light_cannot_erase_or_invent_dim_presence(self):
        for panel, gutter, ink, expected in ((12, 12, None, "readable"),
                                             (8, 15, 20, "unreadable"),
                                             (8, 15, None, "readable")):
            im = display()
            draw = ImageDraw.Draw(im)
            draw.rectangle((393, 366, 870, 453), fill=(panel,) * 3)
            draw.rectangle((393, 455, 870, 462), fill="black")
            draw.rectangle((628, 382, 633, 442), fill=(gutter,) * 3)
            if ink is not None:
                for x in range(449, 598, 18):
                    draw.rectangle((x, 382, x + 9, 405), fill=(ink,) * 3)
            with patch.object(reader, "_ocr", return_value=None):
                result = self.read(im)["secondary"]
            self.assertEqual(result["state"], expected, result)

    def test_secondary_complete_triangles_follow_pixels_with_padding_and_blur(self):
        # Renderer-owned 12 by 12 triangles project to about 20 by 20 pixels.
        # Draw full symbols; the reader does not supply these coordinates.
        for direction in ("front", "rear"):
            for dx, dy in ((0, 0), (-2, -2), (2, 2)):
                for brightness in (70, 220):
                    with self.subTest(direction=direction, shift=(dx, dy), level=brightness):
                        im = display()
                        card(im, 393, direction, 1)
                        crop = Image.new("RGB", (37, 39), (25, 25, 25))
                        draw = ImageDraw.Draw(crop)
                        vertices = ((19, 8), (9, 28), (29, 28)) if direction == "front" else (
                            (9, 10), (29, 10), (19, 30))
                        draw.polygon([(x + dx, y + dy) for x, y in vertices],
                                     fill=(brightness,) * 3)
                        crop = crop.filter(ImageFilter.GaussianBlur(.65))
                        im.paste(crop, (403, 377))
                        result = reader._card_direction(
                            reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 393)
                        self.assertEqual((result["state"], result["value"]),
                                         ("readable", direction), result)

    def test_secondary_side_rectangle_keeps_its_renderer_row(self):
        for dx, dy in ((0, 0), (-2, -2), (2, 2)):
            im = display()
            card(im, 393, "side", 1)
            region = im.crop((403, 377, 440, 416))
            ImageDraw.Draw(im).rectangle((403, 377, 439, 415), fill=(0, 0, 80))
            im.paste(region, (403 + dx, 377 + dy))
            result = reader._card_direction(
                reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 393)
            self.assertEqual((result["state"], result["value"]), ("readable", "side"), result)

    def test_secondary_remaining_triangle_base_is_not_side_direction(self):
        for clear_y in range(392, 399):
            im = display()
            card(im, 393, "rear", 1)
            draw = ImageDraw.Draw(im)
            draw.rectangle((403, 377, 439, 415), fill=(25, 25, 25))
            draw.polygon(((412, 387), (432, 387), (422, 407)), fill="white")
            draw.rectangle((403, clear_y, 439, 415), fill=(25, 25, 25))
            result = reader._card_direction(
                reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 393)
            self.assertEqual((result["state"], result["value"]), ("ambiguous", None), result)

    def test_secondary_partial_wrong_blank_and_faint_shapes_refuse(self):
        for kind in ("missing_center", "foreign_corner", "rectangle", "diamond",
                     "cropped", "blank", "faint"):
            with self.subTest(kind=kind):
                im = display()
                card(im, 393, "rear", 1)
                draw = ImageDraw.Draw(im)
                draw.rectangle((403, 377, 439, 415), fill=(25, 25, 25))
                if kind not in ("blank", "rectangle", "diamond"):
                    draw.polygon(((412, 387), (432, 387), (422, 407)),
                                 fill=(29, 29, 29) if kind == "faint" else "white")
                if kind == "missing_center":
                    draw.rectangle((419, 393, 424, 398), fill=(25, 25, 25))
                elif kind == "foreign_corner":
                    draw.rectangle((412, 400, 417, 405), fill="white")
                elif kind == "rectangle":
                    draw.rectangle((412, 387, 432, 407), fill="white")
                elif kind == "diamond":
                    draw.polygon(((422, 387), (432, 397), (422, 407), (412, 397)), fill="white")
                elif kind == "cropped":
                    draw.rectangle((403, 399, 439, 415), fill=(25, 25, 25))
                result = reader._card_direction(
                    reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 393)
                self.assertEqual((result["state"], result["value"]), ("ambiguous", None), result)

    def test_secondary_meter_uses_full_padding_past_outline_undershoot(self):
        for faint_fill in (False, True):
            with self.subTest(faint_fill=faint_fill):
                im = display()
                card(im, 393, "side", 0)
                draw = ImageDraw.Draw(im)
                draw.rectangle((405, 419, 611, 450), fill=(20, 20, 20))
                for index in range(6):
                    x = 409 + 33 * index
                    draw.rectangle((x, 426, x + 28, 443), outline=(35, 85, 25), width=2)
                    # A dark two-row compression fringe cannot define all of
                    # the local background; the remaining padding is intact.
                    draw.rectangle((x + 4, 422, x + 24, 423), fill=(5, 5, 5))
                    draw.rectangle((x + 4, 446, x + 24, 447), fill=(5, 5, 5))
                if faint_fill:
                    draw.rectangle((411, 428, 435, 441), fill=(28, 28, 28))
                result = reader._card_bars(
                    reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 393)
                self.assertEqual((result["state"], result["value"]),
                                 ("ambiguous", None) if faint_fill else ("readable", 0), result)

    def test_secondary_filled_and_outlined_shapes_survive_small_translations(self):
        # Complete rectangles, drawn independently of the reader's fit or crops.
        for count in range(7):
            for dx, dy in ((0, 0), (-2, -2), (2, 2)):
                with self.subTest(count=count, shift=(dx, dy)):
                    im = display()
                    card(im, 640, "rear", count)
                    meter = im.crop((650, 417, 860, 450))
                    ImageDraw.Draw(im).rectangle((650, 417, 860, 450), fill=(10, 10, 10))
                    im.paste(meter, (650 + dx, 417 + dy))
                    result = reader._card_bars(reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 640)
                    self.assertEqual((result["state"], result["value"]), ("readable", count), result)

    def test_secondary_faint_marks_survive_every_interior_placement(self):
        # The retained and synthetic meter interiors have these two sizes.
        # Full faint shapes must remain evidence when crossing quadrant edges.
        for shape in ((9, 22), (10, 22)):
            marks = [(3, 3, False), (3, 3, True)]
            marks += [(1, length, False) for length in range(5, shape[1] + 1)]
            marks += [(length, 1, False) for length in range(5, shape[0] + 1)]
            for height, width, attenuated in marks:
                for y in range(shape[0] - height + 1):
                    for x in range(shape[1] - width + 1):
                        interior = np.full(shape, 10.0)
                        interior[y:y + height, x:x + width] = 40
                        if attenuated:
                            interior[y:y + height, x + 1] = 30
                        self.assertTrue(reader._card_spatial_ink(interior),
                                        (shape, height, width, attenuated, x, y))

    def test_secondary_spatial_ink_cannot_disappear_when_pixels_brighten(self):
        # Every binary 3x3 pattern, including split marks at the quadrant edge.
        # Raising a pixel through the bright threshold must not fragment ink.
        for pattern in range(512):
            interior = np.full((10, 22), 10.0)
            for bit in range(9):
                if pattern & (1 << bit):
                    interior[4 + bit // 3, 8 + bit % 3] = 40
            before = reader._card_spatial_ink(interior)
            for bit in range(9):
                changed = interior.copy()
                y, x = 4 + bit // 3, 8 + bit % 3
                changed[y, x] = 50 if changed[y, x] == 40 else 40
                after = reader._card_spatial_ink(changed)
                self.assertFalse(before and not after, (pattern, bit))

    def test_secondary_meter_spatial_resolution_and_independent_guards(self):
        for kind in ("crossing3x3", "attenuated3x3", "diagonal5", "brightened_line7",
                     "bright_line3", "dark_line3", "two_bright2x2", "five_local_pixels",
                     "four_local_pixels", "two_weak2x2"):
            with self.subTest(kind=kind):
                im = display()
                card(im, 393, "side", 3)
                draw = ImageDraw.Draw(im)
                if kind == "crossing3x3":
                    draw.rectangle((515, 434, 517, 436), fill=(40, 40, 40))
                elif kind == "attenuated3x3":
                    draw.rectangle((515, 435, 517, 437), fill=(40, 40, 40))
                    draw.rectangle((516, 435, 516, 437), fill=(30, 30, 30))
                elif kind == "diagonal5":
                    draw.line((514, 431, 518, 435), fill=(40, 40, 40))
                elif kind == "brightened_line7":
                    draw.line((513, 435, 519, 435), fill=(40, 40, 40))
                    draw.point((516, 435), fill=(50, 50, 50))
                elif kind == "bright_line3":
                    draw.line((513, 435, 515, 435), fill=(50, 50, 50))
                elif kind == "dark_line3":
                    draw.line((419, 431, 419, 433), fill=(10, 10, 10))
                elif kind in ("two_bright2x2", "two_weak2x2"):
                    level = 50 if kind == "two_bright2x2" else 40
                    draw.rectangle((513, 435, 514, 436), fill=(level,) * 3)
                    draw.rectangle((518, 435, 519, 436), fill=(level,) * 3)
                else:
                    end = 437 if kind == "five_local_pixels" else 436
                    draw.line((515, 435, 515, end), fill=(40, 40, 40))
                    draw.line((517, 435, 517, 436), fill=(40, 40, 40))
                result = reader._card_bars(
                    reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 393)
                # These explicitly retained small/dispersed marks fall below
                # the declared midlevel spatial floor. Bright guards stay stricter.
                below_floor = kind in ("four_local_pixels", "two_weak2x2")
                self.assertEqual((result["state"], result["value"]),
                                 ("readable", 3) if below_floor else ("ambiguous", None), result)

    def test_secondary_partial_faint_and_noncontiguous_fills_refuse(self):
        for kind in ("partial_left", "partial_right", "faint", "noncontiguous"):
            with self.subTest(kind=kind):
                im = display()
                card(im, 640, "rear", 2)
                draw = ImageDraw.Draw(im)
                left = 640 + 16 + 3 * 33
                if kind == "partial_left":
                    draw.rectangle((left + 4, 430, left + 10, 435), fill=(220, 100, 10))
                elif kind == "partial_right":
                    draw.rectangle((left + 19, 434, left + 24, 439), fill=(220, 100, 10))
                elif kind == "faint":
                    draw.rectangle((left + 2, 428, left + 26, 441), fill=(25, 14, 10))
                else:
                    draw.rectangle((left, 426, left + 28, 443), fill=(90, 240, 30))
                result = reader._card_bars(reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 640)
                self.assertEqual(result["state"], "ambiguous", result)
                self.assertIsNone(result["value"])

    def test_secondary_uniform_muted_fill_and_unresolved_perimeter(self):
        im = display()
        card(im, 393, "side", 0)
        draw = ImageDraw.Draw(im)
        for i in range(4):
            draw.rectangle((409 + i * 33, 426, 437 + i * 33, 443), fill=(90, 90, 90))
        result = reader._card_bars(reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 393)
        self.assertEqual((result["state"], result["value"]), ("readable", 4), result)
        draw.rectangle((402, 419, 612, 449), fill=(90, 90, 90))
        result = reader._card_bars(reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 393)
        self.assertEqual(result["state"], "ambiguous", result)

    def test_secondary_whole_fills_allow_independently_configured_brightness(self):
        # Firmware fills each active cell with its own configurable color.  A
        # dim complete cell remains active; only spatially incomplete fill is
        # ambiguous.
        im = display()
        card(im, 393, "side", 0)
        draw = ImageDraw.Draw(im)
        for index, level in enumerate((240, 55, 230, 60)):
            draw.rectangle((409 + index * 33, 426, 437 + index * 33, 443),
                           fill=(level, level, level))
        result = reader._card_bars(
            reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 393)
        self.assertEqual((result["state"], result["value"]), ("readable", 4), result)

    def test_secondary_narrow_partial_stroke_in_next_bar_is_not_absence(self):
        for kind in ("vertical", "horizontal", "faint"):
            with self.subTest(kind=kind):
                im = display()
                card(im, 640, "rear", 2)
                draw = ImageDraw.Draw(im)
                if kind == "vertical":
                    draw.rectangle((726, 430, 726, 439), fill=(220, 100, 10))
                elif kind == "horizontal":
                    draw.rectangle((726, 434, 745, 434), fill=(220, 100, 10))
                else:
                    draw.rectangle((724, 428, 748, 441), fill=(18, 10, 10))
                result = reader._card_bars(reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 640)
                self.assertEqual(result["state"], "ambiguous", result)
                self.assertIsNone(result["value"])

    def test_secondary_compatibility_never_changes_an_ambiguous_literal(self):
        self.assertEqual(reader._compatible_bar_counts(
            ["on", "on", "on", "partial", "off", "off"]), [3, 4])
        self.assertEqual(reader._compatible_bar_counts(
            ["on", "on", "on", "off", "off", "partial"]), [3])
        self.assertEqual(reader._compatible_bar_counts(
            ["on", "on", "off", "on", "off", "off"]), [])
        im = display()
        card(im, 640, "rear", 2)
        ImageDraw.Draw(im).rectangle((726, 430, 726, 439), fill=(220, 100, 10))
        result = reader._card_bars(reader.Pixels(im.tobytes(), WIDTH, HEIGHT, REGISTRATION), 640)
        self.assertEqual((result["state"], result["value"]), ("ambiguous", None), result)
        self.assertIn("compatible_counts", result)

    def test_expected_values_cannot_be_supplied(self):
        with self.assertRaises(TypeError):
            reader.observe(display().tobytes(), WIDTH, HEIGHT, REGISTRATION, expected="68.902")
        for registration_value in (None, {}, {**REGISTRATION, "result": "FAIL"}):
            result = reader.observe(display().tobytes(), WIDTH, HEIGHT, registration_value)
            self.assertTrue(all(result[f]["state"] == "unreadable" for f in reader.FIELDS))


class FrequencyContrastControls(unittest.TestCase):
    def frequency(self, image):
        return reader._frequency(reader.Pixels(image.tobytes(), *image.size, REGISTRATION))

    def test_spatial_brightness_variation_with_one_saturated_stroke(self):
        image = display("68.902")
        pixels = np.array(image)
        region = pixels[250:363, 445:840].astype(float)
        gain = np.linspace(.92, 1.06, region.shape[1])[None, :, None]
        pixels[250:363, 445:840] = np.minimum(region * gain, 255).astype(np.uint8)
        image = Image.fromarray(pixels)
        ImageDraw.Draw(image).rectangle((769, 255, 828, 273), fill=(255, 116, 12))
        observed = self.frequency(image)
        self.assertEqual((observed["state"], observed["value"]), ("readable", "68.902"), observed)

    def test_complete_digits_with_different_brightness_are_readable(self):
        image = display("34.700")
        draw = ImageDraw.Draw(image)
        for origin, digit, level in zip((454, 520, 616, 688, 764), "34700",
                                        (100, 125, 150, 180, 220)):
            color = (level, round(level * .45), max(5, round(level * .05)))
            for segment in DIGITS[digit]:
                x1, y1, x2, y2 = STROKES[segment]
                draw.rectangle((origin + x1, y1, origin + x2, y2), fill=color)
        observed = self.frequency(image)
        self.assertEqual((observed["state"], observed["value"]), ("readable", "34.700"), observed)
        illuminated = [segment["median"] for digit in observed["cells"]
                       for segment in digit["segments"].values() if segment["state"] == "on"]
        self.assertLess(min(illuminated), float(np.median(illuminated)) * .85)

    def test_bright_lingering_stroke_among_dim_new_strokes_refuses(self):
        pixels = np.array(display("34.700"))
        pixels[250:363, 445:840] = (pixels[250:363, 445:840].astype(float) * .795).astype(np.uint8)
        image = Image.fromarray(pixels)
        ImageDraw.Draw(image).rectangle((693, 300, 748, 314), fill=ORANGE)
        observed = self.frequency(image)
        self.assertEqual((observed["state"], observed["value"]), ("ambiguous", None), observed)
        self.assertIn("inconsistent illuminated", observed["reason"])

    def test_dim_lingering_stroke_with_same_color_ratio_refuses(self):
        image = display("34.700")
        ImageDraw.Draw(image).rectangle((693, 300, 748, 314), fill=tuple(round(c * .795) for c in ORANGE))
        observed = self.frequency(image)
        self.assertEqual((observed["state"], observed["value"]), ("ambiguous", None), observed)

    def test_all_numeric_glyphs_in_each_cell(self):
        for position in range(5):
            for digit in "0123456789":
                digits = list("01234")
                digits[position] = digit
                frequency = "".join(digits[:2]) + "." + "".join(digits[2:])
                observed = self.frequency(display(frequency))
                self.assertEqual((observed["state"], observed["value"]), ("readable", frequency), observed)


class FrequencyEStrokeControls(unittest.TestCase):
    # These controls alter the independently drawn whole stroke from STROKES,
    # not the reader's measurement rectangles. They exercise longitudinal
    # damage; the rectangular fixture does not establish camera/font taper.
    ORIGINS = (454, 520, 616, 688, 764)

    def frequency(self, image):
        return reader._frequency(reader.Pixels(image.tobytes(), *image.size, REGISTRATION))

    def image_with_digit(self, position, digit):
        digits = list("11111")
        digits[position] = digit
        frequency = "".join(digits[:2]) + "." + "".join(digits[2:])
        return display(frequency), frequency

    def erase_e(self, image, position):
        x1, y1, x2, y2 = STROKES["e"]
        origin = self.ORIGINS[position]
        ImageDraw.Draw(image).rectangle((origin + x1, y1, origin + x2, y2), fill="black")

    def assert_refused(self, observed):
        summary = {key: observed[key] for key in ("state", "value", "reason")}
        self.assertIn(observed["state"], ("ambiguous", "unreadable"), summary)
        self.assertIsNone(observed["value"], summary)

    def test_whole_lower_left_removal_reads_changed_glyph_or_refuses(self):
        for position in range(5):
            for digit in "0268":
                with self.subTest(position=position, digit=digit):
                    image, _ = self.image_with_digit(position, digit)
                    self.erase_e(image, position)
                    observed = self.frequency(image)
                    # Complete loss of e turns 6 into 5 and 8 into 9. A
                    # reader must report those visible changes, not restore
                    # the digit originally painted by this fixture.
                    if digit in "68":
                        _, changed = self.image_with_digit(position, {"6": "5", "8": "9"}[digit])
                        self.assertEqual((observed["state"], observed["value"]),
                                         ("readable", changed), observed)
                    else:
                        self.assert_refused(observed)

    def test_shortened_cut_and_isolated_middle_strokes_refuse(self):
        for position in range(5):
            for digit in "0268":
                for damage in ("missing_top", "missing_bottom", "middle_cut", "middle_only",
                               "right_half_only"):
                    with self.subTest(position=position, digit=digit, damage=damage):
                        image, _ = self.image_with_digit(position, digit)
                        origin = self.ORIGINS[position]
                        x1, top, x2, bottom = STROKES["e"]
                        draw = ImageDraw.Draw(image)
                        if damage == "middle_only":
                            self.erase_e(image, position)
                            # Keep only the middle half of the whole shape.
                            # This still spans the old short interior probe.
                            margin = (bottom - top + 1) // 4
                            draw.rectangle((origin + x1, top + margin,
                                            origin + x2, bottom - margin), fill=ORANGE)
                        elif damage == "right_half_only":
                            self.erase_e(image, position)
                            # The remaining side of a damaged stroke must not
                            # become absence when its central witness is dark.
                            midpoint = (x1 + x2) // 2
                            draw.rectangle((origin + midpoint, top,
                                            origin + x2, bottom), fill=ORANGE)
                        else:
                            low, high = {"missing_top": (top, 323),
                                         "missing_bottom": (337, bottom),
                                         "middle_cut": (328, 332)}[damage]
                            draw.rectangle((origin + x1, low, origin + x2, high), fill="black")
                        self.assert_refused(self.frequency(image))

    def test_absent_lower_left_stroke_with_dim_fragment_refuses(self):
        for position in range(5):
            for digit in "134579":
                for side in ("whole_width", "right_half"):
                    with self.subTest(position=position, digit=digit, side=side):
                        image, _ = self.image_with_digit(position, digit)
                        origin = self.ORIGINS[position]
                        left = origin if side == "whole_width" else origin + 9
                        # A dim remnant above the off threshold must not be
                        # discarded just because the rest of this stroke is off.
                        ImageDraw.Draw(image).rectangle((left, 326, origin + 18, 335),
                                                        fill=(38, 17, 5))
                        self.assert_refused(self.frequency(image))

    def test_absent_lower_left_strokes_tolerate_dark_background(self):
        for position in range(5):
            for digit in "134579":
                with self.subTest(position=position, digit=digit):
                    image, frequency = self.image_with_digit(position, digit)
                    origin = self.ORIGINS[position]
                    x1, y1, x2, y2 = STROKES["e"]
                    ImageDraw.Draw(image).rectangle((origin + x1, y1, origin + x2, y2),
                                                    fill=(20, 9, 2))
                    observed = self.frequency(image)
                    self.assertEqual((observed["state"], observed["value"]),
                                     ("readable", frequency), observed)

    def test_frequency_shift_without_registration_change_refuses(self):
        for dx, dy in ((12, 0), (-12, 0), (0, 12)):
            with self.subTest(dx=dx, dy=dy):
                image = display("68.902")
                shifted = image.transform(image.size, Image.Transform.AFFINE,
                                          (1, 0, dx, 0, 1, dy))
                self.assert_refused(self.frequency(shifted))


class MainArrowReaderTests(unittest.TestCase):
    def read(self, im):
        return reader._arrows(reader.Pixels(im.tobytes(), *im.size, REGISTRATION))

    def test_all_direction_combinations_and_neutral_filled_arrows(self):
        for mask in range(8):
            for color in (ORANGE, (140, 140, 140)):
                im = display()
                wanted = []
                for n, direction in enumerate(('front', 'side', 'rear')):
                    if mask & (1 << n):
                        arrow(im, direction, color)
                        wanted.append(direction)
                observed = self.read(im)
                self.assertEqual((observed['state'], observed['value']), ('readable', wanted), observed)
                self.assertEqual(observed['visible_directions'], wanted)
                self.assertEqual(set(observed['direction_states']), {'front', 'side', 'rear'})

    def test_changed_direction_follows_image_without_expectation(self):
        for direction in ('front', 'side', 'rear'):
            im = display()
            arrow(im, direction)
            self.assertEqual(self.read(im)['value'], [direction])

    def test_arrow_profile_is_fixed_expected_blind_and_does_not_change_literal_state(self):
        dark = self.read(display())
        lit_image = display()
        arrow(lit_image, "front")
        lit = self.read(lit_image)
        for observed in (dark, lit):
            for direction in ("front", "side", "rear"):
                profile = observed["direction_states"][direction]["profile"]
                self.assertEqual((profile["rows"], profile["columns"]), (4, 4))
                self.assertEqual(len(profile["max_channel_medians"]), 16)
                self.assertEqual(len(profile["reference_bounds"]), 4)
        self.assertEqual((dark["state"], dark["value"]), ("readable", []))
        self.assertEqual((lit["state"], lit["value"]), ("readable", ["front"]))
        self.assertNotEqual(dark["direction_states"]["front"]["profile"]["max_channel_medians"],
                            lit["direction_states"]["front"]["profile"]["max_channel_medians"])

    def test_wrong_color_is_measured_without_a_color_correctness_claim(self):
        for color, name in (((20, 180, 20), 'green'), ((10, 20, 180), 'blue'),
                            ((180, 30, 10), 'warm'), ((150, 150, 150), 'neutral')):
            im = display()
            arrow(im, 'front', color)
            observed = self.read(im)
            self.assertEqual(observed['value'], ['front'])
            self.assertEqual(observed['direction_states']['front']['color'], name)
            self.assertEqual(observed['direction_states']['front']['rgb_median'], list(color))
            self.assertIn('no color correctness contract', observed['color_qualification'])

    def test_faint_fill_never_becomes_absence(self):
        for direction in ('front', 'side', 'rear'):
            for color in ((25, 10, 10), (10, 25, 10), (10, 10, 25)):
                im = display()
                arrow(im, direction, color)
                observed = self.read(im)
                self.assertEqual((observed['state'], observed['value']), ('ambiguous', None), observed)
                self.assertEqual(observed['direction_states'][direction]['state'], 'faint')

    def test_faint_side_does_not_hide_filled_front_or_unlit_rear(self):
        im = display()
        arrow(im, 'front')
        arrow(im, 'side', (25, 10, 10))
        observed = self.read(im)
        self.assertEqual(observed['state'], 'ambiguous')
        self.assertEqual(observed['visible_directions'], ['front'])
        self.assertEqual({k: v['state'] for k, v in observed['direction_states'].items()},
                         {'front': 'filled', 'side': 'faint', 'rear': 'unlit'})

    def test_partial_shape_outside_old_probes_refuses(self):
        im = display()
        arrow(im, 'front')
        # Thin missing fill between the original probes; must remain visible.
        ImageDraw.Draw(im).rectangle((1072, 250, 1072, 267), fill='black')
        observed = self.read(im)
        self.assertEqual((observed['state'], observed['value']), ('ambiguous', None), observed)
        self.assertEqual(observed['direction_states']['front']['state'], 'partial')

    def test_narrow_bright_partial_shape_is_not_absence(self):
        im = display()
        ImageDraw.Draw(im).rectangle((1072, 250, 1072, 267), fill=ORANGE)
        observed = self.read(im)
        self.assertEqual((observed['state'], observed['value']), ('ambiguous', None), observed)

    def test_old_probe_rectangles_cannot_masquerade_as_whole_arrow(self):
        im = display()
        draw = ImageDraw.Draw(im)
        for box in ((1064, 225, 1084, 247), (1035, 274, 1055, 284), (1100, 274, 1118, 284)):
            draw.rectangle(box, fill=ORANGE)
        observed = self.read(im)
        self.assertEqual((observed['state'], observed['value']), ('ambiguous', None), observed)

    def test_dim_resting_and_outlined_shapes_are_not_active(self):
        for outlined in (False, True):
            im = display()
            if outlined:
                ImageDraw.Draw(im).polygon(((987, 289), (1077, 187), (1165, 289),
                    (1123, 289), (1123, 301), (1034, 301), (1034, 289)), outline=(120, 120, 120), width=2)
            else:
                for direction in ('front', 'side', 'rear'):
                    arrow(im, direction, (17, 17, 17))
            observed = self.read(im)
            self.assertEqual((observed['state'], observed['value']), ('readable', []), observed)

    def test_sequence_has_no_smoothing_or_carried_direction(self):
        sequence = []
        for color in (ORANGE, (25, 10, 10), None, ORANGE):
            im = display()
            if color is not None:
                arrow(im, 'front', color)
            observed = self.read(im)
            sequence.append((observed['state'], observed['value']))
        self.assertEqual(sequence, [('readable', ['front']), ('ambiguous', None),
                                    ('readable', []), ('readable', ['front'])])


if __name__ == "__main__":
    unittest.main()
