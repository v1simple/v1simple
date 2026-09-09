"""Frozen narrow OCR-token assembly experiment; no pixels, expected state, or retry."""
from __future__ import annotations
import math
import re

_TRANSLATE = str.maketrans({"К": "K", "к": "K", "а": "a", "А": "A", "Х": "X", "х": "x"})
_BANDS = {"ka": "Ka", "k": "K", "ku": "Ku", "x": "X", "l": "L"}


def split_card_text(observation):
    """Read one independently recognized band and complete frequency on one line.

    Exactly two unique text observations must have coherent left/right geometry.
    No punctuation is repaired, missing text supplied, candidate ignored above
    the existing confidence floor, or separate physical card associated here.
    Caller must retain its usual K/X pixel witness and direction/bar guards.
    """
    refused = ([], {"method": "literal_split_card_text/v1", "accepted": False})
    rows = observation.get("rows", [])
    if len(rows) != 2:
        return refused
    decoded = []
    for row in rows:
        box = row.get("box", [])
        if (len(box) != 4 or any(not isinstance(v, (int, float)) or isinstance(v, bool)
                                 or not math.isfinite(v) for v in box)
                or not (0 <= box[0] < box[2] <= 1 and 0 <= box[1] < box[3] <= 1)):
            return refused
        candidates = {re.sub(r"\s", "", c.get("text", "")).translate(_TRANSLATE)
                      for c in row.get("candidates", []) if c.get("confidence", 0) >= .5}
        # Differing candidate literals remain uncertainty, including punctuation.
        if len(candidates) != 1:
            return refused
        decoded.append((box, next(iter(candidates))))
    decoded.sort(key=lambda item: item[0][0])
    (band_box, band), (frequency_box, frequency) = decoded
    if band.lower() not in _BANDS or not re.fullmatch(r"\d{2}\.\d{3}", frequency):
        return refused
    bw, bh = band_box[2] - band_box[0], band_box[3] - band_box[1]
    fw, fh = frequency_box[2] - frequency_box[0], frequency_box[3] - frequency_box[1]
    overlap_y = max(0, min(band_box[3], frequency_box[3]) - max(band_box[1], frequency_box[1]))
    # OCR boxes include camera fringe. A quarter of the narrow prefix width
    # allows that fringe to overlap while preserving distinct ordered tokens.
    # The complete digit token must be wider than its prefix and share the line.
    if (fw <= bw or band_box[2] > frequency_box[0] + .25 * bw
            or overlap_y < .75 * min(bh, fh)
            or abs((band_box[1] + band_box[3]) - (frequency_box[1] + frequency_box[3])) / 2
            > .2 * max(bh, fh)):
        return refused
    return [(_BANDS[band.lower()], frequency)], {
        "method": "literal_split_card_text/v1", "accepted": True,
        "band_box": band_box, "frequency_box": frequency_box,
    }


# Complete classic GFX K: every source-font cell is checked, including its
# background. This is intentionally a K-only fallback, not an initial decoder.
_COMPLETE_K = (0x7f, 0x08, 0x14, 0x22, 0x41)


