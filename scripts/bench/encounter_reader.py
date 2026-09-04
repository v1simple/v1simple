"""Offline observations of one registered display image, without expected data.

This fixed-layout instrument uses segment interiors and shape/color patches.
Apple Vision reads only the small secondary text. A refusal is never absence.
The calibration is relative to the SCAN landmark, as in counter_reader.
"""
from __future__ import annotations

import base64
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np
import PIL
from PIL import Image, ImageOps

import counter_reader

FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")
METHOD_VERSION = 3
_ocr_binary = None
_ocr_setup = None


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


def _frequency(pixels):
    # Five fixed seven-segment cells, with the decimal separately witnessed.
    x_origins = (454, 520, 616, 688, 764)
    patches = {"a": (18, 261, 41, 266), "b": (52, 280, 57, 293),
               "c": (51, 324, 56, 337), "d": (17, 350, 40, 355),
               "e": (7, 324, 12, 337), "f": (7, 280, 11, 293),
               "g": (18, 303, 41, 309)}
    digits, details, medians = [], [], []
    for origin in x_origins:
        mask, measurements = "", {}
        for name, (x1, y1, x2, y2) in patches.items():
            state, values = _fill(pixels.level((origin + x1, y1, origin + x2, y2)))
            measurements[name] = {"state": state, **values}
            if state == "on":
                mask += name
                medians.append(values["median"])
        details.append({"mask": mask, "segments": measurements})
        digits.append(counter_reader.MASKS.get(mask))
    region = pixels.level((448, 252, 840, 362))
    if float(np.percentile(region, 99.5)) <= 32:
        return field("absent", reason="visible registered frequency region has no lit glyph", cells=details)
    if any(v["state"] == "partial" for d in details for v in d["segments"].values()):
        return field("ambiguous", reason="partial or dim frequency segment interiors", cells=details)
    if not all(d is not None and d.isdigit() for d in digits):
        return field("unreadable", reason="frequency does not form five canonical numeric glyphs", cells=details)
    # An old fading stroke can otherwise turn a clear outer 0 into a valid 8.
    # Require comparable illuminated levels; do not choose a digit by dropping
    # the weaker stroke. The whole frequency is ambiguous when levels differ.
    if medians and min(medians) < max(medians) * .85:
        return field("ambiguous", reason="inconsistent illuminated frequency segment levels", cells=details)
    # The two enclosed holes of every seven-segment cell must stay clear.
    # Extra central ink cannot borrow a valid answer from the sampled strokes.
    for origin in x_origins:
        for top, bottom in ((278, 292), (322, 338)):
            if float(np.mean(pixels.level((origin + 28, top, origin + 40, bottom)) > 40)) > .10:
                return field("ambiguous", reason="frequency ink enters a glyph background interior", cells=details)
    decimal, _ = _fill(pixels.level((590, 349, 599, 357)))
    if decimal != "on":
        return field("ambiguous", reason="frequency decimal is not clearly visible", cells=details)
    return field("readable", "".join(digits[:2]) + "." + "".join(digits[2:]), cells=details)


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
        # Percentiles can hide a narrow but coherent partial stroke. Three
        # neighboring contrary pixels in either direction retain that evidence.
        contrary = interior >= 45 if state == "off" else interior <= 32
        run = min(3, *contrary.shape)
        if state in ("on", "off") and (
                any(np.any(np.all(contrary[y:y + run], axis=0)) for y in range(contrary.shape[0] - run + 1)) or
                any(np.any(np.all(contrary[:, x:x + run], axis=1)) for x in range(contrary.shape[1] - run + 1))):
            state = "partial"
        # Sample meter padding beyond the outline's immediate camera fringe.
        background = np.concatenate((level[max(0, top - 2 * band):top - band, start + inset_x:end - inset_x].ravel(),
                                     level[bottom + band:bottom + 2 * band, start + inset_x:end - inset_x].ravel()))
        contrast = float(np.median(interior) - np.median(background))
        # Eight levels is the declared local-contrast detection floor. This
        # measurement does not distinguish arbitrarily faint ink from noise.
        if state == "off" and contrast >= 8:
            state = "partial"
        states.append(state)
        measurements.append({"state": state, "quadrants": [d for _, d in readings],
                             "background_contrast": round(contrast, 2),
                             "interior": [start + inset_x, top + inset_y, end - inset_x, bottom - inset_y]})
    diagnostics = {"bars": measurements, "perimeter_score": round(best[0], 2),
                   "grid": {"top": top, "bottom": bottom, "cells": best[1]}}
    if "partial" in states or states != sorted(states, key=lambda v: v != "on"):
        return field("ambiguous", reason="partial, faint or noncontiguous secondary strength bars", **diagnostics)
    return field("readable", states.count("on"), **diagnostics)


