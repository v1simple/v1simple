#!/usr/bin/env python3
"""Synthetic control images for the offline encounter reader.

These are image-reader checks, not camera, panel, or firmware evidence.
"""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image, ImageDraw

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
