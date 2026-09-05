"""Expectation-blind optical profiles for the two fixed secondary text slots.

The ordinary encounter reader remains the sole owner of single-frame values.
This probe retains compact, fixed-position RGB evidence for a later temporal
comparison.  It receives no expected value, packet data, or timestamp and it
does not search, align, normalize, or retry after seeing the pixels.
"""
from __future__ import annotations

import base64
import hashlib

import numpy as np


METHOD_VERSION = 1
PROFILE_ROWS = 9
PROFILE_COLUMNS = 46
PROFILE_CHANNELS = ("red", "green", "blue")
TEXT_BOXES = (
    (440, 377, 621, 413),
    (687, 377, 868, 413),
)
PROFILE_BYTE_COUNT = PROFILE_ROWS * PROFILE_COLUMNS * len(PROFILE_CHANNELS)


class Pixels:
    """Apply the fixed SCAN-landmark registration without reader coupling."""

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
            raise ValueError("registered secondary text region leaves the image")
        return self.image[y1:y2, x1:x2]


def _profile(rgb: np.ndarray) -> bytes:
    if (rgb.ndim != 3 or rgb.shape[2] != 3
            or rgb.shape[0] < PROFILE_ROWS or rgb.shape[1] < PROFILE_COLUMNS):
        raise ValueError("secondary text profile region is too small")
    values = []
    for row in np.array_split(rgb.astype(float), PROFILE_ROWS, axis=0):
        for cell in np.array_split(row, PROFILE_COLUMNS, axis=1):
            values.extend(np.rint(np.mean(cell, axis=(0, 1))).astype(np.uint8).tolist())
    encoded = bytes(values)
    if len(encoded) != PROFILE_BYTE_COUNT:
        raise ValueError("secondary text profile has the wrong byte count")
    return encoded


def observe(rgb: bytes, width: int, height: int, registration: dict) -> dict:
    """Retain two fixed full-text profiles without assigning text identity."""
    pixels = Pixels(rgb, width, height, registration)
    cards = []
    for slot, box in enumerate(TEXT_BOXES):
        profile = _profile(pixels.crop(box))
        cards.append({
            "slot": slot,
            "reference_bounds": list(box),
            "profile_b64": base64.b64encode(profile).decode("ascii"),
            "profile_sha256": hashlib.sha256(profile).hexdigest(),
        })
    return {
        "schema_version": 1,
        "method_version": METHOD_VERSION,
        "profile_schema": {
            "rows": PROFILE_ROWS,
            "columns": PROFILE_COLUMNS,
            "channels": list(PROFILE_CHANNELS),
            "order": "row-major cells with RGB-interleaved uint8 components",
            "sample": "rounded arithmetic mean of registered RGB pixels",
            "normalization": "none",
        },
        "cards": cards,
    }
