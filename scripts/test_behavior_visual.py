"""Independent synthetic controls for the bounded behavior-camera verdicts.

These do not qualify optics or physical delivery. Retained originals and the
live runner provide those separate observations; no test embeds private pixels.
"""
from copy import deepcopy
import colorsys
import unittest

import numpy as np
from PIL import Image, ImageFilter

from bench import behavior_visual as visual
from bench.read_replay_fields import READER_REFERENCE_CROP


IDENTITY = {"result": "PASS", "transform": {"kind": "dynamic_similarity",
             "crop_fractions": list(READER_REFERENCE_CROP)}}
COLORS = {"red": (240, 65, 15), "yellow": (240, 240, 10), "green": (20, 235, 20),
          "cyan": (15, 235, 235), "blue": (10, 35, 240), "magenta": (235, 15, 235)}
BARS = ["red", "green", "blue", "cyan", "magenta", "yellow"]


def fixture(*, digits="24150", frequency="magenta", bars=True, card=False, idle=False, muted=False,
            card_slot=0):
    """Procedural geometry authored independently of the observer's rectangles."""
    image = np.zeros((720, 1280, 3), dtype=np.uint8)
    definitions = {
        "0": "ab cdef".replace(" ", ""), "1": "bc", "2": "abdeg", "3": "abcdg",
        "4": "bcfg", "5": "acdfg", "6": "acdefg", "7": "abc", "8": "abcdefg", "9": "abcdfg",
    }
    # Conventional clockwise segment labels, with g in the middle.
    rectangles = {"a": (0, 8, 0, 40), "b": (8, 32, 32, 40), "c": (40, 64, 32, 40),
                  "d": (64, 72, 0, 40), "e": (40, 64, 0, 8), "f": (8, 32, 0, 8),
                  "g": (32, 40, 0, 40)}
    color = (100, 100, 100) if muted else COLORS[frequency]
    if idle:
        for left in (465, 520, 600, 660, 715):
            image[309:319, left:left + 40] = (65, 65, 65)
        image[350:358, 580:588] = (65, 65, 65)
    else:
        for character, left in zip(digits, (465, 520, 600, 660, 715)):
            for segment in definitions[character]:
                y0, y1, x0, x1 = rectangles[segment]
                image[280 + y0:280 + y1, left + x0:left + x1] = color
        image[344:354, 580:590] = color
        image[220:280, 1030:1150] = color
    # Independent always-present witnesses: upper counter and lower status.
    image[200:270, 275:289] = (230, 80, 20)
    image[427:445, 1040:1130] = (20, 80, 230)
    if bars and not idle:
        for index, name in enumerate(BARS):
            image[405 - index * 38:427 - index * 38, 900:970] = COLORS[name]
    if card:
        shift = card_slot * 248
        image[386:410, 460 + shift:610 + shift] = (90, 90, 90)
        for index, name in enumerate(BARS):
            image[430:450, 422 + shift + index * 33:453 + shift + index * 33] = COLORS[name]
    return image


def observation(image):
    return visual.observe_frame(image, IDENTITY)


def samples_of(images):
    return [{"time_s": index / 8, **observation(image)} for index, image in enumerate(images)]


def timed_series(*, persistence=3., stuck=False, early=False, blackout=False, card_only=False):
    idle = observation(fixture(idle=True) if not card_only else fixture(digits="34700", card=False))
    active = observation(fixture(digits="34700" if card_only else "24150", card=card_only))
    held = observation(fixture(digits="34700" if card_only else "24150", card=card_only,
                               muted=not card_only, bars=card_only))
    dark = observation(np.zeros((720, 1280, 3), dtype=np.uint8))
    rows = []
    for tick in range(-8, int((persistence + 1.5) * 8) + 1):
        when = tick / 8
        if when < 0:
            value = active
        elif when < persistence and not early:
            value = held
        else:
            value = dark if blackout else active if stuck else idle
        rows.append({"time_s": when, **deepcopy(value)})
    return rows, [{"time_s": -2 + index / 8, **deepcopy(idle)} for index in range(3)]


def clearance(rows, reference, **kwargs):
    return visual.evaluate_clearance(rows, reference, active_window=(-.875, -.375), clear_time_s=0.,
                                     persistence_s=kwargs.pop("persistence_s", 3.), **kwargs)


def takeover_series(*, switch_at=1., replacement="34700"):
    retained = observation(fixture(digits="10525", muted=True))
    live = observation(fixture(digits=replacement))
    return [{"time_s": tick / 8, **deepcopy(retained if tick / 8 < switch_at else live)}
            for tick in range(21)]


def takeover(rows, **kwargs):
    return visual.evaluate_takeover(rows, release_time_s=0., new_alert_time_s=1.,
                                    persistence_s=kwargs.pop("persistence_s", 3.), **kwargs)


