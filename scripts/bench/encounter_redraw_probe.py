"""Expectation-blind spatial measurements for narrow display redraw classifiers.

The ordinary encounter reader owns every single-frame field decision.  This
module only retains fixed spatial measurements needed to decide whether a run
of raw reader refusals is a coherent camera-captured redraw.  It receives no
timeline, expected value, packet data, or timestamp.
"""
from __future__ import annotations

import numpy as np


METHOD_VERSION = 1
MAIN_BAR_BOXES = tuple((900, y, 937, y + 10) for y in (400, 363, 326, 289, 251, 214))
MUTED_BADGE_BOX = (569, 216, 617, 228)
FREQUENCY_X_ORIGINS = (454, 520, 616, 688, 764)
FREQUENCY_SEGMENT_PATCHES = {
    "a": (18, 261, 41, 266),
    "b": (52, 280, 57, 293),
    "c": (51, 324, 56, 337),
    "d": (17, 350, 40, 355),
    "e": (7, 324, 12, 337),
    "f": (7, 280, 11, 293),
    "g": (18, 303, 41, 309),
}
FREQUENCY_HOLE_BOUNDS = ((278, 292), (322, 338))
FREQUENCY_DECIMAL_BOX = (590, 349, 599, 357)


class Pixels:
    """Apply the same fixed SCAN-landmark registration without reader coupling."""

    def __init__(self, rgb: bytes, width: int, height: int, registration: dict):
        self.image = np.frombuffer(rgb, dtype=np.uint8).reshape(height, width, 3)
        x1, y1, x2, y2 = registration["landmark_bounds"]
        self.scale = ((x2 - x1 + 1) / 220 * width / 1280,
                      (y2 - y1 + 1) / 79 * height / 720)
        self.anchor = (x1 * width / 960, y1 * height / 540)

    def crop(self, box):
        result = []
        for index, value in enumerate(box):
            axis = index % 2
            origin = (376 * 4 / 3, 192 * 4 / 3)[axis]
            result.append(round(self.anchor[axis] + (value - origin) * self.scale[axis]))
        x1, y1, x2, y2 = result
        if not 0 <= x1 < x2 <= self.image.shape[1] or not 0 <= y1 < y2 <= self.image.shape[0]:
            raise ValueError("registered redraw region leaves the image")
        return self.image[y1:y2, x1:x2]

    def level(self, box):
        return self.crop(box).max(axis=2).astype(float)


def _profile(level: np.ndarray, rows: int, columns: int) -> list[float]:
    if level.ndim != 2 or level.shape[0] < rows or level.shape[1] < columns:
        raise ValueError("redraw profile region is too small")
    return [round(float(np.median(cell)), 2)
            for row in np.array_split(level, rows, axis=0)
            for cell in np.array_split(row, columns, axis=1)]


def _fill(level: np.ndarray, on: float = 45, off: float = 32) -> dict:
    low, median, high = np.percentile(level, [10, 50, 90])
    state = "on" if low >= on else "off" if high <= off else "partial"
    return {
        "state": state,
        "p10": round(float(low), 2),
        "median": round(float(median), 2),
        "p90": round(float(high), 2),
    }


def observe(rgb: bytes, width: int, height: int, registration: dict) -> dict:
    """Measure fixed registered regions without reading or inferring content."""
    pixels = Pixels(rgb, width, height, registration)

    bars = []
    for box in MAIN_BAR_BOXES:
        level = pixels.level(box)
        bars.append({
            "box": list(box),
            "fill": _fill(level),
            "profile": _profile(level, 4, 4),
        })

    badge = pixels.level(MUTED_BADGE_BOX)
    frequency_cells = []
    for origin in FREQUENCY_X_ORIGINS:
        segments = {}
        for name, (x1, y1, x2, y2) in FREQUENCY_SEGMENT_PATCHES.items():
            segments[name] = _fill(pixels.level((origin + x1, y1, origin + x2, y2)))
        holes = [round(float(np.mean(
            pixels.level((origin + 28, top, origin + 40, bottom)) > 40)), 6)
            for top, bottom in FREQUENCY_HOLE_BOUNDS]
        frequency_cells.append({"origin": origin, "segments": segments,
                                "hole_ink_fractions": holes})

    return {
        "schema_version": 1,
        "method_version": METHOD_VERSION,
        "main_bars": {
            "profile_schema": {"rows": 4, "columns": 4,
                               "sample": "max-channel cell median"},
            "bars": bars,
        },
        "muted_badge": {
            "box": list(MUTED_BADGE_BOX),
            "lit_fraction": round(float(np.mean(badge > 45)), 6),
            "p95": round(float(np.percentile(badge, 95)), 2),
            "profile_schema": {"rows": 2, "columns": 4,
                               "sample": "max-channel cell median"},
            "profile": _profile(badge, 2, 4),
        },
        "primary_frequency": {
            "cells": frequency_cells,
            "decimal": _fill(pixels.level(FREQUENCY_DECIMAL_BOX)),
        },
    }
