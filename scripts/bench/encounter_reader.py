"""Offline observations of one registered display image, without expected data.

This fixed-layout instrument uses segment interiors and shape/color patches.
Apple Vision reads only the small secondary text. A refusal is never absence.
The calibration is relative to the SCAN landmark, as in counter_reader.
"""
from __future__ import annotations

import base64
from contextlib import contextmanager
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np
import PIL
from PIL import Image, ImageDraw, ImageOps

import counter_reader

FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")
METHOD_VERSION = 11
_ocr_binary = None
_ocr_setup = None
_ocr_session = None


def field(state, value=None, reason=None, **diagnostics):
    return {"state": state, "value": value, "reason": reason, **diagnostics}


def prepare_reader(cache_dir: Path | None = None) -> dict:
    """Compile the local OCR helper once; failure leaves other readers usable.

    The caller chooses an artifact/cache directory. This function never changes
    permissions, downloads dependencies, or escapes its execution sandbox.
    """
    global _ocr_binary, _ocr_setup
    if _ocr_setup is not None:
        return dict(_ocr_setup)
    source = Path(__file__).with_name("encounter_ocr.swift")
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    cache = Path(cache_dir) if cache_dir is not None else Path(tempfile.gettempdir()) / "encounter-reader-cache"
    binary = cache / ("vision-" + digest[:16])
    info = {"method_version": METHOD_VERSION, "ocr": "Apple Vision",
            "ocr_revision": 3, "ocr_language_correction": False,
            "ocr_source_sha256": digest, "ocr_available": False,
            "numpy_version": np.__version__, "pillow_version": PIL.__version__}
    try:
        cache.mkdir(parents=True, exist_ok=True)
        if not binary.is_file():
            result = subprocess.run(["/usr/bin/swiftc", "-O", "-module-cache-path",
                                     str(cache / "modules"), str(source), "-o", str(binary)],
                                    capture_output=True, timeout=180)
            if result.returncode:
                raise RuntimeError("local OCR helper compilation failed")
        _ocr_binary = binary
        info["ocr_available"] = True
        info["ocr_binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    except (OSError, subprocess.SubprocessError, RuntimeError):
        info["ocr_error"] = "local OCR helper unavailable"
    _ocr_setup = info
    return dict(info)


@contextmanager
def analysis_session():
    """Reuse one OCR helper for this analysis, with no retry after failure."""
    global _ocr_session
    from encounter_ocr_session import OCRSession
    prepare_reader()
    prior = _ocr_session
    if prior is not None or _ocr_binary is None:
        yield
        return
    with OCRSession(_ocr_binary) as session:
        _ocr_session = session
        try:
            yield
        finally:
            _ocr_session = prior


def _ocr(crops):
    prepare_reader()
    if _ocr_binary is None:
        return None
    encoded = []
    for crop in crops:
        # Fixed enlargement and local contrast normalization, never a search
        # selected using recognized text or an expected answer.
        image = ImageOps.autocontrast(Image.fromarray(crop).convert("L"))
        image = ImageOps.invert(image).resize((image.width * 4, image.height * 4))
        stream = io.BytesIO()
        image.save(stream, format="PNG")
        encoded.append(base64.b64encode(stream.getvalue()).decode("ascii"))
    if _ocr_session is not None:
        return _ocr_session.request(encoded)
    try:
        completed = subprocess.run([str(_ocr_binary)], input=json.dumps(encoded) + "\n",
                                   text=True, capture_output=True, timeout=20)
        result = json.loads(completed.stdout)
        if completed.returncode or "error" in result or len(result.get("crops", [])) != len(crops):
            return None
        return result["crops"]
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


class Pixels:
    def __init__(self, rgb, width, height, registration):
        self.image = np.frombuffer(rgb, dtype=np.uint8).reshape(height, width, 3)
        x1, y1, x2, y2 = registration["landmark_bounds"]
        self.scale = ((x2 - x1 + 1) / 220 * width / 1280,
                      (y2 - y1 + 1) / 79 * height / 720)
        self.anchor = (x1 * width / 960, y1 * height / 540)

    def crop(self, box):
        result = []
        for i, v in enumerate(box):
            axis = i % 2
            origin = (376 * 4 / 3, 192 * 4 / 3)[axis]
            result.append(round(self.anchor[axis] + (v - origin) * self.scale[axis]))
        x1, y1, x2, y2 = result
        if not 0 <= x1 < x2 <= self.image.shape[1] or not 0 <= y1 < y2 <= self.image.shape[0]:
            raise ValueError("registered display region leaves the image")
        return self.image[y1:y2, x1:x2]

    def level(self, box):
        return self.crop(box).max(axis=2).astype(float)


def _fill(values, on=45, off=32):
    low, median, high = np.percentile(values, [10, 50, 90])
    state = "on" if low >= on else "off" if high <= off else "partial"
    return state, {"p10": round(float(low), 2), "median": round(float(median), 2),
                   "p90": round(float(high), 2)}


def _frequency_lower_left(pixels, box, absence_guard):
    # The tapered lower-left stroke needs a centered, longitudinal witness.
    # A short patch on its right edge rejects complete dim glyphs and can
    # accept an isolated middle fragment. Require all three body sections to
    # agree, with the same photometric limits as the other six strokes.
    left, top, right, bottom = box
    boundaries = (top, top + (bottom - top) // 3,
                  top + 2 * (bottom - top) // 3, bottom)
    sections = [pixels.level((left, start, right, end))
                for start, end in zip(boundaries, boundaries[1:])]
    states = [_fill(section)[0] for section in sections]
    state = states[0] if len(set(states)) == 1 else "partial"
    # Moving the positive witness must not hide a remnant on the old right
    # support. Both supports must be dark before this stroke can be absent.
    guard_state, guard_values = _fill(pixels.level(absence_guard))
    if state == "off" and guard_state != "off":
        state = "partial"
    _, values = _fill(np.concatenate([section.ravel() for section in sections]))
    return state, {**values, "longitudinal_states": states,
                   "absence_guard": {"state": guard_state, **guard_values}}


def _dark_frequency_placeholder(pixels, details):
    # The dash font is centered independently of numeric text. These fixed
    # stroke and decimal interiors follow the registered image geometry; no
    # input, firmware state, expected text or neighboring frame is consulted.
    strokes = []
    for left, right in ((480, 508), (556, 584), (648, 676), (726, 754), (804, 832)):
        background = float(np.median(np.concatenate([
            pixels.level((left, 280, right, 290)).ravel(),
            pixels.level((left, 327, right, 338)).ravel()])))
        body = pixels.level((left, 304, right, 309))
        parts = [part for row in np.array_split(body, 2, axis=0)
                 for part in np.array_split(row, 3, axis=1)]
        measures = [{"p10": round(float(np.percentile(part, 10)), 2),
                     "median": round(float(np.median(part)), 2)} for part in parts]
        body_median = float(np.median(body))
        # Eight-bit capture/compression can vary an intact stroke by one level
        # across a small subdivision. Require contrast throughout the shape,
        # and stronger contrast in the whole body, without requiring uniform
        # illumination. An erased section still fails; uniformly faint marks
        # still lack the whole-body witness.
        complete = (body_median >= background + 3
                    and all(part["median"] >= background + 2 and part["p10"] >= background + 1
                            for part in measures))
        strokes.append({"background": background, "body_median": body_median,
                        "parts": measures, "complete": complete})
    decimal_background = float(np.median(pixels.level((606, 334, 622, 340))))
    decimal = pixels.level((610, 350, 618, 356))
    decimal_parts = [part for row in np.array_split(decimal, 2, axis=0)
                     for part in np.array_split(row, 2, axis=1)]
    decimal_levels = [{"p10": round(float(np.percentile(part, 10)), 2),
                       "median": round(float(np.median(part)), 2)} for part in decimal_parts]
    decimal_complete = all(part["median"] >= decimal_background + 2
                           and part["p10"] >= decimal_background + 1 for part in decimal_levels)
    # The remaining upper/lower glyph body must be dark. A numeric remnant
    # cannot borrow five horizontal strokes to become a dash-only literal.
    background = float(np.median(pixels.level((448, 270, 840, 290))))
    extra_contrast = 0.0
    guards = ((448, 258, 840, 292), (448, 321, 840, 341),
              (448, 347, 600, 359), (632, 347, 840, 359),
              (524, 294, 540, 318), (598, 294, 634, 318),
              (688, 294, 712, 318), (772, 294, 788, 318))
    for box in guards:
        windows = np.lib.stride_tricks.sliding_window_view(pixels.level(box), (4, 4))[::2, ::2]
        contrast = float(np.max(np.median(windows, axis=(-2, -1)))) - background
        extra_contrast = max(extra_contrast, contrast)
    diagnostics = {"strokes": strokes, "decimal": {"background": decimal_background,
                    "parts": decimal_levels, "complete": decimal_complete},
                   "maximum_extra_body_contrast": round(extra_contrast, 2),
                   "basis": "Five fixed dash interiors and a separate decimal against local background; "
                            "all stroke sections required. Contrast below the retained limits is unresolved."}
    if all(stroke["complete"] for stroke in strokes) and decimal_complete and extra_contrast < 3:
        return field("readable", "--.---", cells=details, dark_placeholder=diagnostics)
    possible_strokes = any(part["median"] - stroke["background"] >= 1.5
                           for stroke in strokes for part in stroke["parts"])
    possible_decimal = any(part["median"] - decimal_background >= 1.5 for part in decimal_levels)
    if possible_strokes or possible_decimal or extra_contrast >= 3:
        return field("ambiguous", reason="dark frequency marks do not establish five complete dashes and decimal",
                     cells=details, dark_placeholder=diagnostics)
    return field("absent", reason="visible registered frequency region has no supported glyph contrast",
                 cells=details, dark_placeholder=diagnostics)


def _frequency(pixels):
    # Five fixed seven-segment cells, with the decimal separately witnessed.
    x_origins = (454, 520, 616, 688, 764)
    patches = {"a": (18, 261, 41, 266), "b": (52, 280, 57, 293),
               "c": (51, 324, 56, 337), "d": (17, 350, 40, 355),
               "e": (4, 316, 8, 344), "f": (7, 280, 11, 293),
               "g": (18, 303, 41, 309)}
    digits, details, illuminated_by_digit = [], [], []
    for origin in x_origins:
        mask, measurements, illuminated = "", {}, []
        for name, (x1, y1, x2, y2) in patches.items():
            box = (origin + x1, y1, origin + x2, y2)
            if name == "e":
                state, values = _frequency_lower_left(
                    pixels, box, (origin + 7, 324, origin + 12, 337))
            else:
                state, values = _fill(pixels.level(box))
            measurements[name] = {"state": state, **values}
            if state == "on":
                mask += name
                illuminated.append(values["median"])
        details.append({"mask": mask, "segments": measurements})
        digits.append(counter_reader.MASKS.get(mask))
        illuminated_by_digit.append(illuminated)
    region = pixels.level((448, 252, 840, 362))
    if float(np.percentile(region, 99.5)) <= 32:
        return _dark_frequency_placeholder(pixels, details)
    if any(v["state"] == "partial" for d in details for v in d["segments"].values()):
        return field("ambiguous", reason="partial or dim frequency segment interiors", cells=details)
    if not all(d is not None and d.isdigit() for d in digits):
        return field("unreadable", reason="frequency does not form five canonical numeric glyphs", cells=details)
    # Definite strokes determine literal content. An extra complete middle
    # stroke makes an observed 8, which the independent input comparison can
    # reject when the requested digit is 0. Unequal sampled brightness alone
    # does not change that literal; retain it without claiming physical
    # illumination uniformity or a settled panel response.
    anomalies = []
    for cell_index, illuminated in enumerate(illuminated_by_digit):
        reference = float(np.median(illuminated)) if illuminated else 0.0
        for name, segment in details[cell_index]["segments"].items():
            if segment["state"] == "on" and (segment["median"] < reference * .85 or
                                              segment["median"] > reference / .85):
                anomalies.append({"cell": cell_index + 1, "segment": name,
                                  "median": segment["median"], "reference_median": reference,
                                  "ratio": round(segment["median"] / reference, 6)})
    illumination = {"within_sampled_ratio_bounds": not anomalies,
                    "ratio_bounds": [.85, 1 / .85], "anomalies": anomalies,
                    "meaning": "sampled stroke brightness only; physical uniformity and settling are not evaluated"}
    # The two enclosed holes of every seven-segment cell must stay clear.
    # Extra central ink cannot borrow a valid answer from the sampled strokes.
    for origin in x_origins:
        for top, bottom in ((278, 292), (322, 338)):
            if float(np.mean(pixels.level((origin + 28, top, origin + 40, bottom)) > 40)) > .10:
                return field("ambiguous", reason="frequency ink enters a glyph background interior", cells=details,
                             sampled_illumination=illumination)
    decimal, _ = _fill(pixels.level((590, 349, 599, 357)))
    if decimal != "on":
        return field("ambiguous", reason="frequency decimal is not clearly visible", cells=details,
                     sampled_illumination=illumination)
    return field("readable", "".join(digits[:2]) + "." + "".join(digits[2:]), cells=details,
                 sampled_illumination=illumination)


def _bands(pixels):
    bands, diagnostics = [], {}
    for name, box in (("L", (317, 188, 355, 245)), ("Ka", (315, 253, 407, 310)),
                      ("K", (314, 319, 363, 377)), ("X", (313, 384, 363, 440))):
        level = pixels.level(box)
        fraction = float(np.mean(level > 45))
        dim = float(np.mean(level > 25))
        diagnostics[name] = {"lit_fraction": round(fraction, 4), "dim_fraction": round(dim, 4)}
        if fraction > .12:
            bands.append(name)
        elif dim > .05:
            return field("ambiguous", reason="partial or dim band label", bands=diagnostics)
    return field("readable", bands, bands=diagnostics)


def _bars(pixels, boxes):
    states, measurements = [], []
    for box in boxes:
        state, data = _fill(pixels.level(box))
        states.append(state)
        measurements.append(data)
    if "partial" in states or states != sorted(states, key=lambda v: v != "on"):
        return field("ambiguous", reason="partial or noncontiguous strength bars", bars=measurements)
    return field("readable", states.count("on"), bars=measurements)


def _compatible_bar_counts(states):
    """Counts consistent with definite cells in a contiguous six-cell meter."""
    return [count for count in range(7) if all(
        state == "partial" or (state == "on" and index < count) or
        (state == "off" and index >= count)
        for index, state in enumerate(states))]


def _card_spatial_ink(interior):
    """Witness a faint mark independently of the quadrant boundaries."""
    support = interior > 32  # Include bright pixels so brightening cannot split a mark.
    if min(support.shape) >= 3:
        windows = np.lib.stride_tricks.sliding_window_view(support, (3, 3))
        if np.any(windows.sum(axis=(-1, -2)) >= 5):
            return True
    unseen = set(map(tuple, np.argwhere(support)))
    while unseen:
        stack = [unseen.pop()]
        area = 0
        while stack:
            y, x = stack.pop()
            area += 1
            if area >= 5:
                return True
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    point = (y + dy, x + dx)
                    if point in unseen:
                        unseen.remove(point)
                        stack.append(point)
    return False


def _card_bars(pixels, left):
    """Locate one six-cell meter by its perimeter, then read its interiors.

    Registration never scores a proposed filled/hollow state or bar count.
    A shared grid prevents independently moving each crop onto convenient ink.
    The outer strokes are excluded from the interiors, including their camera
    fringe; partial, faint and noncontiguous fills still refuse a count.
    """
    level = pixels.level((left + 8, 419, left + 221, 450))
    sy, sx = level.shape[0] / 31, level.shape[1] / 213
    vertical = np.diff(np.median(level[:, round(8 * sx):round(202 * sx)], axis=1))
    top_candidates = range(round(3 * sy), round(10 * sy))
    bottom_candidates = range(round(19 * sy), round(28 * sy))
    top_edge = max(top_candidates, key=lambda y: vertical[y])
    bottom_edge = min(bottom_candidates, key=lambda y: vertical[y])
    top, bottom = top_edge + 1, bottom_edge + 1
    if vertical[top_edge] < 12 or vertical[bottom_edge] > -12 or bottom - top < 10 * sy:
        return field("ambiguous", reason="secondary meter perimeter is not resolved")
    band = max(1, round(2 * sy))
    perimeter = np.median(np.concatenate((level[top:top + band], level[bottom - band:bottom])), axis=0)
    # A two-pixel edge contrast is less sensitive to the subpixel placement of
    # an outline than an adjacent-pixel derivative.
    edge = np.zeros(len(perimeter))
    edge[2:-1] = (perimeter[2:-1] + perimeter[3:] - perimeter[:-3] - perimeter[1:-2]) / 2
    best = None
    for origin in np.arange(4, 11, .5):
        for pitch in np.arange(32, 35.01, .25):
            cells = [(round((origin + i * pitch) * sx),
                      round((origin + i * pitch + pitch * 19 / 21) * sx)) for i in range(6)]
            if cells[0][0] < 1 or cells[-1][1] >= len(edge):
                continue
            contrasts = [v for start, end in cells for v in (edge[start], -edge[end])]
            score = float(np.mean(np.clip(contrasts, -60, 60)))
            if best is None or score > best[0]:
                best = (score, cells, contrasts)
    # Three complete perimeter pairs establish the shared origin and pitch;
    # very dark outlines in other cells must not count as illuminated bars.
    supported_cells = sum(a > 5 and b > 5 for a, b in zip(best[2][::2], best[2][1::2])) if best else 0
    if best is None or best[0] < 12 or supported_cells < 3:
        return field("ambiguous", reason="secondary meter six-cell outline is not resolved",
                     perimeter_score=round(best[0], 2) if best else None,
                     perimeter_edges=[round(v, 2) for v in best[2]] if best else [])
    inset_x, inset_y = max(2, round(4 * sx)), max(2, round(4 * sy))
    states, measurements = [], []
    for start, end in best[1]:
        interior = level[top + inset_y:bottom - inset_y, start + inset_x:end - inset_x]
        if min(interior.shape) < 2:
            return field("ambiguous", reason="secondary meter interior is too small")
        # Keep local partial drawing visible rather than accepting its average.
        quadrants = [part for half in np.array_split(interior, 2, axis=0)
                     for part in np.array_split(half, 2, axis=1)]
        readings = [_fill(part) for part in quadrants]
        state = readings[0][0] if len({s for s, _ in readings}) == 1 else "partial"
        spatial_ink = _card_spatial_ink(interior)
        if state != "on":
            # Midlevel ink has a declared spatial resolution: five connected
            # pixels, or five pixels within any 3x3 neighborhood. Smaller or
            # more dispersed marks may read off; this is not proof of noise.
            # Keep bright-pixel percentile evidence and all-on acceptance.
            bright_only = np.where((interior > 32) & (interior < 45), 32, interior)
            bright_quadrants = [part for half in np.array_split(bright_only, 2, axis=0)
                                for part in np.array_split(half, 2, axis=1)]
            state = "off" if (not spatial_ink and all(
                _fill(part)[0] == "off" for part in bright_quadrants)) else "partial"
        # Percentiles can hide a narrow but coherent partial stroke. Three
        # neighboring contrary pixels in either direction retain that evidence.
        contrary = interior >= 45 if state == "off" else interior <= 32
        run = min(3, *contrary.shape)
        if state in ("on", "off") and (
                any(np.any(np.all(contrary[y:y + run], axis=0)) for y in range(contrary.shape[0] - run + 1)) or
                any(np.any(np.all(contrary[:, x:x + run], axis=1)) for x in range(contrary.shape[1] - run + 1))):
            state = "partial"
        # The renderer leaves four display pixels of meter padding above and
        # below its ten-pixel bars. Use that padding beyond the camera fringe;
        # a thin strip immediately outside a bright outline can sample only
        # its compression undershoot and invent a faint interior fill.
        background = np.concatenate((level[max(0, top - 3 * band):top - band, start + inset_x:end - inset_x].ravel(),
                                     level[bottom + band:bottom + 3 * band, start + inset_x:end - inset_x].ravel()))
        contrast = float(np.median(interior) - np.median(background))
        # Eight levels is the declared local-contrast detection floor. This
        # measurement does not distinguish arbitrarily faint ink from noise.
        if state == "off" and contrast >= 8:
            state = "partial"
        states.append(state)
        measurements.append({"state": state, "quadrants": [d for _, d in readings],
                             "spatial_ink_supported": spatial_ink,
                             "background_contrast": round(contrast, 2),
                             "interior": [start + inset_x, top + inset_y, end - inset_x, bottom - inset_y]})
    diagnostics = {"bars": measurements, "perimeter_score": round(best[0], 2),
                   "grid": {"top": top, "bottom": bottom, "cells": best[1]},
                   "compatible_counts": _compatible_bar_counts(states)}
    if "partial" in states or states != sorted(states, key=lambda v: v != "on"):
        return field("ambiguous", reason="partial, faint or noncontiguous secondary strength bars", **diagnostics)
    return field("readable", states.count("on"), **diagnostics)


def _arrows(pixels):
    # The three old probes retain their measured threshold contract. Inset
    # whole-glyph interiors also witness fill between those probes, so three
    # isolated bright rectangles cannot masquerade as an intact direction.
    boxes = {"front": ((1064, 225, 1084, 247), (1035, 274, 1055, 284), (1100, 274, 1118, 284)),
             "side": ((1050, 322, 1095, 332), (1008, 321, 1022, 331), (1135, 321, 1149, 331)),
             "rear": ((1063, 380, 1086, 386), (1044, 369, 1057, 374), (1096, 369, 1109, 374))}
    interiors = {
        "front": ((1008, 278), (1077, 203), (1144, 278), (1115, 278),
                  (1115, 292), (1042, 292), (1042, 278)),
        "side": ((1003, 328), (1022, 313), (1022, 320), (1135, 320),
                 (1135, 313), (1152, 329), (1135, 343), (1135, 337),
                 (1022, 337), (1022, 344)),
        "rear": ((1044, 372), (1059, 372), (1059, 366), (1093, 366),
                 (1093, 372), (1112, 372), (1077, 391)),
    }
    region = (990, 190, 1165, 400)
    rgb = pixels.crop(region)
    height, width = rgb.shape[:2]
    levels = rgb.max(axis=2)
    arrows, diagnostics, directions = [], {}, {}

    def coherent(mask):
        # Ignore an isolated noisy pixel, but retain a thin three-pixel stroke.
        return bool(np.any(mask[:, :-2] & mask[:, 1:-1] & mask[:, 2:]) or
                    np.any(mask[:-2] & mask[1:-1] & mask[2:]))

    for name, patches in boxes.items():
        readings = [_fill(pixels.level(box)) for box in patches]
        diagnostics[name] = [{"state": state, **detail} for state, detail in readings]
        mask_image = Image.new("1", (width, height))
        ImageDraw.Draw(mask_image).polygon([
            ((x - region[0]) * width / (region[2] - region[0]),
             (y - region[1]) * height / (region[3] - region[1]))
            for x, y in interiors[name]], fill=1)
        mask = np.asarray(mask_image, dtype=bool)
        # Retain a compact, expectation-blind spatial witness for sequence
        # interpretation.  The grid covers the fixed direction polygon's
        # bounding box, including its background corners.  It does not change
        # this frame's filled/partial/faint/unlit decision.
        polygon = interiors[name]
        profile_bounds = (min(x for x, _ in polygon), min(y for _, y in polygon),
                          max(x for x, _ in polygon) + 1, max(y for _, y in polygon) + 1)
        profile_level = pixels.level(profile_bounds)
        profile = [round(float(np.median(cell)), 2)
                   for row in np.array_split(profile_level, 4, axis=0)
                   for cell in np.array_split(row, 4, axis=1)]
        interior_state, level_detail = _fill(levels[mask])
        colors = rgb[mask].astype(int)
        medians = np.median(colors, axis=0)
        maximum, minimum = max(medians), min(medians)
        dominant = int(np.argmax(medians))
        color = "neutral" if maximum - minimum < max(8, maximum * .15) else (
            "warm" if dominant == 0 else "green" if dominant == 1 else "blue")
        # Preserve the existing faint warm-fill refusal and extend it to other
        # coherent colored fills. Neutral resting glyphs are not active arrows.
        red_tint, colored_tint = [], []
        for box in patches:
            patch = pixels.crop(box).astype(int)
            red_tint.append(float(np.percentile(patch[:, :, 0] -
                                                np.maximum(patch[:, :, 1], patch[:, :, 2]), 10)))
            channels = [c for c in range(3) if c != dominant]
            colored_tint.append(float(np.percentile(patch[:, :, dominant] -
                                                    np.maximum(patch[:, :, channels[0]], patch[:, :, channels[1]]), 10)))
        all_on = all(state == "on" for state, _ in readings)
        all_off = all(state == "off" for state, _ in readings)
        if all_on and interior_state == "on" and not coherent(mask & (levels <= 32)):
            state = "filled"
            arrows.append(name)
        elif all_off and interior_state == "off" and not coherent(mask & (levels >= 45)):
            state = "faint" if min(red_tint) > 0 or min(colored_tint) >= 8 else "unlit"
        else:
            state = "partial"
        directions[name] = {"state": state, "color": color,
                            "rgb_median": [round(float(v), 2) for v in medians],
                            "interior": level_detail,
                            "profile": {"rows": 4, "columns": 4,
                                        "reference_bounds": list(profile_bounds),
                                        "max_channel_medians": profile}}
    unresolved = [name for name, detail in directions.items() if detail["state"] in ("partial", "faint")]
    # Complete all three observations before deciding whether the combined
    # field is readable. A faint side arrow must not hide a clear front arrow.
    reason = "; ".join(f"{name} arrow {directions[name]['state']}" for name in unresolved) or None
    return field("ambiguous" if unresolved else "readable", None if unresolved else arrows,
                 reason=reason, arrows=diagnostics, direction_states=directions,
                 visible_directions=arrows,
                 color_qualification="observed color only; no color correctness contract")

def _card_direction(pixels, left):
    """Read the complete renderer-owned triangle or horizontal rectangle.

    The card renderer uses a 12 by 12 triangle or 12 by 4 rectangle. Locate
    the neutral symbol within its padded layout region, then verify the whole
    shape. Fixed top/bottom strips can cut off a translated triangle's tip.
    Neither the crop nor the threshold depends on a proposed direction.
    """
    rgb = pixels.crop((left + 10, 377, left + 47, 416))
    level = rgb.min(axis=2).astype(float)
    floor, ceiling = np.percentile(level, [10, 99])
    bright = level > max(30, floor + .4 * (ceiling - floor))
    # Two agreeing pixels locate the extent without promoting isolated noise.
    ys = np.flatnonzero(bright.sum(axis=1) >= 2)
    xs = np.flatnonzero(bright.sum(axis=0) >= 2)
    if not len(xs) or not len(ys):
        return field("ambiguous", reason="secondary direction has no resolved symbol")
    x1, x2, y1, y2 = int(xs[0]), int(xs[-1]) + 1, int(ys[0]), int(ys[-1]) + 1
    sx, sy = level.shape[1] / 37, level.shape[0] / 39
    width, height = (x2 - x1) / sx, (y2 - y1) / sy
    diagnostics = {"symbol_bounds": [x1, y1, x2, y2],
                   "reference_size": [round(width, 2), round(height, 2)]}
    # The source symbol projects to about 20 pixels wide in this registration.
    # The padded region must contain all of it; a cropped shape is unknown.
    if (x1 == 0 or y1 == 0 or x2 == level.shape[1] or y2 == level.shape[0] or
            not 16 <= width <= 27):
        return field("ambiguous", reason="secondary direction extent is unresolved", **diagnostics)
    glyph = bright[y1:y2, x1:x2]
    yy, xx = np.mgrid[:glyph.shape[0], :glyph.shape[1]]
    xx = (xx + .5) / glyph.shape[1]
    yy = (yy + .5) / glyph.shape[0]
    matches = []
    for name in ("front", "rear", "side"):
        if name == "side":
            center_y = (y1 + y2) / (2 * sy)
            # A remaining triangle base can resemble a short rectangle, but
            # lies above/below the renderer's central side-arrow row.
            if not 4 <= height <= 10 or width / height < 2 or not 15 <= center_y <= 21:
                continue
            inner = (xx > .15) & (xx < .85) & (yy > .15) & (yy < .85)
            outer = np.zeros(glyph.shape, dtype=bool)
        else:
            if not 17 <= height <= 28 or not .7 <= width / height <= 1.4:
                continue
            half_width = (yy if name == "front" else 1 - yy) / 2
            distance = np.abs(xx - .5) - half_width
            stable_rows = (yy > .1) & (yy < .9)
            inner = (distance < -.12) & stable_rows
            outer = (distance > .12) & stable_rows
        # Leave the camera fringe out of both tests, but require the complete
        # interior and background corners rather than a top-heavy blob.
        if (inner.any() and float(glyph[inner].mean()) >= .95 and
                (not outer.any() or float(glyph[outer].mean()) <= .05)):
            matches.append(name)
    diagnostics["shape_matches"] = matches
    if len(matches) != 1:
        return field("ambiguous", reason="secondary direction is partial or noncanonical", **diagnostics)
    return field("readable", matches[0], **diagnostics)


def _secondary(pixels):
    cards, text_crops = [], []
    for slot, left in enumerate((393, 640)):
        region = pixels.level((left, 366, left + 230, 453))
        if float(np.percentile(region, 99)) < 25:
            # Dim remnants can remain well below the absolute text-reading
            # threshold. Test the card content and lower border against the
            # panel background before asserting absence. The upper
            # padding is excluded because primary-frequency light spills there.
            # At least one percent of this fixed area must stand eight levels
            # above its local background; smaller/fainter marks are outside
            # this presence measurement. This never supplies card identity.
            content = pixels.level((left + 8, 377, left + 221, 453))
            # The ten-display-pixel gutter between cards remains on-panel.
            # Also retain a darker within-card background if adjacent bright
            # content spills into that gutter. Off-panel black is not a valid
            # reference for a panel with an elevated black level.
            gutter = pixels.level((628, 382, 634, 443))
            background = min(float(np.median(gutter)), float(np.percentile(content, 10)))
            if float(np.percentile(content, 99)) - background < 8:
                continue
        # Card interiors retain their individual row association throughout.
        bars = _card_bars(pixels, left)
        arrow_reading = _card_direction(pixels, left)
        direction = arrow_reading["value"]
        text = pixels.crop((left + 47, 377, left + 228, 413))
        # Very dark text is not made certain by contrast enhancement and OCR.
        text_visible = float(np.percentile(text.max(axis=2), 90)) >= 45
        cards.append({"slot": slot, "band": None, "frequency": None, "direction": direction,
                      "bars": bars.get("value"), "compatible_bars": bars.get("compatible_counts"),
                      "text_visible": text_visible,
                      "bars_state": bars["state"], "bar_reading": bars,
                      "direction_reading": arrow_reading})
        text_crops.append(text)
    if not cards:
        return field("readable", [])
    recognized = _ocr(text_crops)
    for card, result in zip(cards, recognized or [{} for _ in cards]):
        card["ocr_observation"] = result
        candidates = []
        for row in result.get("rows", []):
            for candidate in row.get("candidates", []):
                # Vision occasionally emits visually identical Cyrillic
                # band letters even with en-US selected. Normalize glyphs;
                # never insert digits or infer a missing decimal.
                text = re.sub(r"\s", "", candidate.get("text", "")).translate(
                    str.maketrans({"К": "K", "к": "K", "а": "a", "А": "A", "Х": "X", "х": "x"}))
                match = re.fullmatch(r"(Ka|K|Ku|X|L)(\d{2}\.\d{3})", text, re.IGNORECASE)
                if match and candidate.get("confidence", 0) >= .5:
                    band, frequency = match.groups()
                    candidates.append(({"ka": "Ka", "k": "K", "ku": "Ku", "x": "X", "l": "L"}[band.lower()], frequency))
        unique = set(candidates)
        card["ocr_candidates"] = candidates
        if len(unique) == 1 and card["text_visible"]:
            card["band"], card["frequency"] = next(iter(unique))
    complete = all(all(c[k] is not None for k in ("band", "frequency", "direction", "bars"))
                   and c["bars_state"] == "readable" for c in cards)
    values = [{k: c[k] for k in ("band", "frequency", "direction", "bars")} for c in cards]
    if not complete:
        return field("unreadable", reason="secondary card text, direction, or bars are not fully readable",
                     partial_cards=values, cards=cards, ocr_available=recognized is not None)
    return field("readable", values, cards=cards)


def observe(rgb: bytes, width: int, height: int, registration: dict) -> dict:
    """Read a whole RGB24 frame. Accepts no timeline, stimulus, or expected values.

    Lists and zero/false are readable observations; an absent frequency has
    value=None. Unknowns are explicitly unreadable/ambiguous with value=None.
    Layout/color qualification is bounded to this calibrated display layout.
    """
    counter = counter_reader.observe(rgb, width, height, registration)
    result = {name: field("unreadable", reason="display visibility has not been established") for name in FIELDS}
    result["method_version"] = METHOD_VERSION
    if not counter.get("alignment", {}).get("cell_xyxy"):
        return {**result, "reason": counter["reason"]}
    result["counter_glyph"] = field(counter["state"], counter.get("glyph"), counter.get("reason"))
    try:
        pixels = Pixels(rgb, width, height, registration)
        witnesses = []
        for box in ((202, 297, 298, 363), (1000, 415, 1150, 445)):
            colors = pixels.crop(box).astype(int)
            green = (colors[:, :, 1] > 100) & (colors[:, :, 1] - np.maximum(colors[:, :, 0], colors[:, :, 2]) > 30)
            witnesses.append(float(np.mean(green)))
        result["visibility"] = {"witness_lit_fractions": witnesses}
        if min(witnesses) < .05:
            return {**{name: field("unreadable", reason="registered display visibility witnesses are dark or occluded")
                       for name in FIELDS}, "method_version": METHOD_VERSION, "visibility": result["visibility"]}
        result["primary_frequency"] = _frequency(pixels)
        result["active_bands"] = _bands(pixels)
        result["main_arrows"] = _arrows(pixels)
        result["main_bars"] = _bars(pixels, [(900, y, 937, y + 10) for y in (400, 363, 326, 289, 251, 214)])
        badge = pixels.level((569, 216, 617, 228))
        fraction = float(np.mean(badge > 45))
        result["muted_badge"] = field("readable", fraction > .3) if fraction > .3 or float(np.percentile(badge, 95)) < 25 else field("ambiguous", reason="partial muted badge")
        result["secondary"] = _secondary(pixels)
    except (ValueError, IndexError) as error:
        return {name: field("unreadable", reason=str(error)) for name in FIELDS}
    return result