def warning_fixture(text="VOL 0", *, color=(235, 45, 15), x=488, y=248, scale=9.75, spread=0):
    image = fixture(idle=True)
    image[230:375, 450:880] = 0
    rows = {
        "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100", "00000"),
        "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110", "00000"),
        "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111", "00000"),
        "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110", "00000"),
        "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110", "00000"),
        " ": ("00000",) * 8,
    }
    for index, char in enumerate(text):
        for row, bits in enumerate(rows[char]):
            for column, bit in enumerate(bits):
                if bit == "1":
                    image[round(y + row * scale):round(y + (row+1) * scale),
                          round(x + (index*6+column)*scale):round(x + (index*6+column+1)*scale)] = color
    if spread:
        image[230:375, 450:880] = np.asarray(Image.fromarray(image[230:375, 450:880]).filter(
            ImageFilter.GaussianBlur(spread)))
    return image


def warning_live_control(clear_s=15):
    image = fixture(digits="24150")
    image[330:380, 340:355] = (235, 45, 15)
    observed = visual.observe_warning_frame(image, IDENTITY)
    return [{"time_s": clear_s - 1.1 + index / 10, **deepcopy(observed)} for index in range(10)]


def warning_native_frames(images, *, start=0., step=.005):
    return [{"time_s": start + index * step, "frame_seq": index, "rgb": image.copy()}
            for index, image in enumerate(images)]