def _arrows(pixels):
    # Disjoint interiors distinguish a filled arrow from inactive outlines.
    boxes = {"front": ((1064, 225, 1084, 247), (1035, 274, 1055, 284), (1100, 274, 1118, 284)),
             "side": ((1050, 322, 1095, 332), (1008, 321, 1022, 331), (1135, 321, 1149, 331)),
             "rear": ((1063, 380, 1086, 386), (1044, 369, 1057, 374), (1096, 369, 1109, 374))}
    arrows, diagnostics = [], {}
    for name, patches in boxes.items():
        states = [_fill(pixels.level(box)) for box in patches]
        diagnostics[name] = [{"state": s, **d} for s, d in states]
        if all(s == "on" for s, _ in states):
            arrows.append(name)
        elif not all(s == "off" for s, _ in states):
            return field("ambiguous", reason="partial or dim main arrow shape", arrows=diagnostics)
        else:
            # A consistently red-tinted interior is not certified unlit even
            # when it is too dim to qualify as a readable arrow.
            tint = []
            for box in patches:
                colors = pixels.crop(box).astype(int)
                redness = colors[:, :, 0] - np.maximum(colors[:, :, 1], colors[:, :, 2])
                tint.append(float(np.percentile(redness, 10)))
            if min(tint) > 0:
                return field("ambiguous", reason="faint colored main arrow interior", arrows=diagnostics)
    return field("readable", arrows, arrows=diagnostics)


def _secondary(pixels):
    cards, text_crops = [], []
    for slot, left in enumerate((393, 640)):
        region = pixels.level((left, 366, left + 230, 453))
        if float(np.percentile(region, 99)) < 25:
            continue
        # Card interiors retain their individual row association throughout.
        bars = _card_bars(pixels, left)
        arrow_rgb = pixels.crop((left + 17, 385, left + 41, 407))
        # White/gray symbol ink, rather than the colored card background.
        arrow = arrow_rgb.min(axis=2).astype(float)
        floor, ceiling = np.percentile(arrow, [10, 99])
        bright = arrow > max(30, floor + .4 * (ceiling - floor))
        rows = bright.mean(axis=1)
        if float(bright.mean()) < .10:
            direction = None
        else:
            top, middle, bottom = float(rows[:6].mean()), float(rows[7:13].mean()), float(rows[-6:].mean())
            direction = "side" if middle > .40 and top < .20 and bottom < .20 else (
                "front" if bottom > top + .20 else "rear" if top > bottom + .20 else None)
        text = pixels.crop((left + 47, 377, left + 228, 413))
        # Very dark text is not made certain by contrast enhancement and OCR.
        text_visible = float(np.percentile(text.max(axis=2), 90)) >= 45
        cards.append({"slot": slot, "band": None, "frequency": None, "direction": direction,
                      "bars": bars.get("value"), "text_visible": text_visible,
                      "bars_state": bars["state"], "bar_reading": bars})
        text_crops.append(text)
    if not cards:
        return field("readable", [])
    recognized = _ocr(text_crops)
    for card, result in zip(cards, recognized or [{} for _ in cards]):
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
