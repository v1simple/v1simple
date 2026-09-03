"""Conservative orange seven-segment observation from registered RGB pixels.

The reader accepts no stimulus or expected values. Geometry is calibrated once
relative to the recorded SCAN landmark, independently on each axis. It does not
search for a transform that produces a recognized glyph. Refusals retain the
sampled source coordinates and segment measurements.
"""
from __future__ import annotations

import math
import statistics

from camera_contract import MAX_DISPLAY_SCALE, MIN_DISPLAY_SCALE

MASKS = {
    "abcdef": "0", "bc": "1", "abdeg": "2", "abcdg": "3",
    "bcfg": "4", "acdfg": "5", "acdefg": "6", "abc": "7",
    "abcdefg": "8", "abcdfg": "9", "abcefg": "A", "def": "L", "de": "l",
}
CHROMA_FLOOR = 30
MIN_CONTRAST = 40
OFF_MAX = 0.1
ON_MIN = 0.9

# Source-pixel calibration in units of SCAN landmark width/height, measured
# from its top-left corner. Coordinates describe interiors, not font templates.
PATCHES = {
    "a": (-.84772727, -.55769231, -.79318182, -.50961538),
    # Stay inside b's vertical stroke instead of sampling its antialiased edge.
    "b": (-.7625, -.44230769, -.75227273, -.32692308),
    "c": (-.77613636, -.125, -.75568182, .00961538),
    "d": (-.87159091, .07692308, -.78977273, .125),
    "e": (-.88522727, -.125, -.87159091, -.00961538),
    # Stay inside f's vertical stroke and clear its tapered end when sampling g.
    "f": (-.87159091, -.45192308, -.85795455, -.31730769),
    "g": (-.83409091, -.25961538, -.78636364, -.21153846),
}
BACKGROUND = (-.84090909, -.43269231, -.80681818, -.31730769)
CELL = (-.92954545, -.63461538, -.70454545, .22115385)
# Broad stroke envelopes cover tapered ends and antialiasing. They only reject
# substantial stray orange ink; their edges are not used to recognize a glyph.
ENVELOPES = (
    (-.88181818, -.63461538, -.73863636, -.42307692),
    (-.8, -.54807692, -.71818182, -.20192308),
    (-.80681818, -.25961538, -.725, .16346154),
    (-.92272727, -.04807692, -.73863636, .20192308),
    (-.92613636, -.25961538, -.82727273, .16346154),
    (-.9125, -.57692308, -.82045455, -.20192308),
    (-.89886364, -.34615385, -.74545455, -.125),
)


def _result(state: str, reason: str | None, **diagnostics) -> dict:
    glyph = diagnostics.pop("glyph", None)
    return {
        "state": state, "glyph": glyph, "reason": reason,
        "anomalies": [reason] if reason else [],
        "fields": {
            "count": {"state": state, "value": int(glyph) if glyph and glyph.isdigit() else None},
            "mode": {"state": state, "value": glyph if glyph in ("A", "L", "l") else None},
        },
        **diagnostics,
    }