def warning_native_presence(*, half=.250, delay=0., frozen=False):
    """Independent 200 fps source timeline, not interpolation of text samples."""
    rows = []
    for index in range(1861):
        elapsed_ns = 350_000_000 + index * 5_000_000
        host = 15_000_000_000 + elapsed_ns
        phase_ns = elapsed_ns - round(delay * 1e9)
        visible = not frozen and (phase_ns // round(half * 1e9)) % 2 == 1
        source = {"host_capture_ns": host, "frame_seq": index,
                  "video_pts_value": index, "video_pts_timescale": 200}
        rows.append({"host_monotonic_ns": host, "video_pts_seconds": index / 200,
                     "frame_seq": index, "center_lit_pixels": 8000 if visible else 0,
                     "source_frame": source})
    return rows


class WarningVisualTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.on = visual.observe_warning_frame(warning_fixture(), IDENTITY)
        cls.off = visual.observe_warning_frame(warning_fixture("     "), IDENTITY)
        cls.idle = visual.observe_warning_frame(fixture(idle=True), IDENTITY)

    def series(self, *, frozen=False, stuck=False, early=False, fast=False):
        rows = []
        for tick in range(-8, 126):
            elapsed = tick / 10
            if elapsed < (-1 if early else 0) or (elapsed >= 10 and not stuck):
                value = self.idle
            elif frozen:
                value = self.off
            else:
                value = self.on if (round(elapsed * 1000) // (100 if fast else 250)) % 2 else self.off
            rows.append({"time_s": 15 + elapsed, **deepcopy(value)})
        reference = [{"time_s": 2 + index / 10, **deepcopy(self.idle)} for index in range(5)]
        self.native = warning_native_presence(half=.100 if fast else .250, frozen=frozen)
        return rows, reference

    def evaluate(self, rows, reference, *, clear=15, live=None):
        return visual.evaluate_warning(rows, reference, onset_s=15, clear_idle_s=clear,
                                       live_control=warning_live_control(clear) if live is None else live,
                                       native_presence=self.native)

    def test_full_glyph_identity_and_blank_are_distinct(self):
        self.assertEqual(self.on["warning"]["text"], "VOL 0", self.on["warning"])
        self.assertFalse(self.on["warning"]["blank"])
        self.assertTrue(self.off["warning"]["blank"])
        self.assertIsNone(self.off["warning"]["text"])
        self.assertFalse(self.idle["warning"]["blank"])

    def test_wrong_glyph_missing_letter_blob_and_wrong_color_cannot_be_warning(self):
        blob = warning_fixture("     ")
        blob[248:318, 488:780] = (235, 45, 15)
        for image in (warning_fixture("VOL O"), warning_fixture("VOL 8"), warning_fixture(" OL 0"),
                      warning_fixture(color=(15, 45, 235)), blob):
            with self.subTest(image=image[250, 490].tolist()):
                self.assertIsNone(visual.observe_warning_frame(image, IDENTITY)["warning"]["text"])

    def test_bright_stroke_spread_keeps_complete_identity_and_rejects_extra_content(self):
        image = warning_fixture(spread=2.5)
        observed = visual.observe_warning_frame(image, IDENTITY)
        self.assertEqual(observed["warning"]["text"], "VOL 0", observed["warning"])
        self.assertEqual(observed["warning"]["cell_errors"], 0)
        for text in ("VOL O", "VOL 8", " OL 0"):
            with self.subTest(text=text):
                self.assertIsNone(visual.observe_warning_frame(warning_fixture(text, spread=2.5),
                                                              IDENTITY)["warning"]["text"])
        for source in (warning_fixture(), image):
            for bounds in ((475, 254, 482, 310), (662, 250, 669, 315), (810, 330, 865, 370),
                           (830, 330, 855, 340), (774, 314, 799, 324)):
                with self.subTest(bounds=bounds):
                    changed = source.copy()
                    left, top, right, bottom = bounds
                    changed[top:bottom, left:right] = (235, 45, 15)
                    self.assertIsNone(visual.observe_warning_frame(changed, IDENTITY)["warning"]["text"])

    def test_unreadable_neighbor_cannot_prove_a_short_visible_half_cycle(self):
        rows, reference = self.series()
        for row in rows:
            if abs(row["time_s"] - 15.8) < .001:
                row["warning"] = {"text": None, "blank": False, "lit_pixels": 9000}
        checks = self.evaluate(rows, reference)
        blink = next(row for row in checks if row["check"] == "warning_blink")
        self.assertEqual(blink["result"], "INCOMPLETE", checks)
        self.assertEqual(blink["failures"], [])

    def test_native_transition_has_opposite_complete_endpoints_and_preserves_unknown(self):
        on, off = warning_fixture(), warning_fixture("     ")
        dim = warning_fixture(color=(40, 8, 3))
        _, reference = self.series()
        for images in ([on, dim, off], [off, dim, on],
                       [on, warning_fixture(" OL 0", color=(40, 8, 3)), off]):
            with self.subTest(direction=images[0][260, 500].tolist()):
                frames = warning_native_frames(images)
                proof = visual.evaluate_warning_transition(frames, IDENTITY, reference, .005)
                self.assertEqual(proof["result"], "PASS", proof)
                self.assertEqual(proof["kind"], "bounded_transition")
                self.assertEqual((proof["start_s"], proof["end_s"]), (0., .01))
                self.assertIsNone(visual.observe_warning_frame(images[1], IDENTITY)["warning"]["text"])

    def test_transition_rejects_added_content_reversal_wrong_endpoints_and_missing_coverage(self):
        on, off = warning_fixture(), warning_fixture("     ")
        dim = warning_fixture(color=(40, 8, 3))
        _, reference = self.series()
        extra = dim.copy()
        extra[330:340, 830:855] = (40, 8, 3)
        occluded = dim.copy()
        occluded[188:278, 240:312] = 0
        offset = [on.copy(), warning_fixture("VOL 8", color=(40, 8, 3)), off.copy()]
        reappearing = [on.copy(), warning_fixture(" OL 0", color=(40, 8, 3)), dim.copy(), off.copy()]
        for image in [*offset, *reappearing]:
            center = image[230:375, 450:880]
            center[center.max(axis=2) == 0] = (8, 0, 0)
        cases = [([on, warning_fixture("VOL 8", color=(40, 8, 3)), off], .005),
                 (offset, .005), (reappearing, .005), ([on, extra, off], .005), ([on, occluded, off], .005),
                 ([on, warning_fixture(color=(3, 8, 40)), off], .005),
                 ([on, dim, on], .005), ([off, dim, off], .005),
                 ([warning_fixture("VOL O"), dim, off], .005),
                 ([on, warning_fixture(" OL 0", color=(40, 8, 3)),
                   warning_fixture(" OL 0", color=(80, 8, 3)), off], .005),
                 ([on] + [dim] * 11 + [off], .005),
                 ([warning_fixture("VOL O"), on, dim, off], .01),
                 ([on, dim, off, warning_fixture("VOL O")], .005)]
        for images, target in cases:
            with self.subTest(target=target, count=len(images)):
                proof = visual.evaluate_warning_transition(warning_native_frames(images), IDENTITY, reference, target)
                self.assertEqual(proof["result"], "INCOMPLETE", proof)
        for change in (lambda rows: rows[2].update(frame_seq=3),
                       lambda rows: rows[2].update(time_s=.005),
                       lambda rows: rows[2].update(time_s=.016)):
            frames = warning_native_frames([on, dim, off])
            change(frames)
            self.assertEqual(visual.evaluate_warning_transition(frames, IDENTITY, reference, .005)["result"],
                             "INCOMPLETE")

    def test_observed_background_fringe_keeps_amplitude_hue_and_foreground_guards(self):
        on = warning_fixture()
        dim = warning_fixture(color=(40, 8, 3))
        off = warning_fixture("     ")
        _, reference = self.series()
        x, y, scale = self.on["warning"]["fitted_geometry"]
        cx, cy = round(x + 27.5 * scale), round(y + 3.5 * scale)

        def marked(image, color):
            result = image.copy()
            result[cy-1:cy+2, cx-1:cx+2] = color
            return result

        cases = [
            ("observed fringe", [marked(on, (40, 8, 3)), marked(on, (20, 4, 2)),
                                  marked(dim, (40, 8, 3)), off], .01, "PASS"),
            ("unwitnessed addition", [on, marked(on, (20, 4, 2)),
                                       marked(dim, (40, 8, 3)), off], .01, "INCOMPLETE"),
            ("coherent amplitude reversal", [marked(on, (40, 8, 3)), marked(on, (20, 4, 2)),
                                              marked(dim, (80, 8, 3)), off], .01, "INCOMPLETE"),
            ("wrong source hue", [marked(on, (40, 100, 3)), marked(on, (20, 4, 2)),
                                   marked(dim, (40, 8, 3)), off], .01, "INCOMPLETE"),
            ("coherent new hue", [marked(on, (40, 8, 3)), marked(on, (20, 4, 2)),
                                   marked(dim, (10, 10, 230)), off], .01, "INCOMPLETE"),
            ("foreground reappearance", [on, warning_fixture(" OL 0", color=(40, 8, 3)),
                                          dim, off], .005, "INCOMPLETE"),
        ]
        for name, images, target, expected in cases:
            with self.subTest(case=name):
                proof = visual.evaluate_warning_transition(warning_native_frames(images), IDENTITY,
                                                           reference, target)
                self.assertEqual(proof["result"], expected, proof)

    def test_native_edge_intervals_keep_raw_unknown_and_cannot_hide_bad_cadence(self):
        on, off = warning_fixture(), warning_fixture("     ")
        dim = warning_fixture(color=(40, 8, 3))
        rows, reference = self.series()
        target = 15.5
        next(row for row in rows if row["time_s"] == target).update(
            visual.observe_warning_frame(dim, IDENTITY))
        proof = visual.evaluate_warning_transition(
            warning_native_frames([on, dim, off], start=target - .005), IDENTITY, reference, target)
        self.assertEqual(proof["result"], "PASS", proof)
        # The original unknown is visibly fading until the native blank endpoint.
        next(row for row in self.native if row["host_monotonic_ns"] == 15_500_000_000)["center_lit_pixels"] = 800
        checks = visual.evaluate_warning(rows, reference, onset_s=15, clear_idle_s=15,
                                          live_control=warning_live_control(), transitions={target: proof},
                                          native_presence=self.native)
        blink = next(row for row in checks if row["check"] == "warning_blink")
        self.assertEqual(blink["result"], "PASS", checks)
        self.assertEqual(next(row for row in rows if row["time_s"] == target)["warning_state"], "unreadable")
        edge = next(row for row in blink["edge_intervals"] if row["low_s"] == target)
        self.assertAlmostEqual(edge["high_s"], proof["end_s"])

    def test_all_native_cadence_rejects_fast_alias_and_nonquarter_second_periods(self):
        for half in (.0625, .225, .250, .300):
            with self.subTest(half=half):
                rows, reference = self.series()
                for row in rows:
                    elapsed = row["time_s"] - 15
                    if 0 <= elapsed < 10:
                        visible = (round(elapsed * 1e9) // round(half * 1e9)) % 2
                        row.update(deepcopy(self.on if visible else self.off))
                self.native = warning_native_presence(half=half)
                checks = self.evaluate(rows, reference)
                blink = next(row for row in checks if row["check"] == "warning_blink")
                self.assertEqual(blink["result"], "PASS" if half == .250 else "FAIL", checks)

    def test_native_cadence_accepts_bounded_lcd_asymmetry_but_not_hidden_phases(self):
        rows, reference = self.series()
        for source in self.native:
            elapsed_ns = source["host_monotonic_ns"] - 15_000_000_000
            phase_ns = elapsed_ns % 500_000_000
            source["center_lit_pixels"] = 8000 if phase_ns >= 260_000_000 or phase_ns < 40_000_000 else 0
        by_time = {row["host_monotonic_ns"]: row for row in self.native}
        for row in rows:
            source = by_time.get(round(row["time_s"] * 1e9))
            if source:
                row.update(deepcopy(self.on if source["center_lit_pixels"] else self.off))
        blink = next(row for row in self.evaluate(rows, reference) if row["check"] == "warning_blink")
        self.assertEqual(blink["result"], "PASS", blink)
        for interval in blink["mean_intervals"]:
            self.assertLessEqual(interval["low_s"], .25)
            self.assertGreaterEqual(interval["high_s"], .25)
        for start, end in ((15_605_000_000, 15_615_000_000), (15_745_000_000, 16_005_000_000)):
            rows, reference = self.series()
            for source in self.native:
                if start <= source["host_monotonic_ns"] < end:
                    source["center_lit_pixels"] = 8000 - source["center_lit_pixels"]
            blink = next(row for row in self.evaluate(rows, reference) if row["check"] == "warning_blink")
            self.assertNotEqual(blink["result"], "PASS", blink)

    def test_native_identity_endpoint_coverage_and_glyph_witnesses_are_required(self):
        for change in (lambda rows: rows.pop(100),
                       lambda rows: rows[100].update(frame_seq=99),
                       lambda rows: rows[100].update(host_monotonic_ns=rows[99]["host_monotonic_ns"]),
                       lambda rows: rows[100].update(video_pts_seconds=999.),
                       lambda rows: rows.pop(0),
                       lambda rows: rows.__delitem__(slice(-5, None)),
                       lambda rows: rows.clear()):
            rows, reference = self.series()
            change(self.native)
            if len(self.native) == 1860 and self.native[0]["frame_seq"] == 1:
                # One absent edge frame is within the explicit10ms endpoint
                # tolerance; five absent frames are not.
                del self.native[:4]
            blink = next(row for row in self.evaluate(rows, reference) if row["check"] == "warning_blink")
            self.assertEqual(blink["result"], "INCOMPLETE", blink)
        rows, reference = self.series()
        self.native[50]["center_lit_pixels"] = 8000
        blink = next(row for row in self.evaluate(rows, reference) if row["check"] == "warning_blink")
        self.assertNotEqual(blink["result"], "PASS", blink)

    def test_default_delay_flash_and_expiry_pass(self):
        rows, reference = self.series()
        checks = self.evaluate(rows, reference)
        self.assertEqual([row["result"] for row in checks], ["PASS"] * 4, checks)

    def test_live_to_warning_transition_uses_the_existing_optical_allowance(self):
        for delay in (.25, .45):
            with self.subTest(delay=delay):
                rows, reference = self.series()
                for row in rows:
                    elapsed = row["time_s"] - 15
                    if 0 <= elapsed < delay:
                        value = warning_live_control()[0]
                    elif delay <= elapsed < 10 + delay:
                        value = self.on if round((elapsed - delay) * 1000) // 250 % 2 else self.off
                    else:
                        value = self.idle
                    row.update({key: deepcopy(value) for key, value in value.items() if key != "time_s"})
                self.native = warning_native_presence(delay=delay)
                checks = self.evaluate(rows, reference)
                if delay == .25:
                    self.assertEqual([row["result"] for row in checks], ["PASS"] * 4, checks)
                else:
                    self.assertEqual(checks[0]["result"], "FAIL", checks)
                    self.assertIn("an alert field remained during connected silence", checks[0]["failures"])

    def test_original_first_off_freeze_fails_flash_and_expiry(self):
        rows, reference = self.series(frozen=True, stuck=True)
        checks = {row["check"]: row for row in self.evaluate(rows, reference)}
        self.assertEqual(checks["warning_blink"]["result"], "FAIL")
        self.assertEqual(checks["warning_expiry"]["result"], "FAIL")

    def test_wrong_deadline_fast_flash_stuck_warning_and_camera_gap_cannot_pass(self):
        for changes in ({"early": True}, {"fast": True}, {"stuck": True}):
            rows, reference = self.series(**changes)
            checks = self.evaluate(rows, reference, clear=13 if changes.get("early") else 15)
            self.assertTrue(any(row["result"] != "PASS" for row in checks), checks)
        rows, reference = self.series()
        del rows[30:35]
        self.assertTrue(any(row["result"] == "INCOMPLETE"
                            for row in self.evaluate(rows, reference)))

    def test_dark_controls_or_missing_reference_cannot_pass(self):
        rows, reference = self.series()
        for row in rows:
            row["guards"] = {"counter": 0, "status": 0}
        self.assertTrue(any(row["result"] != "PASS"
                            for row in self.evaluate(rows, reference)))
        self.assertTrue(all(row["result"] == "INCOMPLETE"
                            for row in self.evaluate(rows, [])))

    def test_accumulated_slow_or_fast_cadence_missing_early_frames_and_live_control_are_rejected(self):
        for half in (.225, .300):
            rows, reference = self.series()
            for row in rows:
                elapsed = row["time_s"] - 15
                if 0 <= elapsed < 10:
                    value = self.on if int(round(elapsed * 1000) / round(half * 1000)) % 2 else self.off
                    row.update(deepcopy(value))
            self.native = warning_native_presence(half=half)
            self.assertTrue(any(row["result"] == "FAIL" for row in self.evaluate(rows, reference)))
        rows, reference = self.series()
        self.assertTrue(any(row["result"] != "PASS" for row in self.evaluate(rows[12:], reference)))
        self.assertTrue(any(row["result"] != "PASS" for row in self.evaluate(rows, reference, live=[])))

    def test_extra_center_content_and_partially_occluded_controls_are_not_accepted(self):
        image = warning_fixture()
        image[330:370, 810:865] = (235, 45, 15)
        self.assertIsNone(visual.observe_warning_frame(image, IDENTITY)["warning"]["text"])
        rows, reference = self.series()
        for row in rows:
            if 15 <= row["time_s"] < 25:
                row["warning_guard_grid"] = [value * .15 for value in row["warning_guard_grid"]]
        self.assertTrue(any(row["result"] != "PASS" for row in self.evaluate(rows, reference)))


class BehaviorVisualTests(unittest.TestCase):
    def test_observer_decodes_numeric_identity_and_individual_sentinel_cells(self):
        observed = observation(fixture(card=True))
        self.assertEqual("24.150", observed["primary_frequency"])
        self.assertEqual("magenta", observed["frequency_color"]["color"])
        self.assertEqual(BARS, [cell["color"] for cell in observed["main_bars"]])
        self.assertEqual(BARS, [cell["color"] for cell in observed["card_bars"][0]])
        self.assertEqual([None] * 6, [cell["color"] for cell in observed["card_bars"][1]])

    def test_dim_numeric_identity_is_read_without_expected_text(self):
        self.assertEqual("24.150", observation(fixture(muted=True))["primary_frequency"])
        self.assertIsNone(observation(fixture(idle=True))["primary_frequency"])

    def test_palette_checks_all_requested_frequencies_and_both_meter_consumers(self):
        samples = samples_of([fixture(digits=text, card=True)
                              for text in ("10525", "24150", "33800") for _ in range(3)])
        report = visual.evaluate_palette(samples, expected_frequency_colors={
            "10.525": "magenta", "24.150": "magenta", "33.800": "magenta"}, expected_bars=BARS)
        self.assertEqual("PASS", report["result"], report)

    def test_right_secondary_card_has_independent_six_cell_coverage(self):
        image = fixture(card=True, card_slot=1)
        observed = observation(image)
        self.assertEqual([None] * 6, [cell["color"] for cell in observed["card_bars"][0]])
        self.assertEqual(BARS, [cell["color"] for cell in observed["card_bars"][1]])
        contract = {"expected_frequency_colors": {"24.150": "magenta"}, "expected_bars": BARS}
        self.assertEqual("PASS", visual.evaluate_palette(samples_of([image] * 3), **contract)["result"])
        image[430:450, 670 + 5 * 33:701 + 5 * 33] = COLORS["red"]
        self.assertEqual("FAIL", visual.evaluate_palette(samples_of([image] * 3), **contract)["result"])

    def test_wrong_custom_color_is_a_failure(self):
        samples = samples_of([fixture(frequency="cyan", card=True) for _ in range(3)])
        report = visual.evaluate_palette(samples, expected_frequency_colors={"24.150": "magenta"}, expected_bars=BARS)
        self.assertEqual("FAIL", report["result"])
        neutral = samples_of([fixture(muted=True, card=True) for _ in range(3)])
        report = visual.evaluate_palette(neutral, expected_frequency_colors={"24.150": "magenta"}, expected_bars=BARS)
        self.assertEqual("FAIL", report["result"])

    def test_camera_like_blue_and_pale_magenta_remain_distinct_from_cyan_and_white(self):
        # Independently specified optical samples; one rule applies to every
        # position and frequency. These are colors, not requested settings.
        observed_colors = {"red": (238, 124, 1), "yellow": (233, 250, 48),
                           "green": (110, 255, 6), "cyan": (116, 252, 241),
                           "blue": (9, 146, 248), "magenta": (234, 166, 236)}
        image = fixture(card=True)
        for name, color in COLORS.items():
            image[np.all(image == color, axis=2)] = observed_colors[name]
        observed = observation(image)
        self.assertEqual("magenta", observed["frequency_color"]["color"])
        self.assertEqual(BARS, [cell["color"] for cell in observed["main_bars"]])
        self.assertEqual(BARS, [cell["color"] for cell in observed["card_bars"][0]])
        contract = {"expected_frequency_colors": {"24.150": "magenta"}, "expected_bars": BARS}
        self.assertEqual("PASS", visual.evaluate_palette(samples_of([image] * 3), **contract)["result"])
        for wrong in ((116, 252, 241), (235, 235, 235)):
            changed = image.copy()
            changed[329:351, 900:970] = wrong
            self.assertEqual("FAIL", visual.evaluate_palette(samples_of([changed] * 3), **contract)["result"])
        changed = image.copy()
        changed[253:275, 900:970] = (235, 235, 235)
        self.assertEqual("FAIL", visual.evaluate_palette(samples_of([changed] * 3), **contract)["result"])
        changed = image.copy()
        changed[291:313, 900:970] = observed_colors["blue"]
        self.assertEqual("FAIL", visual.evaluate_palette(samples_of([changed] * 3), **contract)["result"])

    def test_ambiguous_hue_and_weak_chroma_cannot_establish_palette_pass(self):
        for ambiguous in (tuple(round(channel * 255) for channel in colorsys.hsv_to_rgb(195/360, .9, .9)),
                          (230, 197, 230)):
            image = fixture(card=True)
            image[329:351, 900:970] = ambiguous
            report = visual.evaluate_palette(samples_of([image] * 3),
                                             expected_frequency_colors={"24.150": "magenta"}, expected_bars=BARS)
            self.assertEqual("INCOMPLETE", report["result"], report)

    def test_colored_minority_cannot_authorize_an_incomplete_meter_cell(self):
        for majority in ((235, 235, 235), (100, 100, 100), (0, 0, 0)):
            for card in (False, True):
                image = fixture(card=True)
                if card:
                    image[433:446, 435:449] = majority
                else:
                    image[407:419, 928:963] = majority
                observed = observation(image)
                cell = observed["card_bars"][0][0] if card else observed["main_bars"][0]
                self.assertIsNone(cell["color"], (majority, card))
                report = visual.evaluate_palette(samples_of([image] * 3),
                                                 expected_frequency_colors={"24.150": "magenta"}, expected_bars=BARS)
                self.assertEqual("INCOMPLETE", report["result"], report)

    def test_numeric_identity_requires_agreeing_thresholds_and_rejects_conflicts(self):
        image = fixture(digits="10525")
        # A dim middle-segment halo makes the lowest threshold unreadable.
        image[312:320, 465:505] = (130, 130, 130)
        self.assertEqual("10.525", observation(image)["primary_frequency"])
        # A dim top stroke produces a conflicting readable seven instead.
        image = fixture(digits="10525")
        image[280:288, 465:505] = (130, 130, 130)
        self.assertIsNone(observation(image)["primary_frequency"])

    def test_swapped_main_or_secondary_segments_fail(self):
        for rect in ((900, 405, 970, 427), (422, 430, 453, 450)):
            image = fixture(card=True)
            x0, y0, x1, y1 = rect
            image[y0:y1, x0:x1] = COLORS["yellow"]
            report = visual.evaluate_palette(samples_of([image for _ in range(3)]),
                                             expected_frequency_colors={"24.150": "magenta"}, expected_bars=BARS)
            self.assertEqual("FAIL", report["result"], report)

    def test_missing_or_unreadable_palette_coverage_cannot_pass(self):
        for image in (fixture(card=False), np.zeros((720, 1280, 3), dtype=np.uint8)):
            report = visual.evaluate_palette(samples_of([image for _ in range(3)]),
                                             expected_frequency_colors={"24.150": "magenta"}, expected_bars=BARS)
            self.assertEqual("INCOMPLETE", report["result"], report)

    def test_palette_after_reboot_cannot_silently_revert(self):
        desired = samples_of([fixture(frequency="cyan", card=True) for _ in range(3)])
        reverted = samples_of([fixture(frequency="magenta", card=True) for _ in range(3)])
        contract = {"expected_frequency_colors": {"24.150": "cyan"}, "expected_bars": BARS}
        self.assertEqual("PASS", visual.evaluate_palette(desired, **contract)["result"])
        self.assertEqual("FAIL", visual.evaluate_palette(reverted, **contract)["result"])

    def test_zero_and_three_second_clearance_require_positive_controls(self):
        for seconds in (0., 3.):
            rows, reference = timed_series(persistence=seconds)
            report = clearance(rows, reference, persistence_s=seconds)
            self.assertEqual("PASS", report["result"], report)
            for row in rows:
                if row["time_s"] < 0:
                    row.update({key: value for key, value in reference[0].items() if key != "time_s"})
            self.assertEqual("INCOMPLETE", clearance(rows, reference, persistence_s=seconds)["result"])

    def test_stuck_primary_wrong_card_or_arrow_remnant_fail(self):
        rows, reference = timed_series(stuck=True)
        self.assertEqual("FAIL", clearance(rows, reference)["result"])
        for field in ("card0", "card1", "arrows", "main_bars"):
            rows, reference = timed_series()
            for row in rows:
                if row["time_s"] >= 3:
                    row["activity"][field] = 1000
            report = clearance(rows, reference)
            self.assertEqual("FAIL", report["result"], (field, report))

    def test_persistence_cannot_clear_early(self):
        rows, reference = timed_series(early=True)
        report = clearance(rows, reference)
        self.assertEqual("FAIL", report["result"], report)
        self.assertTrue(any(row.get("field") == "hold" for row in report["failures"]))

    def test_blacked_out_frame_is_incomplete_not_absence_pass(self):
        rows, reference = timed_series(blackout=True)
        self.assertEqual("INCOMPLETE", clearance(rows, reference)["result"])

    def test_hold_losing_status_guards_cannot_pass_even_with_readable_frequency(self):
        rows, reference = timed_series()
        for row in rows:
            if .5 <= row["time_s"] <= 2.5:
                row["guards"]["counter"] = 0
        self.assertEqual("INCOMPLETE", clearance(rows, reference)["result"])

    def test_missing_idle_placeholder_is_incomplete_even_if_status_survives(self):
        rows, reference = timed_series()
        for row in rows:
            if row["time_s"] >= 3:
                row["frequency_grid"] = [0.] * 120
                row["activity"]["frequency_vertical"] = 0
        self.assertEqual("INCOMPLETE", clearance(rows, reference)["result"])

    def test_missing_middle_samples_or_truncated_tail_cannot_pass(self):
        rows, reference = timed_series()
        for selected in ([row for row in rows if not 3.7 <= row["time_s"] <= 4.0],
                         [row for row in rows if row["time_s"] < 3.7]):
            self.assertEqual("INCOMPLETE", clearance(selected, reference)["result"])

    def test_duplicate_or_nonfinite_sample_times_are_rejected(self):
        rows, reference = timed_series()
        for value in (rows[0]["time_s"], float("nan"), True):
            modified = deepcopy(rows)
            modified[1]["time_s"] = value
            with self.assertRaises(ValueError):
                clearance(modified, reference)
        reference[1]["time_s"] = reference[0]["time_s"]
        with self.assertRaises(ValueError):
            clearance(rows, reference)

    def test_invalid_registration_never_produces_observation(self):
        for registration in ({}, {"result": "FAIL"}, {"result": "PASS", "transform": {
                "kind": "dynamic_similarity", "crop_fractions": [0, 0, 0, 1]}}):
            with self.assertRaises(ValueError):
                visual.observe_frame(fixture(), registration)

    def test_every_visual_field_uses_registered_pose(self):
        source = fixture(card=True)
        dx, dy = 35, 40
        moved = np.asarray(Image.fromarray(source).transform((1280, 720), Image.Transform.AFFINE,
                           (1, 0, -dx, 0, 1, -dy), resample=Image.Resampling.BILINEAR))
        x, y, width, height = READER_REFERENCE_CROP
        registration = {"result": "PASS", "transform": {"kind": "dynamic_similarity",
                        "crop_fractions": [x + dx / 1280, y + dy / 720, width, height]}}
        observed = visual.observe_frame(moved, registration)
        self.assertEqual("24.150", observed["primary_frequency"])
        self.assertEqual(BARS, [cell["color"] for cell in observed["main_bars"]])
        self.assertEqual(BARS, [cell["color"] for cell in observed["card_bars"][0]])

    def test_secondary_expiry_keeps_live_primary_as_control(self):
        rows, reference = timed_series(card_only=True)
        contract = {"active_window": (-.875, -.375), "clear_time_s": 0., "persistence_s": 3.}
        report = visual.evaluate_card_clearance(rows, reference, **contract)
        self.assertEqual("PASS", report["result"], report)
        for row in rows:
            if row["time_s"] >= 3:
                row["activity"]["card1"] = 900
        self.assertEqual("FAIL", visual.evaluate_card_clearance(rows, reference, **contract)["result"])

    def test_secondary_control_cannot_be_missing_or_replaced(self):
        rows, reference = timed_series(card_only=True)
        for row in rows:
            if row["time_s"] >= 3:
                row["primary_frequency"] = None
        report = visual.evaluate_card_clearance(rows, reference, active_window=(-.875, -.375),
                                                clear_time_s=0., persistence_s=3.)
        self.assertEqual("INCOMPLETE", report["result"], report)
        for row in rows:
            if row["time_s"] >= 3:
                row["primary_frequency"] = "24.150"
        report = visual.evaluate_card_clearance(rows, reference, active_window=(-.875, -.375),
                                                clear_time_s=0., persistence_s=3.)
        self.assertEqual("FAIL", report["result"], report)

    def test_empty_card_reference_requires_the_same_visible_primary(self):
        rows, reference = timed_series(card_only=True)
        for missing_or_wrong in (None, "24.150"):
            modified = deepcopy(reference)
            modified[0]["primary_frequency"] = missing_or_wrong
            report = visual.evaluate_card_clearance(rows, modified, active_window=(-.875, -.375),
                                                    clear_time_s=0., persistence_s=3.)
            self.assertEqual("INCOMPLETE", report["result"], report)

    def test_readable_wrong_primary_is_failure_not_unreadable(self):
        for period in ("active", "hold"):
            rows, reference = timed_series()
            for row in rows:
                if (period == "active" and row["time_s"] < 0) or (period == "hold" and .5 < row["time_s"] < 2.5):
                    row["primary_frequency"] = "35.500"
            self.assertEqual("FAIL", clearance(rows, reference)["result"])

    def test_reference_with_an_unwanted_card_cannot_authorize_absence(self):
        rows, reference = timed_series()
        reference[0]["activity"]["card0"] = 1000
        self.assertEqual("INCOMPLETE", clearance(rows, reference)["result"])

    def test_live_takeover_is_observed_before_retained_alert_natural_expiry(self):
        report = takeover(takeover_series())
        self.assertEqual("PASS", report["result"], report)
        self.assertLess(report["takeover_window_s"][1], report["natural_expiry_s"])
        # The former primary is permitted to remain as a secondary card.
        rows = takeover_series()
        for row in rows:
            if row["time_s"] >= 1:
                row["activity"]["card0"] = 1000
        self.assertEqual("PASS", takeover(rows)["result"])

    def test_takeover_rejects_stuck_wrong_or_late_primary(self):
        for rows in (takeover_series(switch_at=3.), takeover_series(replacement="35500"),
                     takeover_series(switch_at=1.75)):
            self.assertEqual("FAIL", takeover(rows)["result"])

    def test_takeover_requires_the_old_retained_primary_control(self):
        rows = takeover_series()
        for row in rows:
            if row["time_s"] < 1:
                row["primary_frequency"] = None
        self.assertEqual("INCOMPLETE", takeover(rows)["result"])
        for row in rows:
            if row["time_s"] < 1:
                row["primary_frequency"] = "24.150"
        self.assertEqual("FAIL", takeover(rows)["result"])

    def test_takeover_blackout_or_observation_gaps_cannot_pass(self):
        for period in ("retained", "live"):
            rows = takeover_series()
            for row in rows:
                if (period == "retained" and row["time_s"] < 1) or (period == "live" and row["time_s"] > 1):
                    row["guards"]["status"] = 0
            self.assertEqual("INCOMPLETE", takeover(rows)["result"])
        rows = [row for row in takeover_series() if not 1.5 <= row["time_s"] <= 1.75]
        self.assertEqual("INCOMPLETE", takeover(rows)["result"])

    def test_takeover_contract_cannot_pass_after_natural_expiry(self):
        for seconds in (0., 1., 2.):
            with self.assertRaises(ValueError):
                takeover(takeover_series(), persistence_s=seconds)


if __name__ == "__main__":
    unittest.main()