def complete_card_band(pixels, left, observation):
    """Supply K only from complete prefix pixels beside one exact numeric OCR.

    No token repair, OCR retry, neighboring-frame input, or frequency inference.
    The one-character layout position is essential: an erased Ka/Ku suffix must
    not turn a two-character prefix into K. Existing caller guards still apply.
    """
    import numpy as np
    from PIL import Image, ImageFilter
    diagnostic = {"method": "complete_card_band/v1", "accepted": False}

    def refuse(reason):
        diagnostic["reason"] = reason
        return [], diagnostic

    rows = observation.get("rows", [])
    if len(rows) != 1:
        return refuse("requires exactly one numeric-only OCR row")
    box = rows[0].get("box", [])
    if (len(box) != 4 or any(not isinstance(v, (int, float)) or isinstance(v, bool)
                            or not math.isfinite(v) for v in box)
            or not (0 <= box[0] < box[2] <= 1 and 0 <= box[1] < box[3] <= 1)):
        return refuse("numeric OCR geometry is invalid")
    candidates = {re.sub(r"\s", "", c.get("text", ""))
                  for c in rows[0].get("candidates", []) if c.get("confidence", 0) >= .5}
    if len(candidates) != 1:
        return refuse("numeric OCR literal is not unique")
    frequency = next(iter(candidates))
    if not re.fullmatch(r"[0-9]{2}\.[0-9]{3}", frequency):
        return refuse("OCR row is not one complete unmodified frequency")
    # This fixed-layout card prints a frequency after one 12-source-pixel font
    # cell plus four pixels. Vision includes a camera fringe in its word box.
    # The two-character Ka/Ku position is outside this bounded one-cell window.
    if not (.10 <= box[0] <= .18 and .55 <= box[2]-box[0] <= .80
            and box[1] <= .25 and box[3] >= .70):
        return refuse("numeric OCR is not at the one-character band position")
    rgb = pixels.crop((left + 51, 377, left + 79, 413))
    if rgb.ndim != 3 or rgb.shape[2] != 3 or min(rgb.shape[:2]) < 20:
        return refuse("band crop is too small")
    level = rgb.max(axis=2).astype(float)
    low, high = np.percentile(level, [20, 95])
    if high < 45 or high-low < 24:
        return refuse("whole band contrast is insufficient")
    support = level > (low+high)/2
    # Locate the complete connected prefix in the fixed crop; no text search.
    unseen = set(map(tuple, np.argwhere(support)))
    components = []
    while unseen:
        stack = [unseen.pop()]
        component = set(stack)
        while stack:
            y, x = stack.pop()
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    point = (y+dy, x+dx)
                    if point in unseen:
                        unseen.remove(point)
                        component.add(point)
                        stack.append(point)
        components.append(component)
    if not components:
        return refuse("band has no connected glyph")
    ys, xs = np.array(list(max(components, key=len))).T
    x0, y0, x1, y1 = map(int, (xs.min(), ys.min(), xs.max()+1, ys.max()+1))
    sx, sy = pixels.scale
    if (x0 <= 0 or y0 <= 0 or x1 >= rgb.shape[1] or y1 >= rgb.shape[0]
            or not 13 <= (x1-x0)/sx <= 23 or not 18 <= (y1-y0)/sy <= 28):
        return refuse("band extent is incomplete")
    # A complete K ends before a background gutter. Retain this independent
    # suffix-space witness rather than checking only its first letter.
    gap = pixels.crop((left + 74, 382, left + 77, 407))
    # A complete prefix must not ignore suffix ink merely because another
    # color channel supplied its K shape. Use each channel's own foreground /
    # background midpoint, as in the shape witness, with the existing dark
    # ceiling. This leaves camera fringe below glyph strength unresolved as ink.
    suffix_contradictions = []
    for channel in range(3):
        floor, ceiling = np.percentile(rgb[:, :, channel], [20, 95])
        contrary = gap[:, :, channel].astype(float) > max(32, (floor+ceiling)/2)
        if min(contrary.shape) < 3 or np.any(np.lib.stride_tricks.sliding_window_view(
                contrary, (3, 3)).sum(axis=(-1, -2)) >= 3):
            suffix_contradictions.append(channel)
    if suffix_contradictions:
        diagnostic["suffix_contradicting_channels"] = suffix_contradictions
        return refuse("coherent suffix-region ink contradicts an empty gutter")
    font = np.array([[(column >> row) & 1 for column in _COMPLETE_K]
                     for row in range(7)], dtype=bool)
    raster = Image.fromarray(font.astype(np.uint8)*255).resize(
        (x1-x0, y1-y0), Image.Resampling.NEAREST)
    diagnostic["glyph_bounds"] = [x0, y0, x1, y1]
    diagnostic["frequency_box"] = box
    diagnostics = {}
    accepted = []
    for channel, name in enumerate(("red", "green", "blue")):
        channel_level = rgb[:, :, channel].astype(float)
        floor, ceiling = np.percentile(channel_level, [20, 95])
        detail = {"contrast": round(float(ceiling-floor), 2)}
        diagnostics[name] = detail
        if ceiling < 45 or ceiling-floor < 24:
            continue
        threshold = (floor+ceiling)/2
        gap_max = float(gap[:, :, channel].max()) if gap.size else 255.
        detail["gap_max"] = gap_max
        # Uncertain channel fringe is not evidence of a blank suffix region.
        if gap_max > threshold-8:
            continue
        fits = []
        # A one-camera-pixel phase accounts for sampling of the measured glyph,
        # exactly as the existing initial witness. No scale or threshold sweep.
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                mask = Image.new("L", (rgb.shape[1], rgb.shape[0]))
                mask.paste(raster, (x0+dx, y0+dy))
                inner = np.asarray(mask.filter(ImageFilter.MinFilter(3))) > 0
                outer = np.asarray(mask.filter(ImageFilter.MaxFilter(5))) == 0
                outer[:max(0,y0-1)] = False
                outer[y1+1:] = False
                outer[:, :max(0,x0-1)] = False
                outer[:, x1+1:] = False
                if not inner.any() or not outer.any():
                    continue
                # The positive stroke can be clearer in one channel, but
                # contrary ink outside its two-pixel optical fringe in any
                # channel vetoes that phase. A colored extra arm cannot hide
                # behind the green channel's otherwise complete K.
                conflicting_ink = False
                for other in range(3):
                    other_floor, other_ceiling = np.percentile(rgb[:, :, other], [20,95])
                    contrary = outer & (rgb[:, :, other].astype(float) >
                                        max(32, (other_floor+other_ceiling)/2))
                    if np.any(np.lib.stride_tricks.sliding_window_view(
                            contrary, (3,3)).sum(axis=(-1,-2)) >= 3):
                        conflicting_ink = True
                        break
                if conflicting_ink:
                    continue
                on = float(channel_level[inner].min())
                off = float(channel_level[outer].max())
                if on < max(45, threshold+8) or off > threshold-8:
                    continue
                # Erosion can erase a thin raster cell. Check every one of the
                # 35 on/off font-cell centers too, retaining every diagonal and
                # negative cell instead of accepting a surviving stem or blob.
                values = []
                for row in range(7):
                    for column in range(5):
                        xa, xb = (dx+round(x0+(column+t)*(x1-x0)/5) for t in (.3,.7))
                        ya, yb = (dy+round(y0+(row+t)*(y1-y0)/7) for t in (.3,.7))
                        values.append(float(np.median(channel_level[
                            ya:max(ya+1,yb), xa:max(xa+1,xb)])))
                values = np.array(values)
                if not np.array_equal(values > threshold, font.ravel()):
                    continue
                fits.append({"phase": [dx, dy], "on_min": on, "off_max": off,
                             "cell_on_min": float(values[font.ravel()].min()),
                             "cell_off_max": float(values[~font.ravel()].max())})
        detail["fits"] = fits
        if fits:
            accepted.append(name)
    diagnostic["channels"] = diagnostics
    if not accepted:
        return refuse("complete K shape and empty suffix region are not resolved")
    diagnostic.update({"accepted": True, "band": "K", "frequency": frequency,
                       "supporting_channels": accepted})
    return [("K", frequency)], diagnostic