def observe(rgb: bytes, width: int, height: int, registration: dict) -> dict:
    """Observe one whole RGB24 frame using its camera_preflight registration.

    A readable count implies no mode glyph and vice versa; unknown never means
    zero or absence. Insufficient contrast, partial sampled interiors, unknown
    masks, and detected registration/stray-ink problems are refused. These
    finite checks do not exclude every shift or damage outside the interiors.
    This is a count/mode instrument, not general display OCR.
    """
    if (type(width) is not int or type(height) is not int or width <= 0 or height <= 0
            or not isinstance(rgb, bytes) or len(rgb) != width * height * 3):
        return _result("unreadable", "invalid RGB24 frame dimensions or byte length",
                       alignment={}, segments={}, mask="")
    if not isinstance(registration, dict) or registration.get("result") != "PASS":
        return _result("unreadable", "missing successful camera registration",
                       alignment={}, segments={}, mask="")
    bounds = registration.get("landmark_bounds")
    transform = registration.get("transform")
    if (registration.get("normalized_still_size") != "960x540"
            or not isinstance(bounds, list) or len(bounds) != 4
            or any(type(v) is not int for v in bounds)
            or not 0 <= bounds[0] < bounds[2] < 960
            or not 0 <= bounds[1] < bounds[3] < 540
            or not isinstance(transform, dict)
            or transform.get("kind") != "dynamic_similarity"):
        return _result("unreadable", "unsupported camera registration geometry",
                       alignment={}, segments={}, mask="")
    landmark_width = bounds[2] - bounds[0] + 1
    landmark_height = bounds[3] - bounds[1] + 1
    scales = transform.get("scale_xy")
    actual_scales = (landmark_width / 138, landmark_height / 51)
    if (not isinstance(scales, list) or len(scales) != 2
            or any(type(v) not in (int, float) or not math.isfinite(v) for v in scales)
            or any(abs(a - b) > .000001 for a, b in zip(scales, actual_scales))):
        return _result("unreadable", "inconsistent landmark dimensions and scale_xy",
                       alignment={"landmark_bounds": bounds}, segments={}, mask="")
    if (not MIN_DISPLAY_SCALE <= math.sqrt(actual_scales[0] * actual_scales[1]) <= MAX_DISPLAY_SCALE
            or not .75 <= actual_scales[0] / actual_scales[1] <= 1.35):
        return _result("unreadable", "landmark geometry exceeds camera registration bounds",
                       alignment={"landmark_bounds": bounds}, segments={}, mask="")

    def mapped(box):
        return [round((bounds[i % 2] + value * (landmark_width if i % 2 == 0 else landmark_height))
                      * (width / 960 if i % 2 == 0 else height / 540))
                for i, value in enumerate(box)]

    boxes = {name: mapped(box) for name, box in PATCHES.items()}
    envelopes = [mapped(box) for box in ENVELOPES]
    background_box, cell_box = mapped(BACKGROUND), mapped(CELL)
    alignment = {"kind": "landmark_scale_xy", "landmark_bounds": bounds,
                 "scale_xy": list(actual_scales), "cell_xyxy": cell_box,
                 "patches_xyxy": boxes, "envelopes_xyxy": envelopes,
                 "background_xyxy": background_box}
    for box in [cell_box, background_box, *boxes.values(), *envelopes]:
        if not (0 <= box[0] < box[2] <= width and 0 <= box[1] < box[3] <= height):
            return _result("unreadable", "counter sampling region leaves the source frame",
                           alignment=alignment, segments={}, mask="")

    def scores(box):
        return [rgb[(y * width + x) * 3] - max(rgb[(y * width + x) * 3 + 1],
                                               rgb[(y * width + x) * 3 + 2])
                for y in range(box[1], box[3]) for x in range(box[0], box[2])]

    segments = {}
    for name, box in boxes.items():
        values = scores(box)
        segments[name] = {"active_ratio": sum(v > CHROMA_FLOOR for v in values) / len(values),
                          "minimum": min(values), "median": statistics.median(values),
                          "maximum": max(values)}
    background = statistics.median(scores(background_box))
    contrast = max(item["median"] for item in segments.values()) - background
    partial = [name for name, item in segments.items() if OFF_MAX < item["active_ratio"] < ON_MIN]
    mask = "".join(name for name, item in segments.items() if item["active_ratio"] >= ON_MIN)
    diagnostics = {"alignment": alignment, "segments": segments, "mask": mask,
                   "orange_contrast": contrast, "background_median": background}
    ink = [(x, y) for y in range(cell_box[1], cell_box[3]) for x in range(cell_box[0], cell_box[2])
           if rgb[(y * width + x) * 3] - max(rgb[(y * width + x) * 3 + 1],
                                           rgb[(y * width + x) * 3 + 2]) > CHROMA_FLOOR]
    outside = sum(not any(b[0] <= x < b[2] and b[1] <= y < b[3] for b in envelopes) for x, y in ink)
    border = sum(x in (cell_box[0], cell_box[2] - 1) or y in (cell_box[1], cell_box[3] - 1) for x, y in ink)
    diagnostics["cell"] = {"orange_pixels": len(ink), "outside_stroke_envelopes": outside,
                           "border_orange_pixels": border}
    if contrast < MIN_CONTRAST:
        return _result("unreadable", "insufficient orange contrast in the registered counter cell", **diagnostics)
    if partial:
        return _result("ambiguous", "partial segment interiors: " + ",".join(partial), **diagnostics)
    if border or (ink and outside / len(ink) > OFF_MAX):
        return _result("ambiguous", "counter ink leaves its registered stroke region", **diagnostics)
    if mask not in MASKS:
        return _result("ambiguous", "unknown canonical segment mask", **diagnostics)
    return _result("readable", None, glyph=MASKS[mask], **diagnostics)
