#!/usr/bin/env python3
"""Synthetic self-tests for the display visual verifier."""

from __future__ import annotations

import copy
import contextlib
import io
import json
import re
import sys
import tempfile
from pathlib import Path
from unittest import mock

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import display_visual_check as dvc  # type: ignore  # noqa: E402

BACKGROUND = 0x0000
TEXT = 0xFFFF
GRAY = 0x8410
FREQUENCY = 0xEF7D
BOGEY = 0xAFE5
MUTED = 0x780F
LASER = 0xF800
KA = 0x07E0
K = 0xFFE0
X = 0x001F
FRONT = 0xF81F
SIDE = 0x07FF
REAR = 0xFD20
BLE = 0x051F
BLE_ADVERTISING = 0x7BEF
OBD = 0x05E0
OBD_ATTENTION = 0xF800
ALP_CONNECTED = 0x07E0
ALP_DLI = 0xFDA0
ALP_LID_ACTIVE = 0x051D
ALP_ALERT = 0xF81F
VOLUME = 0xFC00
VOLUME_MUTE = 0x780F

MAIN_RAMP = [0x1000, 0x1800, 0x2000, 0x2800, 0x3000, 0x3800, 0x4000, 0x4800]
CARD_RAMP = [0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700]
CARD_DIM_RAMP = [0x0100, 0x0180, 0x0200, 0x0280, 0x0300, 0x0380]
BINDING = (1, "test-fw", "test-sha", "0x00000001")


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def rect(x: int, y: int, w: int, h: int) -> dict[str, int]:
    return {"x": x, "y": y, "w": w, "h": h}


def color(value: int) -> str:
    return f"0x{value:04X}"


def base_steps() -> dict:
    return {
        "schemaVersion": BINDING[0],
        "firmwareVersion": BINDING[1],
        "firmwareSha": BINDING[2],
        "settingsFingerprint": BINDING[3],
        "manifest": "display-visual-steps",
        "stepCount": 1,
        "complete": True,
        "steps": [
            {
                "index": 0,
                "id": "000_ka_front_34700",
                "raw": {},
                "resolved": {
                    "primary": {
                        "present": True,
                        "band": "Ka",
                        "direction": "front",
                        "frequencyMHz": 34700,
                        "frequencyText": "34.700",
                        "frontBars": 4,
                        "rearBars": 0,
                        "cardBarCount": 0,
                    },
                    "secondary": {
                        "present": True,
                        "band": "K",
                        "direction": "side",
                        "frequencyMHz": 24150,
                        "frequencyText": "24.150",
                        "frontBars": 3,
                        "rearBars": 0,
                        "cardBarCount": 3,
                    },
                    "third": {
                        "present": False,
                        "band": "none",
                        "direction": "none",
                        "frequencyMHz": 0,
                        "frequencyText": "",
                        "frontBars": 0,
                        "rearBars": 0,
                        "cardBarCount": 0,
                    },
                    "activeBandMask": 6,
                    "activeDirectionMask": 3,
                    "flashMask": 0,
                    "bandFlashMask": 0,
                    "mainMeterCount": 5,
                    "alertCount": 2,
                    "muted": False,
                    "photo": False,
                    "frequencyRole": "frequency",
                    "status": {
                        "bogeyChar": "2",
                        "modeChar": "A",
                        "profileSlot": 0,
                        "alpState": 2,
                        "alpHbByte1": 3,
                        "obdState": 1,
                        "bleState": 2,
                        "mainVolume": 5,
                        "muteVolume": 0,
                        "muted": False,
                        "topCounterRole": "bogey",
                        "muteBadgeRole": "background",
                        "alpBadgeRole": "status.alpDli",
                        "obdBadgeRole": "status.obd",
                        "bleBadgeRole": "status.bleConnected",
                    },
                },
            }
        ],
    }


def complete_device_steps() -> dict:
    """Return a 44-step manifest whose first step matches known_good()."""
    payload = base_steps()
    template = payload["steps"][0]
    steps = [template]
    glyphs = tuple(PROTOCOL_BOGEY_SEGMENT_MASKS)
    numeric_frequencies = (
        "34.700",
        "35.500",
        "33.800",
        "24.150",
        "10.525",
        "24.199",
    )
    for index in range(1, dvc.DISPLAY_VISUAL_EXPECTED_STEP_COUNT):
        step = copy.deepcopy(template)
        step["index"] = index
        step["id"] = f"{index:03d}_protocol_corpus"
        step["resolved"]["status"]["bogeyChar"] = glyphs[(index - 1) % len(glyphs)]
        step["resolved"]["primary"]["frequencyText"] = (
            "LASER"
            if index in dvc.DISPLAY_VISUAL_EXPECTED_ALPHA_FREQUENCY_STEP_INDICES
            else numeric_frequencies[index % len(numeric_frequencies)]
        )
        steps.append(step)
    payload["steps"] = steps
    payload["stepCount"] = len(steps)
    return payload


def base_layout() -> dict:
    return {
        "schemaVersion": BINDING[0],
        "firmwareVersion": BINDING[1],
        "firmwareSha": BINDING[2],
        "settingsFingerprint": BINDING[3],
        "manifest": "display-visual-layout",
        "screen": {
            "logical": {"width": dvc.LOGICAL_WIDTH, "height": dvc.LOGICAL_HEIGHT},
            "raw": {
                "width": dvc.RAW_WIDTH,
                "height": dvc.RAW_HEIGHT,
                "format": dvc.FRAMEBUFFER_FORMAT,
                "transform": dvc.FRAMEBUFFER_TRANSFORM,
            },
        },
        "elements": {
            "bandCells": [
                {
                    "index": 0,
                    "label": "L",
                    "band": "laser",
                    "bandMask": 1,
                    "rect": rect(10, 10, 12, 8),
                    "activeRole": "bands.laser",
                    "inactiveRole": "gray",
                    "minCoverage": 0.01,
                    "inactiveMinCoverage": 0.01,
                },
                {
                    "index": 1,
                    "label": "Ka",
                    "band": "ka",
                    "bandMask": 2,
                    "rect": rect(10, 22, 12, 8),
                    "activeRole": "bands.ka",
                    "inactiveRole": "gray",
                    "minCoverage": 0.01,
                    "inactiveMinCoverage": 0.01,
                },
                {
                    "index": 2,
                    "label": "K",
                    "band": "k",
                    "bandMask": 4 | 16,
                    "rect": rect(10, 34, 12, 8),
                    "activeRole": "bands.k",
                    "inactiveRole": "gray",
                    "minCoverage": 0.01,
                    "inactiveMinCoverage": 0.01,
                },
                {
                    "index": 3,
                    "label": "X",
                    "band": "x",
                    "bandMask": 8,
                    "rect": rect(10, 46, 12, 8),
                    "activeRole": "bands.x",
                    "inactiveRole": "gray",
                    "minCoverage": 0.01,
                    "inactiveMinCoverage": 0.01,
                },
            ],
            "directionArrows": [
                {
                    "direction": "front",
                    "directionMask": 1,
                    "rect": rect(45, 10, 10, 10),
                    "activeRole": "arrows.front",
                    "inactiveRole": "gray",
                },
                {
                    "direction": "side",
                    "directionMask": 2,
                    "rect": rect(45, 24, 10, 10),
                    "activeRole": "arrows.side",
                    "inactiveRole": "gray",
                },
                {
                    "direction": "rear",
                    "directionMask": 4,
                    "rect": rect(45, 38, 10, 10),
                    "activeRole": "arrows.rear",
                    "inactiveRole": "gray",
                },
            ],
            "mainSignalBars": [
                {
                    "index": index,
                    "rect": rect(585, 10 + index * 6, 20, 4),
                    "inactiveRole": "gray",
                }
                for index in range(8)
            ],
            "frequency": {
                "rect": rect(145, 0, 180, 50),
                "textRole": "text",
                "roleSource": "frequencyRole",
                "minCoverage": 0.02,
                "maxCoverage": 0.70,
            },
            "cardSlots": [
                {
                    "slot": 0,
                    "rect": rect(145, 62, 70, 18),
                    "emptyRect": rect(140, 58, 80, 42),
                    "textRole": "text",
                    "minCoverage": 0.08,
                    "maxCoverage": 0.40,
                },
                {
                    "slot": 1,
                    "rect": rect(245, 62, 70, 18),
                    "emptyRect": rect(240, 58, 80, 42),
                    "textRole": "text",
                    "minCoverage": 0.08,
                    "maxCoverage": 0.40,
                },
            ],
            "cardMeterBars": [
                {
                    "slot": slot,
                    "index": index,
                    # The renderer fills active bars and outlines inactive bars.
                    # This shared one-pixel top edge truthfully covers both states.
                    "rect": rect(145 + slot * 100 + index * 9, 88, 6, 1),
                    "activeRole": f"cardMeterRamp.{index}",
                    "inactiveRole": f"cardMeterDimRamp.{index}",
                }
                for slot in range(2)
                for index in range(6)
            ],
            "statusText": [
                {
                    "id": "topCounter",
                    "source": "bogeyChar",
                    "rect": rect(130, 103, 55, 68),
                    "textRole": "bogey",
                    "roleSource": "topCounterRole",
                    "minCoverage": 0.02,
                    "maxCoverage": 0.80,
                },
                {
                    "id": "volumeMain",
                    "source": "mainVolume",
                    "rect": rect(30, 150, 16, 10),
                    "textRole": "status.volumeMain",
                    "minCoverage": 0.10,
                    "maxCoverage": 0.50,
                },
                {
                    "id": "volumeMute",
                    "source": "muteVolume",
                    "rect": rect(50, 150, 16, 10),
                    "textRole": "status.volumeMute",
                    "minCoverage": 0.10,
                    "maxCoverage": 0.50,
                },
            ],
            "statusBadges": [
                {
                    "id": "mute",
                    "source": "muted",
                    "rect": rect(72, 150, 8, 8),
                    "coverageRect": rect(70, 148, 12, 12),
                    "roleSource": "muteBadgeRole",
                    "activeRole": "muted",
                    "inactiveRole": "background",
                    "activeWhen": "nonzero",
                },
                {
                    "id": "alp",
                    "source": "alpState",
                    "rect": rect(86, 150, 8, 8),
                    "roleSource": "alpBadgeRole",
                    "activeRole": "status.alpConnected",
                    "inactiveRole": "background",
                    "activeWhen": "nonzero",
                    "minCoverage": 0.10,
                },
                {
                    "id": "obd",
                    "source": "obdState",
                    "rect": rect(100, 150, 8, 8),
                    "roleSource": "obdBadgeRole",
                    "activeRole": "status.obd",
                    "inactiveRole": "background",
                    "activeWhen": "nonzero",
                    "minCoverage": 0.10,
                },
                {
                    "id": "ble",
                    "source": "bleState",
                    "rect": rect(114, 150, 8, 8),
                    "roleSource": "bleBadgeRole",
                    "activeRole": "status.bleAdvertising",
                    "inactiveRole": "background",
                    "activeWhen": "nonzero",
                    "minCoverage": 0.10,
                },
            ],
        },
        "palette": {
            "background": color(BACKGROUND),
            "text": color(TEXT),
            "gray": color(GRAY),
            "frequency": color(FREQUENCY),
            "bogey": color(BOGEY),
            "muted": color(MUTED),
            "persisted": color(0x4208),
            "bands": {
                "laser": color(LASER),
                "ka": color(KA),
                "k": color(K),
                "x": color(X),
                "photo": color(0xF81F),
            },
            "arrows": {
                "front": color(FRONT),
                "side": color(SIDE),
                "rear": color(REAR),
            },
            "mainMeterRamp": [color(value) for value in MAIN_RAMP],
            "cardMeterRamp": [color(value) for value in CARD_RAMP],
            "cardMeterDimRamp": [color(value) for value in CARD_DIM_RAMP],
            "status": {
                "bleConnected": color(BLE),
                "bleDisconnected": color(GRAY),
                "bleAdvertising": color(BLE_ADVERTISING),
                "obd": color(OBD),
                "obdAttention": color(OBD_ATTENTION),
                "alpConnected": color(ALP_CONNECTED),
                "alpDli": color(ALP_DLI),
                "alpLidActive": color(ALP_LID_ACTIVE),
                "alpAlert": color(ALP_ALERT),
                "volumeMain": color(VOLUME),
                "volumeMute": color(VOLUME_MUTE),
            },
        },
        "masks": [],
        "complete": True,
    }


def empty_frame() -> dvc.Framebuffer:
    return dvc.Framebuffer(
        width=dvc.LOGICAL_WIDTH,
        height=dvc.LOGICAL_HEIGHT,
        pixels=[BACKGROUND] * (dvc.LOGICAL_WIDTH * dvc.LOGICAL_HEIGHT),
    )


def fill(frame: dvc.Framebuffer, raw_rect: dict[str, int], value: int) -> None:
    parsed = dvc.parse_rect(raw_rect, "test.rect")
    for y in range(parsed.y, parsed.y + parsed.h):
        for x in range(parsed.x, parsed.x + parsed.w):
            frame.set_pixel(x, y, value)


def fill_coverage(frame: dvc.Framebuffer, raw_rect: dict[str, int], value: int, coverage: float) -> None:
    parsed = dvc.parse_rect(raw_rect, "test.rect")
    target = int(parsed.w * parsed.h * coverage)
    painted = 0
    for y in range(parsed.y, parsed.y + parsed.h):
        for x in range(parsed.x, parsed.x + parsed.w):
            if painted >= target:
                return
            frame.set_pixel(x, y, value)
            painted += 1


# Independent protocol oracle. These are the literal seven-bit bogey-image
# values accepted by packet_parser.cpp (bit 0=A through bit 6=G), not a
# reversal of the verifier's SEG7_PATTERNS table. Keeping the fixture on the
# producer/protocol side means a decoder-table regression cannot make both the
# implementation and its test fixture agree on the same wrong value.
PROTOCOL_BOGEY_SEGMENT_MASKS = {
    "0": 0x3F,
    "1": 0x06,
    "2": 0x5B,
    "3": 0x4F,
    "4": 0x66,
    "5": 0x6D,
    "6": 0x7D,
    "7": 0x07,
    "8": 0x7F,
    "9": 0x6F,
    "&": 0x18,
    "u": 0x1C,
    "J": 0x1E,
    "L": 0x38,
    "C": 0x39,
    "U": 0x3E,
    "#": 0x49,
    "c": 0x58,
    "d": 0x5E,
    "F": 0x71,
    "P": 0x73,
    "A": 0x77,
    "E": 0x79,
    "b": 0x7C,
}
PACKET_PARSER_BOGEY_CHARS = frozenset("0123456789&uJLCU#cdFPAEb")
SEG7_CORRUPTION_SHIFTS = tuple(
    sorted(
        {
            *((x, 0) for x in range(-5, 6)),
            *((0, y) for y in range(-5, 6)),
            *((x, y) for x in range(-1, 2) for y in range(-1, 2)),
        }
    )
)


def draw_seg7_mask(
    frame: dvc.Framebuffer,
    raw_rect: dict[str, int],
    segment_mask: int,
    color: int,
    *,
    single_cell: bool = False,
    x_shift: int = 0,
    y_shift: int = 0,
) -> None:
    """Render one independently specified A..G bit mask."""
    rect = dvc.parse_rect(raw_rect, "test.seg7.rect")
    if single_cell:
        available_h = round(
            rect.h * dvc.SEG7_COUNTER_CELL_HEIGHT / dvc.SEG7_COUNTER_RECT_HEIGHT
        )
        top = rect.y + round(
            rect.h * dvc.SEG7_COUNTER_CELL_TOP / dvc.SEG7_COUNTER_RECT_HEIGHT
        ) + y_shift
        cell_width = round(
            rect.w * dvc.SEG7_COUNTER_CELL_WIDTH / dvc.SEG7_COUNTER_RECT_WIDTH
        )
        t = max(3, round(available_h * 0.12))
        segment_len = max(2, (available_h - 3 * t) // 2)
        x = (
            rect.x
            + round(
                rect.w * dvc.SEG7_COUNTER_CELL_LEFT / dvc.SEG7_COUNTER_RECT_WIDTH
            )
            + x_shift
        )
    else:
        available_h = rect.h - 2
        top = rect.y + 1 + y_shift
        t = max(3, round(available_h * 0.15))
        segment_len = max(2, (available_h - 3 * t) // 2)
        cell_width = segment_len + 2 * t
        x = rect.x + 3 + x_shift

    def box(x0: int, y0: int, w: int, h: int) -> None:
        for yy in range(y0, y0 + h):
            for xx in range(x0, x0 + w):
                frame.set_pixel(xx, yy, color)

    a, b, c, d, e, f, g = tuple(
        bool(segment_mask & (1 << index)) for index in range(7)
    )
    if single_cell and segment_mask == PROTOCOL_BOGEY_SEGMENT_MASKS["1"]:
        # V1SevenX's B/C strokes occupy the full qualified glyph height while
        # retaining a real midpoint gap. This keeps the independent mask
        # fixture faithful to the physical cell's geometry as well as its bits.
        upper_height = max(2, available_h // 2 - 3)
        lower_top = top + available_h // 2
        lower_height = max(2, available_h - available_h // 2 - 3)
        box(x + cell_width - t - 2, top, t + 2, upper_height)
        box(x + cell_width - t - 2, lower_top, t + 2, lower_height)
        return
    if a:
        if single_cell:
            box(x + 3, top, cell_width - 4, t)
        else:
            box(x, top, cell_width, t)
    if b:
        if single_cell:
            box(x + cell_width - t, top, t, segment_len + t)
        else:
            box(x + cell_width - t, top + t, t, segment_len)
    if c:
        if single_cell:
            box(
                x + cell_width - t - 2,
                top + available_h // 2,
                t + 1,
                segment_len + t,
            )
        else:
            box(x + cell_width - t, top + segment_len + 2 * t, t, segment_len)
    if d:
        box(
            x,
            top + 2 * segment_len + 2 * t,
            cell_width - 3 if single_cell else cell_width,
            t,
        )
    if e:
        if single_cell:
            box(x, top + available_h // 2, t, segment_len + t)
        else:
            box(x, top + segment_len + 2 * t, t, segment_len)
    if f:
        if single_cell:
            box(x + 1, top + t // 2, t, segment_len + t)
        else:
            box(x, top + t, t, segment_len)
    if g:
        box(
            x,
            top + segment_len + t,
            cell_width - 3 if single_cell else cell_width,
            t,
        )


def draw_seg7_text(
    frame: dvc.Framebuffer,
    raw_rect: dict[str, int],
    text: str,
    color: int,
    *,
    single_cell: bool = False,
) -> None:
    """Render decoder-compatible 7-segment text inside rect.

    Geometry mirrors the renderer contract: segment thickness max(3, 15% of
    glyph height), glyph width 55% of height, dots as small squares, 2-px
    inter-glyph gaps, and a 3-px left margin so runs never touch the rect edge
    (edge ink is treated as declared-overlap foreign ink). Segment membership
    comes only from the fixed V1 protocol masks above.
    """
    rect = dvc.parse_rect(raw_rect, "test.seg7.rect")
    if single_cell:
        assert len(text) == 1 and text != ".", "single-cell fixture expects one symbol"
        draw_seg7_mask(
            frame,
            raw_rect,
            PROTOCOL_BOGEY_SEGMENT_MASKS[text],
            color,
            single_cell=True,
        )
        return

    gh = rect.h - 2
    gw = max(5, int(gh * 0.55))
    t = max(3, round(gh * 0.15))
    dot = max(2, gh // 4)
    x = rect.x + 3
    top = rect.y + 1

    def box(x0: int, y0: int, w: int, h: int) -> None:
        for yy in range(y0, y0 + h):
            for xx in range(x0, x0 + w):
                frame.set_pixel(xx, yy, color)

    for char in text:
        if char == ".":
            box(x, top + gh - dot, dot, dot)
            x += dot + 2
            continue
        if char == "1":
            # Model V1SevenX's distinct B/C polygons, including the blank
            # midpoint row used by the production narrow-glyph invariant.
            narrow = max(3, t)
            upper_h = max(2, gh // 2 - 1)
            lower_y = top + gh // 2 + 1
            lower_h = max(2, top + gh - lower_y)
            box(x, top, narrow, upper_h)
            box(x, lower_y, narrow, lower_h)
            x += narrow + 2
            continue
        segment_mask = PROTOCOL_BOGEY_SEGMENT_MASKS[char]
        a, b, c, d, e, f, g = tuple(
            bool(segment_mask & (1 << index)) for index in range(7)
        )
        if a:
            box(x, top, gw, t)
        if b:
            box(x + gw - t, top, t, gh // 2)
        if c:
            box(x + gw - t, top + gh // 2, t, gh - gh // 2)
        if d:
            box(x, top + gh - t, gw, t)
        if e:
            box(x, top + gh // 2, t, gh - gh // 2)
        if f:
            box(x, top, t, gh // 2)
        if g:
            box(x, top + (gh - t) // 2, gw, t)
        x += gw + 2
    assert x <= rect.x + rect.w - 1, f"seg7 text {text!r} overflows rect {raw_rect}"


def draw_qualified_frequency_text(
    frame: dvc.Framebuffer,
    raw_rect: dict[str, int],
    text: str,
    color: int,
) -> None:
    """Render decoder-compatible text into independent physical-lane bounds."""
    assert len(text) == 6 and text[2] == "." and text.replace(".", "").isdigit()
    rect = dvc.parse_rect(raw_rect, "test.qualified_frequency")
    origins = {0: 33, 1: 74, 3: 134, 4: 181, 5: 229}
    digit_bounds = {
        "0": (0, 16, 40, 80),
        "1": (30, 17, 40, 76),
        "2": (0, 16, 40, 80),
        "3": (0, 16, 40, 80),
        "4": (2, 17, 40, 76),
        "5": (0, 16, 38, 80),
        "6": (0, 16, 38, 80),
        "7": (4, 16, 40, 76),
        "8": (0, 16, 40, 80),
        "9": (0, 16, 40, 80),
    }

    def scaled(bounds: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
        x0, y0, x1, y1 = bounds
        return (
            rect.x + round(x0 * rect.w / 305),
            rect.y + round(y0 * rect.h / 85),
            rect.x + round((x1 + 1) * rect.w / 305) - 1,
            rect.y + round((y1 + 1) * rect.h / 85) - 1,
        )

    def box(x: int, y: int, width: int, height: int) -> None:
        for yy in range(y, y + height):
            for xx in range(x, x + width):
                frame.set_pixel(xx, yy, color)

    fill(frame, raw_rect, BACKGROUND)
    for index, char in enumerate(text):
        if index == 2:
            x0, y0, x1, y1 = scaled((117, 72, 126, 81))
            box(x0, y0, x1 - x0 + 1, y1 - y0 + 1)
            continue
        rel_x0, y0, rel_x1, y1 = digit_bounds[char]
        origin = origins[index]
        x0, y0, x1, y1 = scaled((origin + rel_x0, y0, origin + rel_x1, y1))
        width = x1 - x0 + 1
        height = y1 - y0 + 1
        thickness = max(3, round(min(width, height) * 0.18))
        half = height // 2
        mask = PROTOCOL_BOGEY_SEGMENT_MASKS[char]
        a, b, c, d, e, f, g = tuple(bool(mask & (1 << bit)) for bit in range(7))
        if char == "1":
            upper_height = max(2, half - 1)
            lower_y = y0 + half + 1
            box(x0, y0, width, upper_height)
            box(x0, lower_y, width, max(2, y1 - lower_y + 1))
            continue
        if a:
            box(x0, y0, width, thickness)
        if b:
            box(x1 - thickness + 1, y0, thickness, half)
        if c:
            box(x1 - thickness + 1, y0 + half, thickness, height - half)
        if d:
            box(x0, y1 - thickness + 1, width, thickness)
        if e:
            box(x0, y0 + half, thickness, height - half)
        if f:
            box(x0, y0, thickness, half)
        if g:
            box(x0, y0 + (height - thickness) // 2, width, thickness)


def known_good() -> tuple[dict, dict, dvc.Framebuffer]:
    layout = base_layout()
    steps = base_steps()
    frame = empty_frame()

    for cell in layout["elements"]["bandCells"]:
        active = cell["band"] in {"ka", "k"}
        role_color = {"laser": LASER, "ka": KA, "k": K, "x": X}[cell["band"]]
        fill(frame, cell["rect"], role_color if active else GRAY)

    for arrow in layout["elements"]["directionArrows"]:
        active = arrow["direction"] in {"front", "side"}
        role_color = {"front": FRONT, "side": SIDE, "rear": REAR}[arrow["direction"]]
        fill(frame, arrow["rect"], role_color if active else GRAY)

    for bar in layout["elements"]["mainSignalBars"]:
        value = MAIN_RAMP[bar["index"]] if bar["index"] < 5 else GRAY
        fill(frame, bar["rect"], value)

    draw_qualified_frequency_text(
        frame,
        layout["elements"]["frequency"]["rect"],
        "34.700",
        FREQUENCY,
    )
    fill_coverage(frame, layout["elements"]["cardSlots"][0]["rect"], TEXT, 0.20)
    for meter in layout["elements"]["cardMeterBars"]:
        value = BACKGROUND
        if meter["slot"] == 0:
            value = (
                CARD_RAMP[meter["index"]]
                if meter["index"] < 3
                else CARD_DIM_RAMP[meter["index"]]
            )
        fill(frame, meter["rect"], value)

    draw_seg7_text(
        frame,
        layout["elements"]["statusText"][0]["rect"],
        "2",
        BOGEY,
        single_cell=True,
    )
    for status_text, value in zip(layout["elements"]["statusText"][1:], [VOLUME, VOLUME_MUTE]):
        fill_coverage(frame, status_text["rect"], value, 0.25)

    badge_colors = [BACKGROUND, ALP_DLI, OBD, BLE]
    for badge, value in zip(layout["elements"]["statusBadges"], badge_colors):
        fill(frame, badge["rect"], value)
    return steps, layout, frame


def muted_known_good() -> tuple[dict, dict, dvc.Framebuffer]:
    steps, layout, frame = known_good()
    resolved = steps["steps"][0]["resolved"]
    resolved["muted"] = True
    resolved["frequencyRole"] = "muted"
    resolved["status"]["muted"] = True
    resolved["status"]["muteBadgeRole"] = "muted"

    for cell in layout["elements"]["bandCells"]:
        if cell["band"] in {"ka", "k"}:
            fill(frame, cell["rect"], MUTED)
    for arrow in layout["elements"]["directionArrows"]:
        if arrow["direction"] in {"front", "side"}:
            fill(frame, arrow["rect"], MUTED)
    for bar in layout["elements"]["mainSignalBars"]:
        if bar["index"] < 5:
            fill(frame, bar["rect"], MUTED)

    frequency = layout["elements"]["frequency"]
    fill(frame, frequency["rect"], BACKGROUND)
    draw_qualified_frequency_text(frame, frequency["rect"], "34.700", MUTED)
    card = layout["elements"]["cardSlots"][0]
    fill(frame, card["rect"], BACKGROUND)
    fill_coverage(frame, card["rect"], MUTED, 0.20)
    for meter in layout["elements"]["cardMeterBars"]:
        if meter["slot"] == 0 and meter["index"] < 3:
            fill(frame, meter["rect"], MUTED)
    fill(frame, layout["elements"]["statusBadges"][0]["rect"], MUTED)
    return steps, layout, frame


def validated_known_good() -> tuple[dict, dict, dvc.Framebuffer]:
    steps, layout, frame = known_good()
    dvc.validate_steps_manifest(steps)
    dvc.validate_layout_manifest(layout)
    dvc.validate_manifest_binding(steps, layout)
    return steps, layout, frame


def low_single_alert_step() -> dict:
    step = copy.deepcopy(base_steps()["steps"][0])
    step["index"] = 1
    step["id"] = "001_ka_front_low_single"
    resolved = step["resolved"]
    resolved["secondary"]["present"] = False
    resolved["secondary"]["band"] = "none"
    resolved["secondary"]["direction"] = "none"
    resolved["secondary"]["frequencyMHz"] = 0
    resolved["secondary"]["frequencyText"] = ""
    resolved["secondary"]["frontBars"] = 0
    resolved["secondary"]["cardBarCount"] = 0
    resolved["activeBandMask"] = 2
    resolved["activeDirectionMask"] = 1
    resolved["mainMeterCount"] = 2
    resolved["alertCount"] = 1
    resolved["status"]["bogeyChar"] = "1"
    resolved["status"]["alpState"] = 0
    resolved["status"]["alpHbByte1"] = 0
    resolved["status"]["bleState"] = 0
    resolved["status"]["obdState"] = 0
    resolved["status"]["alpBadgeRole"] = "background"
    resolved["status"]["obdBadgeRole"] = "background"
    resolved["status"]["bleBadgeRole"] = "background"
    return step


def transition_fixture() -> tuple[dict, dict, dvc.Framebuffer, dvc.Framebuffer]:
    steps, layout, source_frame = known_good()
    target_step = low_single_alert_step()
    steps["steps"].append(target_step)
    steps["stepCount"] = 2
    target_frame = empty_frame()

    for cell in layout["elements"]["bandCells"]:
        active = cell["band"] == "ka"
        role_color = {"laser": LASER, "ka": KA, "k": K, "x": X}[cell["band"]]
        fill(target_frame, cell["rect"], role_color if active else GRAY)

    for arrow in layout["elements"]["directionArrows"]:
        active = arrow["direction"] == "front"
        role_color = {"front": FRONT, "side": SIDE, "rear": REAR}[arrow["direction"]]
        fill(target_frame, arrow["rect"], role_color if active else GRAY)

    for bar in layout["elements"]["mainSignalBars"]:
        value = MAIN_RAMP[bar["index"]] if bar["index"] < 2 else GRAY
        fill(target_frame, bar["rect"], value)

    draw_qualified_frequency_text(
        target_frame,
        layout["elements"]["frequency"]["rect"],
        "34.700",
        FREQUENCY,
    )
    for slot in layout["elements"]["cardSlots"]:
        fill(target_frame, slot["emptyRect"], BACKGROUND)
    for meter in layout["elements"]["cardMeterBars"]:
        fill(target_frame, meter["rect"], BACKGROUND)

    draw_seg7_text(
        target_frame,
        layout["elements"]["statusText"][0]["rect"],
        "1",
        BOGEY,
        single_cell=True,
    )
    for status_text, value in zip(layout["elements"]["statusText"][1:], [VOLUME, VOLUME_MUTE]):
        fill_coverage(target_frame, status_text["rect"], value, 0.25)
    for badge in layout["elements"]["statusBadges"]:
        fill(target_frame, badge["rect"], BACKGROUND)

    dvc.validate_steps_manifest(steps)
    dvc.validate_layout_manifest(layout)
    dvc.validate_manifest_binding(steps, layout)
    return steps, layout, source_frame, target_frame


def verify(steps: dict, layout: dict, frame: dvc.Framebuffer) -> dvc.VerificationSummary:
    return dvc.verify_step(frame, layout, steps["steps"][0])


def assert_failure_contains(summary: dvc.VerificationSummary, needle: str) -> None:
    haystack = "\n".join(f"{failure.element}: {failure.message}" for failure in summary.failures)
    assert_true(summary.result == "FAIL", f"expected failure, got {summary.to_json()}")
    assert_true(needle in haystack, f"expected failure containing {needle!r}, got:\n{haystack}")


def corrupt(mutator) -> dvc.VerificationSummary:
    steps, layout, frame = validated_known_good()
    mutator(steps, layout, frame)
    return verify(steps, layout, frame)


def test_known_good_frame_passes() -> None:
    steps, layout, frame = validated_known_good()
    summary = verify(steps, layout, frame)
    assert_true(summary.result == "PASS", f"known-good frame failed: {summary.to_json()}")
    assert_true(summary.assertion_count > 0, "assertions should be counted")
    assert_true(summary.checked_regions > 0, "checked regions should be counted")
    assert_true(summary.checked_pixels > 0, "checked pixels should be counted")


def test_fixture_matches_renderer_contract() -> None:
    steps = base_steps()
    layout = base_layout()
    resolved = steps["steps"][0]["resolved"]
    status = resolved["status"]
    assert_true(resolved["frequencyRole"] == "frequency", resolved)
    for role_field in (
        "topCounterRole",
        "muteBadgeRole",
        "alpBadgeRole",
        "obdBadgeRole",
        "bleBadgeRole",
    ):
        assert_true(bool(status[role_field]), f"missing resolved status role {role_field}")

    elements = layout["elements"]
    assert_true(elements["frequency"]["roleSource"] == "frequencyRole", elements["frequency"])
    assert_true(elements["statusText"][0]["roleSource"] == "topCounterRole", elements["statusText"][0])
    badges = elements["statusBadges"]
    assert_true([badge["id"] for badge in badges] == ["mute", "alp", "obd", "ble"], badges)
    assert_true(
        [badge["roleSource"] for badge in badges]
        == ["muteBadgeRole", "alpBadgeRole", "obdBadgeRole", "bleBadgeRole"],
        badges,
    )
    assert_true(bool(badges[0].get("coverageRect")), "mute badge must expose its full clear region")
    assert_true(all(slot.get("emptyRect") for slot in elements["cardSlots"]), elements["cardSlots"])
    assert_true(all(meter["rect"]["h"] == 1 for meter in elements["cardMeterBars"]), elements["cardMeterBars"])


def test_ku_activates_the_shared_k_cell() -> None:
    steps, layout, frame = validated_known_good()
    resolved = steps["steps"][0]["resolved"]
    resolved["secondary"]["band"] = "Ku"
    resolved["activeBandMask"] = 2 | 16
    summary = verify(steps, layout, frame)
    assert_true(summary.result == "PASS", f"Ku shared K-cell contract failed: {summary.to_json()}")


def test_resolved_dynamic_role_fields_are_required() -> None:
    paths = (
        ("frequencyRole",),
        ("status", "topCounterRole"),
        ("status", "muteBadgeRole"),
        ("status", "alpBadgeRole"),
        ("status", "obdBadgeRole"),
        ("status", "bleBadgeRole"),
    )
    for path in paths:
        steps = base_steps()
        owner = steps["steps"][0]["resolved"]
        for key in path[:-1]:
            owner = owner[key]
        del owner[path[-1]]
        try:
            dvc.validate_steps_manifest(steps)
        except dvc.ProtocolError as exc:
            assert_true(path[-1] in str(exc), str(exc))
            continue
        raise AssertionError(f"missing {'.'.join(path)} should fail manifest validation")


def test_renderer_faithful_muted_frame_passes_and_normal_colors_fail() -> None:
    steps, layout, frame = muted_known_good()
    dvc.validate_steps_manifest(steps)
    dvc.validate_layout_manifest(layout)
    dvc.validate_manifest_binding(steps, layout)
    summary = verify(steps, layout, frame)
    assert_true(summary.result == "PASS", f"renderer-faithful muted frame failed: {summary.to_json()}")

    normal_band = copy.deepcopy(frame)
    fill(normal_band, layout["elements"]["bandCells"][1]["rect"], KA)
    assert_failure_contains(verify(steps, layout, normal_band), "band_cells.ka")

    normal_frequency = copy.deepcopy(frame)
    frequency_rect = layout["elements"]["frequency"]["rect"]
    fill(normal_frequency, frequency_rect, BACKGROUND)
    fill_coverage(normal_frequency, frequency_rect, FREQUENCY, 0.20)
    assert_failure_contains(verify(steps, layout, normal_frequency), "frequency")


def test_dynamic_badge_roles_drive_verification() -> None:
    steps, layout, frame = validated_known_good()
    status = steps["steps"][0]["resolved"]["status"]
    status["alpHbByte1"] = 4
    status["alpBadgeRole"] = "status.alpLidActive"
    status["obdState"] = 2
    status["obdBadgeRole"] = "status.obdAttention"
    status["bleState"] = 1
    status["bleBadgeRole"] = "status.bleAdvertising"

    badges = {badge["id"]: badge for badge in layout["elements"]["statusBadges"]}
    fill(frame, badges["alp"]["rect"], ALP_LID_ACTIVE)
    fill(frame, badges["obd"]["rect"], OBD_ATTENTION)
    fill(frame, badges["ble"]["rect"], BLE_ADVERTISING)
    summary = verify(steps, layout, frame)
    assert_true(summary.result == "PASS", f"dynamic badge roles failed: {summary.to_json()}")

    static_color = copy.deepcopy(frame)
    fill(static_color, badges["alp"]["rect"], ALP_CONNECTED)
    assert_failure_contains(verify(steps, layout, static_color), "status.alp")

    missing_role_source = copy.deepcopy(layout)
    del missing_role_source["elements"]["statusBadges"][1]["roleSource"]
    assert_failure_contains(verify(steps, missing_role_source, frame), "status.alp")

    corrupt_role_source = copy.deepcopy(layout)
    corrupt_role_source["elements"]["statusBadges"][1]["roleSource"] = "missingBadgeRole"
    try:
        verify(steps, corrupt_role_source, frame)
    except dvc.ProtocolError as exc:
        assert_true("missingBadgeRole" in str(exc), str(exc))
    else:
        raise AssertionError("unknown status badge roleSource should fail closed")


def test_card_meter_present_inactive_and_absent_are_distinct() -> None:
    steps, layout, frame = validated_known_good()
    meters = layout["elements"]["cardMeterBars"]
    present_active = next(meter for meter in meters if meter["slot"] == 0 and meter["index"] == 2)
    present_inactive = next(meter for meter in meters if meter["slot"] == 0 and meter["index"] == 4)
    absent = next(meter for meter in meters if meter["slot"] == 1 and meter["index"] == 4)
    active_rect = dvc.parse_rect(present_active["rect"], "present_active")
    present_rect = dvc.parse_rect(present_inactive["rect"], "present_inactive")
    absent_rect = dvc.parse_rect(absent["rect"], "absent")
    assert_true(
        active_rect.h == 1 and present_rect.h == 1 and absent_rect.h == 1,
        "card meter probes must be one-pixel strips",
    )
    assert_true(frame.pixel(active_rect.x, active_rect.y) == CARD_RAMP[2], "present active bar must be filled")
    assert_true(frame.pixel(present_rect.x, present_rect.y) == CARD_DIM_RAMP[4], "present inactive bar must be dim")
    assert_true(frame.pixel(absent_rect.x, absent_rect.y) == BACKGROUND, "absent-card bar must be cleared")

    missing_fill = copy.deepcopy(frame)
    fill(missing_fill, present_active["rect"], CARD_DIM_RAMP[2])
    assert_failure_contains(verify(steps, layout, missing_fill), "cards.slot0.bars[2]")

    missing_outline = copy.deepcopy(frame)
    fill(missing_outline, present_inactive["rect"], BACKGROUND)
    assert_failure_contains(verify(steps, layout, missing_outline), "cards.slot0.bars[4]")

    ghost_outline = copy.deepcopy(frame)
    fill(ghost_outline, absent["rect"], CARD_DIM_RAMP[4])
    assert_failure_contains(verify(steps, layout, ghost_outline), "cards.slot1.bars[4]")


def test_missing_active_main_bar_fails() -> None:
    def remove_bar(_steps: dict, layout: dict, frame: dvc.Framebuffer) -> None:
        fill(frame, layout["elements"]["mainSignalBars"][4]["rect"], BACKGROUND)

    assert_failure_contains(corrupt(remove_bar), "signal_bars[4]")


def test_extra_stale_main_bar_fails() -> None:
    def add_stale_bar(_steps: dict, layout: dict, frame: dvc.Framebuffer) -> None:
        fill(frame, layout["elements"]["mainSignalBars"][6]["rect"], MAIN_RAMP[6])

    assert_failure_contains(corrupt(add_stale_bar), "signal_bars[6]")


def test_wrong_active_color_fails() -> None:
    def wrong_band_color(_steps: dict, layout: dict, frame: dvc.Framebuffer) -> None:
        fill(frame, layout["elements"]["bandCells"][1]["rect"], X)

    assert_failure_contains(corrupt(wrong_band_color), "band_cells.ka")


def test_wrong_inactive_state_fails() -> None:
    def blank_inactive_arrow(_steps: dict, layout: dict, frame: dvc.Framebuffer) -> None:
        fill(frame, layout["elements"]["directionArrows"][2]["rect"], BACKGROUND)

    assert_failure_contains(corrupt(blank_inactive_arrow), "direction_arrows.rear")


def test_text_coverage_below_threshold_fails() -> None:
    def blank_frequency(_steps: dict, layout: dict, frame: dvc.Framebuffer) -> None:
        fill(frame, layout["elements"]["frequency"]["rect"], BACKGROUND)

    assert_failure_contains(corrupt(blank_frequency), "frequency")


def test_text_coverage_uses_the_text_role_over_a_colored_backdrop() -> None:
    steps, layout, frame = validated_known_good()
    card = layout["elements"]["cardSlots"][0]
    fill(frame, card["rect"], 0x0009)
    fill_coverage(frame, card["rect"], TEXT, 0.20)

    summary = verify(steps, layout, frame)
    assert_true(summary.result == "PASS", f"colored card backdrop hid valid text: {summary.to_json()}")


def test_ghost_card_pixels_fail() -> None:
    empty_rect = base_layout()["elements"]["cardSlots"][1]["emptyRect"]
    points = (
        (empty_rect["x"] + 1, empty_rect["y"] + 1),
        (empty_rect["x"] + empty_rect["w"] - 2, empty_rect["y"] + 1),
        (empty_rect["x"] + 1, empty_rect["y"] + empty_rect["h"] - 2),
        (empty_rect["x"] + empty_rect["w"] - 2, empty_rect["y"] + empty_rect["h"] - 2),
    )
    for x, y in points:
        def ghost_empty_card(
            _steps: dict,
            _layout: dict,
            frame: dvc.Framebuffer,
            x: int = x,
            y: int = y,
        ) -> None:
            frame.set_pixel(x, y, TEXT)

        assert_failure_contains(corrupt(ghost_empty_card), "cards.slot1")


def test_outside_declared_region_fails_cleanliness() -> None:
    def stray_pixel(_steps: dict, _layout: dict, frame: dvc.Framebuffer) -> None:
        frame.set_pixel(400, 120, TEXT)

    assert_failure_contains(corrupt(stray_pixel), "cleanliness")


def encode_wrong_identity(frame: dvc.Framebuffer) -> bytes:
    raw = [BACKGROUND] * (dvc.RAW_WIDTH * dvc.RAW_HEIGHT)
    for y in range(min(frame.height, dvc.RAW_HEIGHT)):
        for x in range(min(frame.width, dvc.RAW_WIDTH)):
            raw[y * dvc.RAW_WIDTH + x] = frame.pixel(x, y)
    out = bytearray()
    for value in raw:
        out.append(value & 0xFF)
        out.append((value >> 8) & 0xFF)
    return bytes(out)


def test_framebuffer_rotation_mapping_and_wrong_rotation_failure() -> None:
    steps, layout, frame = validated_known_good()
    raw = dvc.encode_logical_to_raw(frame)
    headers = dvc.framebuffer_headers(binding=BINDING)
    decoded = dvc.decode_framebuffer(raw, headers, binding=BINDING)
    assert_true(decoded.pixel(10, 22) == KA, "known logical pixel should survive canvas rotation")
    assert_true(verify(steps, layout, decoded).result == "PASS", "decoded known-good frame should pass")

    wrongly_encoded = encode_wrong_identity(frame)
    bad_decoded = dvc.decode_framebuffer(wrongly_encoded, headers, binding=BINDING)
    assert_failure_contains(verify(steps, layout, bad_decoded), "frequency")


def test_manifest_missing_required_geometry_fails_closed() -> None:
    layout = base_layout()
    del layout["elements"]["directionArrows"]
    try:
        dvc.validate_layout_manifest(layout)
    except dvc.ProtocolError as exc:
        assert_true("directionArrows" in str(exc), str(exc))
        return
    raise AssertionError("missing directionArrows should fail manifest validation")


def test_manifest_empty_status_badges_fails_closed() -> None:
    layout = base_layout()
    layout["elements"]["statusBadges"] = []
    try:
        dvc.validate_layout_manifest(layout)
    except dvc.ProtocolError as exc:
        assert_true("statusBadges" in str(exc), str(exc))
        return
    raise AssertionError("empty statusBadges should fail manifest validation")


def test_manifest_duplicate_fixed_geometry_fails_closed() -> None:
    layout = base_layout()
    layout["elements"]["bandCells"].append(copy.deepcopy(layout["elements"]["bandCells"][0]))
    try:
        dvc.validate_layout_manifest(layout)
    except dvc.ProtocolError as exc:
        assert_true("bandCells" in str(exc) and "exactly 4" in str(exc), str(exc))
        return
    raise AssertionError("duplicate fixed geometry must not satisfy the manifest contract")


def test_manifest_k_cell_requires_the_ku_mask() -> None:
    layout = base_layout()
    layout["elements"]["bandCells"][2]["bandMask"] = 4
    try:
        dvc.validate_layout_manifest(layout)
    except dvc.ProtocolError as exc:
        assert_true("K+Ku" in str(exc), str(exc))
        return
    raise AssertionError("K cell without the Ku mask should fail manifest validation")


def test_unknown_dynamic_palette_role_fails_before_capture() -> None:
    steps = base_steps()
    layout = base_layout()
    steps["steps"][0]["resolved"]["frequencyRole"] = "frequency.missing"
    try:
        dvc.validate_manifest_binding(
            dvc.validate_steps_manifest(steps),
            dvc.validate_layout_manifest(layout),
        )
    except dvc.ProtocolError as exc:
        assert_true("unknown palette role" in str(exc), str(exc))
        return
    raise AssertionError("unknown dynamic roles must fail before framebuffer capture")


def test_manifest_binding_mismatch_fails_closed() -> None:
    steps = base_steps()
    layout = base_layout()
    layout["settingsFingerprint"] = "0x00000002"
    try:
        dvc.validate_manifest_binding(
            dvc.validate_steps_manifest(steps),
            dvc.validate_layout_manifest(layout),
        )
    except dvc.ProtocolError as exc:
        assert_true("binding mismatch" in str(exc), str(exc))
        return
    raise AssertionError("binding mismatch should fail")


def test_framebuffer_binding_headers_are_required() -> None:
    _steps, _layout, frame = validated_known_good()
    headers = dvc.framebuffer_headers()
    try:
        dvc.decode_framebuffer(dvc.encode_logical_to_raw(frame), headers, binding=BINDING)
    except dvc.ProtocolError as exc:
        assert_true("X-Display-Manifest-Schema-Version" in str(exc), str(exc))
        return
    raise AssertionError("missing framebuffer binding headers should fail")


def test_mutating_copy_does_not_hide_known_good_result() -> None:
    steps, layout, frame = validated_known_good()
    bad_frame = copy.deepcopy(frame)
    bad_frame.set_pixel(400, 120, TEXT)
    assert_failure_contains(verify(steps, layout, bad_frame), "cleanliness")
    assert_true(verify(steps, layout, frame).result == "PASS", "original known-good frame should still pass")


def test_transition_discovery_finds_curated_cases() -> None:
    steps, layout, _source_frame, _target_frame = transition_fixture()
    categories = {case.category for case in dvc.discover_transition_cases(steps, layout)}
    assert_true("multi_alert_to_single" in categories, f"missing multi-alert transition: {categories}")
    assert_true("single_alert_to_multi" in categories, f"missing single-alert transition: {categories}")
    assert_true("high_main_bars_to_low" in categories, f"missing high-to-low bars transition: {categories}")
    assert_true("alp_badge_on_to_off" in categories, f"missing ALP badge cleanup transition: {categories}")
    assert_true("ble_badge_on_to_off" in categories, f"missing BLE badge cleanup transition: {categories}")
    assert_true("obd_badge_on_to_off" in categories, f"missing OBD badge cleanup transition: {categories}")


def test_flashing_discovery_requires_arrow_and_band_flash_coverage() -> None:
    steps = base_steps()
    arrow_only = steps["steps"][0]
    arrow_only["resolved"]["flashMask"] = 0x20
    arrow_only["resolved"]["bandFlashMask"] = 0

    both = copy.deepcopy(arrow_only)
    both["index"] = 1
    both["id"] = "001_arrow_and_band_flash"
    both["resolved"]["bandFlashMask"] = 2
    steady = copy.deepcopy(arrow_only)
    steady["index"] = 2
    steady["id"] = "002_steady"
    steady["resolved"]["flashMask"] = 0
    steady["resolved"]["bandFlashMask"] = 0
    steps["steps"].extend((both, steady))
    steps["stepCount"] = 3

    layout = base_layout()
    cases = dvc.discover_transition_cases(steps, layout)
    flashing = next(case for case in cases if case.category == "flashing_to_steady")
    assert_true(flashing.source_index == 1 and flashing.target_index == 2, flashing.__dict__)
    regions = dvc.clean_reference_regions(layout, both, "flashing_to_steady")
    names = {name for name, _rect in regions}
    assert_true(any("direction_arrows" in name for name in names), names)
    assert_true(any("band_cells" in name for name in names), names)


def test_clean_reference_regions_are_the_exact_40_region_contract() -> None:
    layout = base_layout()
    source = base_steps()["steps"][0]
    category = "multi_alert_to_single"
    prefix = f"transition.{category}.clean_reference."
    names = [
        name.removeprefix(prefix)
        for name, _rect in dvc.clean_reference_regions(layout, source, category)
    ]
    expected = [
        "band_cells.laser",
        "band_cells.ka",
        "band_cells.k",
        "band_cells.x",
        "direction_arrows.front",
        "direction_arrows.side",
        "direction_arrows.rear",
        *(f"primary.signal_bars[{index}]" for index in range(8)),
        *(f"cards.slot0.bars[{index}]" for index in range(6)),
        *(f"cards.slot1.bars[{index}]" for index in range(6)),
        "status.bogeyChar",
        "status.mainVolume",
        "status.muteVolume",
        "status.mute",
        "status.mute.coverage",
        "status.alp",
        "status.obd",
        "status.ble",
        "frequency",
        "cards.slot0",
        "cards.slot0.empty",
        "cards.slot1",
        "cards.slot1.empty",
    ]
    assert_true(
        len(names) == 40,
        f"expected 40 clean-reference regions, got {len(names)}: {names}",
    )
    assert_true(
        len(names) == len(set(names)),
        f"clean-reference region names must be unique: {names}",
    )
    assert_true(
        names == expected,
        f"clean-reference contract drifted:\nexpected={expected}\nactual={names}",
    )


def test_transition_known_good_dirty_target_passes() -> None:
    steps, layout, source_frame, target_frame = transition_fixture()
    summary = dvc.verify_transition(
        source_frame,
        target_frame,
        layout,
        steps["steps"][0],
        steps["steps"][1],
        category="multi_alert_to_single",
        clean_target_frame=copy.deepcopy(target_frame),
    )
    assert_true(summary.result == "PASS", f"known-good transition failed: {summary.to_json()}")
    assert_true(summary.transitions_checked == 1, "transition should be counted")
    assert_true(summary.steps_checked == 0, "dirty-frame L1 checks should not inflate clean step count")
    assert_true(summary.transition_records[0].stale_regions_checked > 0, "cleanup regions should be counted")


def test_status_badges_respect_sparse_manifest_thresholds() -> None:
    # Badges keep manifest-declared floors: sparse badge glyphs are legitimate.
    steps, layout, frame = validated_known_good()
    badge_colors = [ALP_DLI, OBD, BLE]
    for badge, badge_color in zip(layout["elements"]["statusBadges"][1:], badge_colors):
        fill(frame, badge["rect"], BACKGROUND)
        fill_coverage(frame, badge["rect"], badge_color, 0.20)
    summary = verify(steps, layout, frame)
    assert_true(summary.result == "PASS", f"sparse badge glyphs failed L1: {summary.to_json()}")


def test_sparse_band_glyph_fails_host_floor() -> None:
    # Band-cell floors are host-owned: a manifest declaring 0.01 must not let
    # a nearly-empty band letter pass (run 8a599c91 regression class).
    def mutate(_steps, layout, frame):
        k_cell = layout["elements"]["bandCells"][2]
        fill(frame, k_cell["rect"], BACKGROUND)
        fill_coverage(frame, k_cell["rect"], K, 0.20)

    summary = corrupt(mutate)
    assert_failure_contains(summary, "coverage >= 35%")


def test_band_glyph_bottom_truncation_fails() -> None:
    # Cut the bottom half off the active Ka letter: coverage may stay high,
    # but the glyph bbox no longer spans the cell height.
    def mutate(_steps, layout, frame):
        cell = dvc.parse_rect(layout["elements"]["bandCells"][1]["rect"], "ka")
        for y in range(cell.y + cell.h // 2, cell.y + cell.h):
            for x in range(cell.x, cell.x + cell.w):
                frame.set_pixel(x, y, BACKGROUND)

    summary = corrupt(mutate)
    assert_failure_contains(summary, "of cell height")


def test_band_glyph_top_chop_fails() -> None:
    # Mirror of the run 8a599c91 incident: the K letter's top rows erased by a
    # neighboring clear rect while the rest of the glyph stays intact.
    def mutate(_steps, layout, frame):
        cell = dvc.parse_rect(layout["elements"]["bandCells"][2]["rect"], "k")
        for y in range(cell.y, cell.y + 3):
            for x in range(cell.x, cell.x + cell.w):
                frame.set_pixel(x, y, BACKGROUND)

    summary = corrupt(mutate)
    assert_failure_contains(summary, "of cell height")


def test_band_glyph_width_truncation_fails() -> None:
    # "Ka" rendered as a bare "K" during a Ka alert: the right of the cell is
    # empty, so the glyph bbox no longer spans the cell width.
    def mutate(_steps, layout, frame):
        cell = dvc.parse_rect(layout["elements"]["bandCells"][1]["rect"], "ka")
        for y in range(cell.y, cell.y + cell.h):
            for x in range(cell.x + cell.w // 2, cell.x + cell.w):
                frame.set_pixel(x, y, BACKGROUND)

    summary = corrupt(mutate)
    assert_failure_contains(summary, "of cell width")


def test_transition_stale_bar_fails_cleanup() -> None:
    steps, layout, source_frame, target_frame = transition_fixture()
    clean_target = copy.deepcopy(target_frame)
    fill(target_frame, layout["elements"]["mainSignalBars"][4]["rect"], MAIN_RAMP[4])
    summary = dvc.verify_transition(
        source_frame,
        target_frame,
        layout,
        steps["steps"][0],
        steps["steps"][1],
        category="high_main_bars_to_low",
        clean_target_frame=clean_target,
    )
    assert_failure_contains(summary, "transition cleanup")


def test_transition_band_cell_erase_fails_clean_reference() -> None:
    # The run 8a599c91 Ka-chop class: a partial erase that keeps full bbox
    # span and healthy coverage, detectable only by dirty-vs-clean identity.
    steps, layout, source_frame, target_frame = transition_fixture()
    clean_target = copy.deepcopy(target_frame)
    k_cell = dvc.parse_rect(layout["elements"]["bandCells"][2]["rect"], "k")
    for x in range(k_cell.x, k_cell.x + k_cell.w // 2):
        target_frame.set_pixel(x, k_cell.y, BACKGROUND)

    l1_only = dvc.verify_step(copy.deepcopy(target_frame), layout, steps["steps"][1])
    assert_true(
        l1_only.result == "PASS",
        f"partial erase should evade coverage/shape floors so this test pins the clean-reference catch: {l1_only.to_json()}",
    )

    summary = dvc.verify_transition(
        source_frame,
        target_frame,
        layout,
        steps["steps"][0],
        steps["steps"][1],
        category="multi_alert_to_single",
        clean_target_frame=clean_target,
    )
    assert_failure_contains(summary, "dirty target differs from clean target")
    assert_true(
        any("clean_reference.band_cells.k" in failure.element for failure in summary.failures),
        summary.to_json(),
    )


def test_l2_wrong_frequency_text_fails() -> None:
    # The wrong-value class: manifest says 34.700, screen shows 35.500.
    # Every coverage/color/shape check passes; only literal decoding fails.
    def mutate(_steps, layout, frame):
        freq = layout["elements"]["frequency"]["rect"]
        draw_qualified_frequency_text(frame, freq, "35.500", FREQUENCY)

    summary = corrupt(mutate)
    assert_failure_contains(summary, "frequency.l2")
    assert_failure_contains(summary, "expected text '34.700'")


def test_l2_frequency_translation_and_uniform_scale_fail_geometry() -> None:
    cases = (
        (3, 0, 1.0, "right translation"),
        (-3, 0, 1.0, "left translation"),
        (0, -3, 1.0, "vertical translation"),
        (0, 0, 0.65, "uniform shrink"),
    )
    for dx, dy, factor, label in cases:
        steps, layout, frame = known_good()
        raw_rect = layout["elements"]["frequency"]["rect"]
        parsed = dvc.parse_rect(raw_rect, "test.frequency_transform")
        image = Image.new("L", (parsed.w, parsed.h))
        for y in range(parsed.h):
            for x in range(parsed.w):
                if frame.pixel(parsed.x + x, parsed.y + y) != BACKGROUND:
                    image.putpixel((x, y), 255)
        bounds = image.getbbox()
        assert bounds is not None
        cropped = image.crop(bounds)
        size = (
            max(1, round(cropped.width * factor)),
            max(1, round(cropped.height * factor)),
        )
        if size != cropped.size:
            cropped = cropped.resize(size, Image.Resampling.NEAREST)
        center_x = (bounds[0] + bounds[2]) / 2 + dx
        center_y = (bounds[1] + bounds[3]) / 2 + dy
        transformed = Image.new("L", image.size)
        transformed.paste(
            cropped,
            (round(center_x - size[0] / 2), round(center_y - size[1] / 2)),
        )
        fill(frame, raw_rect, BACKGROUND)
        for y in range(parsed.h):
            for x in range(parsed.w):
                if transformed.getpixel((x, y)):
                    frame.set_pixel(parsed.x + x, parsed.y + y, FREQUENCY)

        summary = verify(steps, layout, frame)
        assert_failure_contains(summary, "L2 geometry")
        assert_true(
            all(failure.element.endswith(".l2") for failure in summary.failures),
            f"{label} must retain L1 and fail only L2: {summary.to_json()}",
        )


def test_l2_interior_frequency_glyph_translation_and_scale_fail_geometry() -> None:
    cases = (
        (4, 0, 1.0, 1.0, "interior narrow-one right translation"),
        (-4, 0, 1.0, 1.0, "interior narrow-one left translation"),
        (0, 3, 1.0, 1.0, "interior narrow-one vertical translation"),
        (0, 0, 0.70, 0.70, "interior narrow-one uniform shrink"),
        (0, 0, 1.25, 1.25, "interior narrow-one uniform enlargement"),
        (0, 0, 0.80, 1.0, "interior narrow-one width-only compression"),
    )
    for dx, dy, x_factor, y_factor, label in cases:
        steps, layout, frame = known_good()
        expected = "24.150"
        steps["steps"][0]["resolved"]["primary"]["frequencyText"] = expected
        raw_rect = layout["elements"]["frequency"]["rect"]
        parsed = dvc.parse_rect(raw_rect, "test.interior_frequency_transform")
        draw_qualified_frequency_text(frame, raw_rect, expected, FREQUENCY)
        assert_true(verify(steps, layout, frame).result == "PASS", "baseline 24.150 must pass")

        # Independent qualified bounds for position 3's narrow `1`.
        reference = (164, 17, 174, 76)
        x0 = round(reference[0] * parsed.w / 305)
        y0 = round(reference[1] * parsed.h / 85)
        x1 = round((reference[2] + 1) * parsed.w / 305) - 1
        y1 = round((reference[3] + 1) * parsed.h / 85) - 1
        glyph = Image.new("L", (x1 - x0 + 1, y1 - y0 + 1))
        for local_y in range(y0, y1 + 1):
            for local_x in range(x0, x1 + 1):
                if frame.pixel(parsed.x + local_x, parsed.y + local_y) != BACKGROUND:
                    glyph.putpixel((local_x - x0, local_y - y0), 255)
                frame.set_pixel(parsed.x + local_x, parsed.y + local_y, BACKGROUND)
        size = (
            max(1, round(glyph.width * x_factor)),
            max(1, round(glyph.height * y_factor)),
        )
        if size != glyph.size:
            glyph = glyph.resize(size, Image.Resampling.NEAREST)
        center_x = (x0 + x1) / 2 + dx
        center_y = (y0 + y1) / 2 + dy
        target_x = round(center_x - glyph.width / 2)
        target_y = round(center_y - glyph.height / 2)
        for glyph_y in range(glyph.height):
            for glyph_x in range(glyph.width):
                if glyph.getpixel((glyph_x, glyph_y)):
                    frame.set_pixel(
                        parsed.x + target_x + glyph_x,
                        parsed.y + target_y + glyph_y,
                        FREQUENCY,
                    )

        summary = verify(steps, layout, frame)
        assert_failure_contains(summary, "L2 geometry")
        assert_true(
            all(failure.element.endswith(".l2") for failure in summary.failures),
            f"{label} must retain L1 and fail only L2: {summary.to_json()}",
        )


def test_l2_frequency_dot_shrink_fails_geometry() -> None:
    steps, layout, frame = known_good()
    expected = "24.150"
    steps["steps"][0]["resolved"]["primary"]["frequencyText"] = expected
    raw_rect = layout["elements"]["frequency"]["rect"]
    parsed = dvc.parse_rect(raw_rect, "test.frequency_dot_shrink")
    draw_qualified_frequency_text(frame, raw_rect, expected, FREQUENCY)
    assert_true(verify(steps, layout, frame).result == "PASS", "baseline 24.150 must pass")

    reference = dvc.SEG7_FREQUENCY_DOT_BBOX
    x0 = round(reference[0] * parsed.w / dvc.SEG7_FREQUENCY_RECT_WIDTH)
    y0 = round(reference[1] * parsed.h / dvc.SEG7_FREQUENCY_RECT_HEIGHT)
    x1 = round((reference[2] + 1) * parsed.w / dvc.SEG7_FREQUENCY_RECT_WIDTH) - 1
    y1 = round((reference[3] + 1) * parsed.h / dvc.SEG7_FREQUENCY_RECT_HEIGHT) - 1
    fill(
        frame,
        {"x": parsed.x + x0, "y": parsed.y + y0, "w": x1 - x0 + 1, "h": y1 - y0 + 1},
        BACKGROUND,
    )
    new_w = max(1, round((x1 - x0 + 1) * 0.80))
    new_h = max(1, round((y1 - y0 + 1) * 0.80))
    target_x = round((x0 + x1) / 2 - new_w / 2)
    target_y = round((y0 + y1) / 2 - new_h / 2)
    fill(
        frame,
        {"x": parsed.x + target_x, "y": parsed.y + target_y, "w": new_w, "h": new_h},
        FREQUENCY,
    )

    summary = verify(steps, layout, frame)
    assert_failure_contains(summary, "L2 geometry")
    assert_true(
        all(failure.element.endswith(".l2") for failure in summary.failures),
        f"dot shrink must retain L1 and fail only L2: {summary.to_json()}",
    )


def test_l2_frequency_eight_reduced_to_one_segment_fails() -> None:
    steps, layout, frame = known_good()
    frequency = layout["elements"]["frequency"]["rect"]
    parsed = dvc.parse_rect(frequency, "test.frequency_fragment")
    draw_qualified_frequency_text(frame, frequency, "34.800", FREQUENCY)
    steps["steps"][0]["resolved"]["primary"]["frequencyText"] = "34.800"

    columns = [
        [
            y
            for y in range(parsed.y, parsed.y + parsed.h)
            if frame.pixel(x, y) != BACKGROUND
        ]
        for x in range(parsed.x, parsed.x + parsed.w)
    ]
    runs: list[tuple[int, int]] = []
    start = None
    for index in range(parsed.w + 1):
        active = bool(columns[index]) if index < parsed.w else False
        if active and start is None:
            start = index
        elif not active and start is not None:
            runs.append((start, index - 1))
            start = None
    boxes = []
    for x0, x1 in runs:
        ys = [y for x in range(x0, x1 + 1) for y in columns[x]]
        boxes.append((x0, min(ys), x1, max(ys)))
    max_height = max(y1 - y0 + 1 for _, y0, _, y1 in boxes)
    glyph_boxes = [box for box in boxes if box[3] - box[1] + 1 > 0.3 * max_height]
    eight_x0, eight_y0, eight_x1, eight_y1 = glyph_boxes[2]
    eight_x0 += parsed.x
    eight_x1 += parsed.x
    for y in range(eight_y0, eight_y1 + 1):
        for x in range(eight_x0, eight_x1 + 1):
            frame.set_pixel(x, y, BACKGROUND)
    thickness = max(2, (eight_x1 - eight_x0 + 1) // 5)
    for y in range(eight_y0, eight_y0 + (eight_y1 - eight_y0 + 1) // 2):
        for x in range(eight_x0, eight_x0 + thickness):
            frame.set_pixel(x, y, FREQUENCY)

    summary = verify(steps, layout, frame)
    assert_failure_contains(summary, "L2 decode")
    assert_true(
        all(failure.element.endswith(".l2") for failure in summary.failures),
        f"frequency fragment must clear L1 and fail only exact decoding: {summary.to_json()}",
    )


def test_l2_decodes_the_complete_packet_parser_bogey_domain() -> None:
    assert_true(
        frozenset(PROTOCOL_BOGEY_SEGMENT_MASKS) == PACKET_PARSER_BOGEY_CHARS,
        "fixed protocol fixture must cover every packet_parser bogey character",
    )
    glyph_rect = rect(40, 30, 55, 68)
    parsed_rect = dvc.parse_rect(glyph_rect, "test.protocol_bogey")
    for expected, protocol_mask in PROTOCOL_BOGEY_SEGMENT_MASKS.items():
        frame = empty_frame()
        draw_seg7_mask(
            frame,
            glyph_rect,
            protocol_mask,
            BOGEY,
            single_cell=True,
        )
        decoded = dvc.seg7_decode_region(
            frame,
            parsed_rect,
            BACKGROUND,
            drop_dots=True,
            single_cell=True,
        )
        assert_true(
            decoded == expected,
            f"protocol mask 0x{protocol_mask:02X} ({expected!r}) decoded as {decoded!r}",
        )


def test_l2_decodes_every_embedded_v1sevenx_protocol_glyph() -> None:
    header = (ROOT / "include" / "Segment7Font.h").read_text(encoding="utf-8")
    font_bytes = bytes(
        int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", header)
    )
    assert_true(len(font_bytes) == 15732, "embedded V1SevenX byte extraction drifted")
    font = ImageFont.truetype(io.BytesIO(font_bytes), 60)
    glyph_rect = rect(16, 6, 55, 68)
    parsed_rect = dvc.parse_rect(glyph_rect, "test.embedded_v1sevenx")

    for expected in PROTOCOL_BOGEY_SEGMENT_MASKS:
        image = Image.new("L", (dvc.LOGICAL_WIDTH, dvc.LOGICAL_HEIGHT))
        # Mirrors the production single-cell cursor (x=28, textY=8) for the
        # fixed 55x68 top-counter field without consulting decoder patterns.
        ImageDraw.Draw(image).text(
            (parsed_rect.x + 12, parsed_rect.y + 2),
            expected,
            font=font,
            fill=255,
        )
        frame = empty_frame()
        for y in range(parsed_rect.y, parsed_rect.y + parsed_rect.h):
            for x in range(parsed_rect.x, parsed_rect.x + parsed_rect.w):
                if image.getpixel((x, y)):
                    frame.set_pixel(x, y, BOGEY)

        decoded = dvc.seg7_decode_region(
            frame,
            parsed_rect,
            BACKGROUND,
            drop_dots=True,
            single_cell=True,
        )
        assert_true(
            decoded == expected,
            f"embedded V1SevenX glyph {expected!r} decoded as {decoded!r}",
        )


def test_l2_embedded_counter_translation_and_scale_changes_fail() -> None:
    header = (ROOT / "include" / "Segment7Font.h").read_text(encoding="utf-8")
    font_bytes = bytes(
        int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", header)
    )
    font = ImageFont.truetype(io.BytesIO(font_bytes), 60)
    glyph_rect = rect(16, 6, 55, 68)
    parsed_rect = dvc.parse_rect(glyph_rect, "test.embedded_transform")

    def decode(image: Image.Image) -> str:
        frame = empty_frame()
        for y in range(parsed_rect.h):
            for x in range(parsed_rect.w):
                if image.getpixel((x, y)):
                    frame.set_pixel(parsed_rect.x + x, parsed_rect.y + y, BOGEY)
        return dvc.seg7_decode_region(
            frame,
            parsed_rect,
            BACKGROUND,
            drop_dots=True,
            single_cell=True,
        )

    def translate(image: Image.Image, dx: int, dy: int) -> Image.Image:
        transformed = Image.new("L", image.size)
        transformed.paste(image, (dx, dy))
        return transformed

    def scale_about_ink_center(image: Image.Image, factor: float) -> Image.Image:
        bounds = image.getbbox()
        assert bounds is not None
        cropped = image.crop(bounds)
        size = (
            max(1, round(cropped.width * factor)),
            max(1, round(cropped.height * factor)),
        )
        resized = cropped.resize(size, Image.Resampling.NEAREST)
        center_x = (bounds[0] + bounds[2]) / 2
        center_y = (bounds[1] + bounds[3]) / 2
        transformed = Image.new("L", image.size)
        transformed.paste(
            resized,
            (round(center_x - size[0] / 2), round(center_y - size[1] / 2)),
        )
        return transformed

    for expected in PROTOCOL_BOGEY_SEGMENT_MASKS:
        original = Image.new("L", (parsed_rect.w, parsed_rect.h))
        ImageDraw.Draw(original).text((12, 2), expected, font=font, fill=255)
        assert_true(decode(original) == expected, f"baseline {expected!r} must decode")
        for dx, dy in ((-5, 0), (5, 0), (0, -5), (0, 5)):
            decoded = decode(translate(original, dx, dy))
            assert_true(
                decoded != expected,
                f"translated embedded glyph {expected!r} ({dx}, {dy}) aliased itself",
            )
        for factor in (0.5, 0.75, 1.25, 1.5):
            decoded = decode(scale_about_ink_center(original, factor))
            assert_true(
                decoded != expected,
                f"scaled embedded glyph {expected!r} at {factor:.2f}x aliased itself",
            )


def test_l2_every_single_segment_corruption_changes_the_decode() -> None:
    glyph_rect = rect(40, 30, 55, 68)
    parsed_rect = dvc.parse_rect(glyph_rect, "test.protocol_bogey_corruption")
    for expected, protocol_mask in PROTOCOL_BOGEY_SEGMENT_MASKS.items():
        for bit in range(7):
            corrupted_mask = protocol_mask ^ (1 << bit)
            for x_shift, y_shift in SEG7_CORRUPTION_SHIFTS:
                frame = empty_frame()
                draw_seg7_mask(
                    frame,
                    glyph_rect,
                    corrupted_mask,
                    BOGEY,
                    single_cell=True,
                    x_shift=x_shift,
                    y_shift=y_shift,
                )
                decoded = dvc.seg7_decode_region(
                    frame,
                    parsed_rect,
                    BACKGROUND,
                    drop_dots=True,
                    single_cell=True,
                )
                assert_true(
                    decoded != expected,
                    f"{expected!r} mask 0x{protocol_mask:02X} corrupted to "
                    f"0x{corrupted_mask:02X} at shift=({x_shift}, {y_shift}) but still "
                    f"decoded as {decoded!r}",
                )


def test_l2_no_noncanonical_mask_aliases_a_protocol_character() -> None:
    glyph_rect = rect(40, 30, 55, 68)
    parsed_rect = dvc.parse_rect(glyph_rect, "test.noncanonical_bogey_mask")
    canonical = {
        mask: char for char, mask in PROTOCOL_BOGEY_SEGMENT_MASKS.items()
    }
    for segment_mask in range(1, 0x80):
        if segment_mask in canonical:
            frame = empty_frame()
            draw_seg7_mask(
                frame,
                glyph_rect,
                segment_mask,
                BOGEY,
                single_cell=True,
            )
            decoded = dvc.seg7_decode_region(
                frame,
                parsed_rect,
                BACKGROUND,
                drop_dots=True,
                single_cell=True,
            )
            assert_true(
                decoded == canonical[segment_mask],
                f"canonical mask 0x{segment_mask:02X} decoded as {decoded!r}",
            )
        else:
            for x_shift, y_shift in SEG7_CORRUPTION_SHIFTS:
                frame = empty_frame()
                draw_seg7_mask(
                    frame,
                    glyph_rect,
                    segment_mask,
                    BOGEY,
                    single_cell=True,
                    x_shift=x_shift,
                    y_shift=y_shift,
                )
                decoded = dvc.seg7_decode_region(
                    frame,
                    parsed_rect,
                    BACKGROUND,
                    drop_dots=True,
                    single_cell=True,
                )
                assert_true(
                    decoded not in PACKET_PARSER_BOGEY_CHARS,
                    f"noncanonical mask 0x{segment_mask:02X} at "
                    f"shift=({x_shift}, {y_shift}) aliased {decoded!r}",
                )


def test_l2_mirrored_one_and_shifted_counter_fail_end_to_end() -> None:
    cases = (
        ("1", 0x30, 0, 0, "mirrored left-side one"),
        ("1", PROTOCOL_BOGEY_SEGMENT_MASKS["1"], 0, 8, "vertically shifted one"),
        ("8", PROTOCOL_BOGEY_SEGMENT_MASKS["8"], -8, 0, "shifted full counter"),
    )
    for expected, segment_mask, x_shift, y_shift, label in cases:
        steps, layout, frame = known_good()
        steps["steps"][0]["resolved"]["status"]["bogeyChar"] = expected
        counter = layout["elements"]["statusText"][0]["rect"]
        fill(frame, counter, BACKGROUND)
        draw_seg7_mask(
            frame,
            counter,
            segment_mask,
            BOGEY,
            single_cell=True,
            x_shift=x_shift,
            y_shift=y_shift,
        )

        summary = verify(steps, layout, frame)
        assert_failure_contains(summary, "L2 decode")
        assert_true(
            all(failure.element.endswith(".l2") for failure in summary.failures),
            f"{label} must clear L1 and fail only exact decoding: {summary.to_json()}",
        )


def test_l2_one_segment_cannot_pass_as_expected_eight() -> None:
    for bit in range(7):
        steps, layout, frame = known_good()
        steps["steps"][0]["resolved"]["status"]["bogeyChar"] = "8"
        counter = layout["elements"]["statusText"][0]["rect"]
        fill(frame, counter, BACKGROUND)
        draw_seg7_mask(
            frame,
            counter,
            1 << bit,
            BOGEY,
            single_cell=True,
        )

        summary = verify(steps, layout, frame)
        assert_failure_contains(summary, "L2 decode")
        assert_true(
            all(failure.element.endswith(".l2") for failure in summary.failures),
            f"one-segment fixture must clear L1 and fail only exact decoding: {summary.to_json()}",
        )


def test_l2_invalid_bogey_character_contract_fails_closed() -> None:
    for invalid in ("", "Z", "12"):
        steps = base_steps()
        steps["steps"][0]["resolved"]["status"]["bogeyChar"] = invalid
        try:
            dvc.validate_steps_manifest(steps)
        except dvc.ProtocolError as exc:
            message = str(exc)
            assert_true("bogeyChar" in message and repr(invalid) in message, message)
        else:
            raise AssertionError(f"invalid bogeyChar {invalid!r} must be a protocol error")

    blank = base_steps()
    blank["steps"][0]["resolved"]["status"]["bogeyChar"] = " "
    dvc.validate_steps_manifest(blank)


def test_l2_fixed_oracle_matches_the_production_packet_parser_switch() -> None:
    source = (ROOT / "src" / "packet_parser.cpp").read_text(encoding="utf-8")
    match = re.search(
        r"char decodeBogeyCounterByte\(.*?\n\}(?=\n\nbool isAsciiDigit)",
        source,
        flags=re.DOTALL,
    )
    assert_true(match is not None, "production bogey decoder switch not found")
    actual = {
        int(mask): char
        for mask, char in re.findall(
            r"case\s+(\d+):\s+return\s+'(.)';",
            match.group(0),
        )
    }
    expected = {
        mask: char for char, mask in PROTOCOL_BOGEY_SEGMENT_MASKS.items()
    }
    assert_true(
        actual == expected,
        f"packet-parser bogey domain drifted:\nexpected={expected}\nactual={actual}",
    )


def test_l2_device_manifest_requires_the_complete_physical_corpus() -> None:
    complete = complete_device_steps()
    dvc.validate_protocol_bogey_corpus(complete)

    missing_symbol = copy.deepcopy(complete)
    for step in missing_symbol["steps"]:
        if step["resolved"]["status"]["bogeyChar"] == "&":
            step["resolved"]["status"]["bogeyChar"] = " "
    try:
        dvc.validate_protocol_bogey_corpus(missing_symbol)
    except dvc.ProtocolError as exc:
        assert_true("missing" in str(exc) and "&" in str(exc), str(exc))
    else:
        raise AssertionError("device corpus missing '&' must fail closed")

    short = copy.deepcopy(complete)
    short["steps"].pop()
    short["stepCount"] = len(short["steps"])
    try:
        dvc.validate_protocol_bogey_corpus(short)
    except dvc.ProtocolError as exc:
        assert_true("43 steps" in str(exc) and "expected 44" in str(exc), str(exc))
    else:
        raise AssertionError("short device corpus must fail closed")

    reordered = copy.deepcopy(complete)
    reordered["steps"][0], reordered["steps"][1] = (
        reordered["steps"][1],
        reordered["steps"][0],
    )
    try:
        dvc.validate_protocol_bogey_corpus(reordered)
    except dvc.ProtocolError as exc:
        assert_true("step order drifted" in str(exc), str(exc))
    else:
        raise AssertionError("reordered fixed preview steps must fail closed")

    missing_numeric_step = copy.deepcopy(complete)
    missing_numeric_step["steps"][1]["resolved"]["primary"]["frequencyText"] = "LASER"
    try:
        dvc.validate_protocol_bogey_corpus(missing_numeric_step)
    except dvc.ProtocolError as exc:
        assert_true("38 numeric frequency steps" in str(exc), str(exc))
    else:
        raise AssertionError("device corpus with only 38 numeric steps must fail closed")

    drifted_numeric_values = copy.deepcopy(complete)
    for step in drifted_numeric_values["steps"]:
        primary = step["resolved"]["primary"]
        if primary["frequencyText"] == "24.199":
            primary["frequencyText"] = "24.198"
    try:
        dvc.validate_protocol_bogey_corpus(drifted_numeric_values)
    except dvc.ProtocolError as exc:
        assert_true("numeric frequency corpus drifted" in str(exc), str(exc))
        assert_true("24.199" in str(exc) and "24.198" in str(exc), str(exc))
    else:
        raise AssertionError("device corpus with substituted numeric values must fail closed")

    blank_duplicate = copy.deepcopy(complete)
    duplicate_index = next(
        index
        for index, step in enumerate(blank_duplicate["steps"][1:], start=1)
        if step["resolved"]["status"]["bogeyChar"] == "2"
    )
    blank_duplicate["steps"][duplicate_index]["resolved"]["status"]["bogeyChar"] = " "
    try:
        dvc.validate_protocol_bogey_corpus(blank_duplicate)
    except dvc.ProtocolError as exc:
        assert_true("43 nonblank counter steps" in str(exc), str(exc))
    else:
        raise AssertionError("device corpus with a blank duplicate counter must fail closed")

    redistributed = copy.deepcopy(complete)
    numeric_text = redistributed["steps"][0]["resolved"]["primary"]["frequencyText"]
    alpha_text = redistributed["steps"][15]["resolved"]["primary"]["frequencyText"]
    redistributed["steps"][0]["resolved"]["primary"]["frequencyText"] = alpha_text
    redistributed["steps"][15]["resolved"]["primary"]["frequencyText"] = numeric_text
    try:
        dvc.validate_protocol_bogey_corpus(redistributed)
    except dvc.ProtocolError as exc:
        assert_true("numeric/alpha step classification drifted" in str(exc), str(exc))
    else:
        raise AssertionError(
            "redistributing numeric and alpha texts while preserving global counts must fail closed"
        )

    categories = sorted(dvc.REQUIRED_TRANSITION_CATEGORIES)
    assert_true(len(categories) == 9, categories)
    target_indices = [0] * 7 + [1, 15]
    cases = [
        dvc.TransitionCase(category, 2, target, "source", f"target-{target}")
        for category, target in zip(categories, target_indices)
    ]
    complete_by_index = {step["index"]: step for step in complete["steps"]}
    baseline_expected = dvc.expected_l2_decodes_for_run(
        complete["steps"],
        cases,
        complete_by_index,
    )
    assert_true(baseline_expected == 100, baseline_expected)
    dvc.require_full_default_l2_workload(baseline_expected, full_default_run=True)

    redistributed_by_index = {
        step["index"]: step for step in redistributed["steps"]
    }
    redistributed_expected = dvc.expected_l2_decodes_for_run(
        redistributed["steps"],
        cases,
        redistributed_by_index,
    )
    assert_true(redistributed_expected == 94, redistributed_expected)
    try:
        dvc.require_full_default_l2_workload(
            redistributed_expected,
            full_default_run=True,
        )
    except dvc.ProtocolError as exc:
        assert_true("would execute 94 decodes; required 100" in str(exc), str(exc))
    else:
        raise AssertionError("full default run with only 94 expected L2 decodes must fail closed")


def test_l2_half_erased_one_does_not_decode_as_one() -> None:
    glyph_rect = rect(40, 30, 55, 68)
    parsed_rect = dvc.parse_rect(glyph_rect, "test.half_erased_one")
    for erased_half in ("top", "bottom"):
        frame = empty_frame()
        draw_seg7_text(frame, glyph_rect, "1", BOGEY, single_cell=True)
        cell_top = parsed_rect.y + round(
            parsed_rect.h * dvc.SEG7_COUNTER_CELL_TOP / dvc.SEG7_COUNTER_RECT_HEIGHT
        )
        cell_height = round(
            parsed_rect.h * dvc.SEG7_COUNTER_CELL_HEIGHT / dvc.SEG7_COUNTER_RECT_HEIGHT
        )
        split_y = cell_top + cell_height // 2
        y0, y1 = (
            (parsed_rect.y, split_y)
            if erased_half == "top"
            else (split_y, parsed_rect.y + parsed_rect.h)
        )
        for y in range(y0, y1):
            for x in range(parsed_rect.x, parsed_rect.x + parsed_rect.w):
                frame.set_pixel(x, y, BACKGROUND)

        summary = dvc.VerificationSummary()
        dvc.assert_seg7_text(
            frame,
            summary,
            element=f"test.half_erased_one.{erased_half}",
            rect=parsed_rect,
            expected="1",
            background=BACKGROUND,
            drop_dots=True,
            single_cell=True,
        )
        assert_failure_contains(summary, "L2 decode")


def test_l2_wrong_counter_symbol_fails() -> None:
    def mutate(_steps, layout, frame):
        counter = layout["elements"]["statusText"][0]["rect"]
        fill(frame, counter, BACKGROUND)
        draw_seg7_text(frame, counter, "3", BOGEY, single_cell=True)

    summary = corrupt(mutate)
    assert_failure_contains(summary, "L2 decode")
    assert_failure_contains(summary, "'3'")


def test_transition_status_text_erase_fails_clean_reference() -> None:
    # Stage D2: with all render paths pin-deterministic, status text is under
    # pixel-exact protection too — an erased top-counter fragment on the
    # dirty path must fail, the same class as the chopped band letters.
    steps, layout, source_frame, target_frame = transition_fixture()
    clean_target = copy.deepcopy(target_frame)
    counter = dvc.parse_rect(layout["elements"]["statusText"][0]["rect"], "topCounter")
    ink = [
        (x, y)
        for y in range(counter.y, counter.y + counter.h)
        for x in range(counter.x, counter.x + counter.w)
        if target_frame.pixel(x, y) == BOGEY
    ]
    assert_true(bool(ink), "top-counter fixture must contain rendered ink")
    target_frame.set_pixel(*ink[0], BACKGROUND)

    summary = dvc.verify_transition(
        source_frame,
        target_frame,
        layout,
        steps["steps"][0],
        steps["steps"][1],
        category="multi_alert_to_single",
        clean_target_frame=clean_target,
    )
    assert_failure_contains(summary, "dirty target differs from clean target")
    assert_true(
        any("clean_reference.status.bogeyChar" in failure.element for failure in summary.failures),
        summary.to_json(),
    )


def test_transition_missing_clean_reference_is_an_error() -> None:
    steps, layout, source_frame, target_frame = transition_fixture()
    summary = dvc.verify_transition(
        source_frame,
        target_frame,
        layout,
        steps["steps"][0],
        steps["steps"][1],
        category="multi_alert_to_single",
    )
    assert_true(summary.result == "ERROR", summary.to_json())
    assert_true(
        any("missing its clean target reference capture" in error for error in summary.errors),
        summary.errors,
    )


def test_frequency_transition_compares_dirty_target_to_clean_reference() -> None:
    steps, layout, source_frame, target_frame = transition_fixture()
    source_step = steps["steps"][0]
    target_step = steps["steps"][1]
    source_step["resolved"]["primary"]["frequencyText"] = "24.150"
    target_step["resolved"]["primary"]["frequencyText"] = "34.700"
    frequency_rect = layout["elements"]["frequency"]["rect"]

    draw_qualified_frequency_text(source_frame, frequency_rect, "24.150", FREQUENCY)
    clean_target = copy.deepcopy(target_frame)
    draw_qualified_frequency_text(clean_target, frequency_rect, "34.700", FREQUENCY)

    passing = dvc.verify_transition(
        source_frame,
        copy.deepcopy(clean_target),
        layout,
        source_step,
        target_step,
        category="frequency_wide_to_narrow",
        clean_target_frame=clean_target,
    )
    assert_true(passing.result == "PASS", passing.to_json())

    dirty_target = copy.deepcopy(clean_target)
    parsed = dvc.parse_rect(frequency_rect, "frequency")
    # Choose a real source-only pixel from the changed numeric glyphs.
    stale_x, stale_y = next(
        (x, y)
        for y in range(parsed.y, parsed.y + parsed.h)
        for x in range(parsed.x, parsed.x + parsed.w)
        if source_frame.pixel(x, y) != BACKGROUND
        and clean_target.pixel(x, y) == BACKGROUND
    )
    dirty_target.set_pixel(stale_x, stale_y, FREQUENCY)
    failing = dvc.verify_transition(
        source_frame,
        dirty_target,
        layout,
        source_step,
        target_step,
        category="frequency_wide_to_narrow",
        clean_target_frame=clean_target,
    )
    assert_failure_contains(failing, "dirty target differs from clean target")


def test_flashing_transition_targets_the_authored_flash_regions() -> None:
    steps, layout, _source_frame, _target_frame = transition_fixture()
    source = steps["steps"][0]
    source["resolved"]["flashMask"] = 0x40
    source["resolved"]["bandFlashMask"] = 4
    regions = dvc.clean_reference_regions(layout, source, "flashing_to_steady")
    names = {name for name, _rect in regions}
    assert_true("transition.flashing_to_steady.direction_arrows.side" in names, names)
    assert_true("transition.flashing_to_steady.band_cells.k" in names, names)


def test_transition_invalid_source_is_error_not_display_failure() -> None:
    steps, layout, source_frame, target_frame = transition_fixture()
    k_cell = layout["elements"]["bandCells"][2]
    fill(source_frame, k_cell["rect"], BACKGROUND)

    summary = dvc.verify_transition(
        source_frame,
        target_frame,
        layout,
        steps["steps"][0],
        steps["steps"][1],
        category="multi_alert_to_single",
    )
    assert_true(summary.result == "ERROR", summary.to_json())
    assert_true(any("source did not exercise" in error for error in summary.errors), summary.errors)
    assert_true(summary.transition_records[0].result == "ERROR", summary.transition_records[0].__dict__)


def test_clear_visual_pin_requires_release_and_restore_ack() -> None:
    good = {"success": True, "active": False, "restored": True}
    with mock.patch.object(dvc, "post_json", return_value=good):
        assert_true(dvc.clear_visual_pin("http://device") == good, "valid clear acknowledgement failed")

    invalid = (
        {"success": False, "active": False, "restored": True},
        {"success": True, "active": True, "restored": True},
        {"success": True, "active": False},
    )
    for response in invalid:
        with mock.patch.object(dvc, "post_json", return_value=response):
            try:
                dvc.clear_visual_pin("http://device")
            except dvc.ProtocolError:
                continue
        raise AssertionError(f"invalid clear acknowledgement passed: {response}")


def runner_fixture() -> tuple[dict, dict, dvc.Framebuffer, object]:
    _single_step, layout, frame = validated_known_good()
    steps = complete_device_steps()
    dvc.validate_steps_manifest(steps)
    dvc.validate_protocol_bogey_corpus(steps)
    dvc.validate_manifest_binding(steps, layout)
    args = dvc.parse_args(["test-device", "--step", "0"])
    return steps, layout, frame, args


def shadow_response_for(frame: dvc.Framebuffer, *, render_seq: int = 7) -> tuple[bytes, dict[str, str]]:
    headers = dvc.framebuffer_headers(render_seq=render_seq, binding=BINDING)
    headers["X-FB-Shadow"] = "1"
    return dvc.encode_logical_to_raw(frame), headers


def test_flush_shadow_divergence_fails_with_named_pixels() -> None:
    _steps, _layout, frame = validated_known_good()
    shadow = dvc.Framebuffer(frame.width, frame.height, list(frame.pixels))

    summary = dvc.VerificationSummary()
    with mock.patch.object(dvc, "fetch_flush_shadow", return_value=shadow_response_for(shadow)):
        dvc.verify_flush_shadow("http://device", frame, 7, BINDING, summary, element="flush_shadow.match")
    assert_true(not summary.failures, f"matching shadow must pass: {summary.to_json()}")
    assert_true(summary.flush_shadow_checked, summary.to_json())
    assert_true(summary.flush_shadow_compares == 1, summary.to_json())

    # Simulate dirty-window under-coverage: the framebuffer has pixels the
    # panel never received (shadow still background where fb is lit).
    shadow.set_pixel(10, 10, BACKGROUND if frame.pixel(10, 10) != BACKGROUND else TEXT)
    shadow.set_pixel(11, 10, BACKGROUND if frame.pixel(11, 10) != BACKGROUND else TEXT)
    summary = dvc.VerificationSummary()
    with mock.patch.object(dvc, "fetch_flush_shadow", return_value=shadow_response_for(shadow)):
        dvc.verify_flush_shadow("http://device", frame, 7, BINDING, summary, element="flush_shadow.diverged")
    assert_failure_contains(summary, "flush shadow diverges from framebuffer at 2/")
    assert_failure_contains(summary, "(10,10)")


def test_flush_shadow_sequence_drift_and_missing_marker_are_protocol_errors() -> None:
    _steps, _layout, frame = validated_known_good()

    drifted = shadow_response_for(frame, render_seq=8)
    summary = dvc.VerificationSummary()
    try:
        with mock.patch.object(dvc, "fetch_flush_shadow", return_value=drifted):
            dvc.verify_flush_shadow("http://device", frame, 7, BINDING, summary, element="flush_shadow.seq")
    except dvc.ProtocolError as exc:
        assert_true("render sequence changed" in str(exc), str(exc))
    else:
        raise AssertionError("sequence drift must be a protocol error")

    body, headers = shadow_response_for(frame)
    del headers["X-FB-Shadow"]
    try:
        with mock.patch.object(dvc, "fetch_flush_shadow", return_value=(body, headers)):
            dvc.verify_flush_shadow("http://device", frame, 7, BINDING, summary, element="flush_shadow.marker")
    except dvc.ProtocolError:
        pass
    else:
        raise AssertionError("missing X-FB-Shadow marker must be a protocol error")


def test_run_device_verifies_flush_shadow_per_pin_and_flag_skips_it() -> None:
    steps, layout, frame, args = runner_fixture()
    shadow_fetches: list[str] = []

    def fetch(_base_url: str, path: str) -> dict:
        return copy.deepcopy(steps if path.endswith("/steps") else layout)

    def shadow(_base_url: str):
        shadow_fetches.append("shadow")
        return shadow_response_for(frame)

    summary = dvc.VerificationSummary(result="RUNNING", run_id="shadow-run")
    with (
        mock.patch.object(dvc, "fetch_json", side_effect=fetch),
        mock.patch.object(dvc, "pin_and_capture", return_value=(copy.deepcopy(frame), 7)),
        mock.patch.object(dvc, "fetch_flush_shadow", side_effect=shadow),
        mock.patch.object(dvc, "clear_visual_pin", return_value={"success": True, "active": False, "restored": True}),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        result = dvc.run_device(args, summary=summary)
    assert_true(result.result == "PASS", result.to_json())
    assert_true(result.l2_decodes == result.l2_decodes_expected == 2, result.to_json())
    assert_true(len(shadow_fetches) == 1, f"one shadow fetch per pin expected, got {len(shadow_fetches)}")
    assert_true(result.flush_shadow_checked, result.to_json())
    assert_true(result.flush_shadow_compares == 1, result.to_json())
    assert_true(result.to_json()["verdict_scope"]["flush_shadow_checked"] is True, result.to_json())
    assert_true(result.scope_label() == "framebuffer-semantic-l1-l2+flushshadow", result.scope_label())

    disabled_args = dvc.parse_args(["test-device", "--step", "0", "--no-flush-shadow"])
    shadow_fetches.clear()
    summary = dvc.VerificationSummary(result="RUNNING", run_id="no-shadow-run")
    with (
        mock.patch.object(dvc, "fetch_json", side_effect=fetch),
        mock.patch.object(dvc, "pin_and_capture", return_value=(copy.deepcopy(frame), 7)),
        mock.patch.object(dvc, "fetch_flush_shadow", side_effect=shadow),
        mock.patch.object(dvc, "clear_visual_pin", return_value={"success": True, "active": False, "restored": True}),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        result = dvc.run_device(disabled_args, summary=summary)
    assert_true(result.result == "PASS", result.to_json())
    assert_true(not shadow_fetches, "flag must skip shadow fetches")
    assert_true(not result.flush_shadow_checked, result.to_json())
    assert_true(result.scope_label() == "framebuffer-semantic-l1-l2", result.scope_label())


def test_run_device_cleans_up_after_success() -> None:
    steps, layout, frame, args = runner_fixture()
    events: list[str] = []

    def fetch(_base_url: str, path: str) -> dict:
        return copy.deepcopy(steps if path.endswith("/steps") else layout)

    def capture(*_args, **_kwargs):
        events.append("capture")
        return copy.deepcopy(frame), 7

    def clear(_base_url: str) -> dict:
        events.append("clear")
        return {"success": True, "active": False, "restored": True}

    summary = dvc.VerificationSummary(result="RUNNING", run_id="success-run")
    with (
        mock.patch.object(dvc, "fetch_json", side_effect=fetch),
        mock.patch.object(dvc, "pin_and_capture", side_effect=capture),
        mock.patch.object(dvc, "fetch_flush_shadow", side_effect=lambda _u: shadow_response_for(frame)),
        mock.patch.object(dvc, "clear_visual_pin", side_effect=clear),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        result = dvc.run_device(args, summary=summary)

    assert_true(result.result == "PASS", result.to_json())
    assert_true(result.verification_completed, result.to_json())
    assert_true(result.steps_checked == result.steps_expected == 1, result.to_json())
    assert_true(result.cleanup_attempted and result.cleanup_succeeded, result.to_json())
    assert_true(events == ["capture", "clear"], events)


def test_run_device_l2_decode_coverage_mismatch_is_error() -> None:
    steps, layout, frame, args = runner_fixture()

    def fetch(_base_url: str, path: str) -> dict:
        return copy.deepcopy(steps if path.endswith("/steps") else layout)

    summary = dvc.VerificationSummary(result="RUNNING", run_id="l2-coverage-error-run")
    with (
        mock.patch.object(dvc, "fetch_json", side_effect=fetch),
        mock.patch.object(dvc, "pin_and_capture", return_value=(copy.deepcopy(frame), 7)),
        mock.patch.object(dvc, "fetch_flush_shadow", side_effect=lambda _u: shadow_response_for(frame)),
        mock.patch.object(dvc, "verify_step", return_value=summary),
        mock.patch.object(
            dvc,
            "clear_visual_pin",
            return_value={"success": True, "active": False, "restored": True},
        ),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        result = dvc.run_device(args, summary=summary)

    assert_true(result.result == "ERROR", result.to_json())
    assert_true(result.verification_completed, result.to_json())
    assert_true(result.l2_decodes == 0, result.to_json())
    assert_true(result.l2_decodes_expected == 2, result.to_json())
    assert_true(
        any("L2 decode coverage incomplete: checked 0, expected 2" in error for error in result.errors),
        result.errors,
    )
    assert_true(dvc.summary_exit_code(result) == 2, result.to_json())


def test_run_device_cleans_up_after_capture_error() -> None:
    steps, layout, _frame, args = runner_fixture()
    events: list[str] = []

    def fetch(_base_url: str, path: str) -> dict:
        return copy.deepcopy(steps if path.endswith("/steps") else layout)

    def capture(*_args, **_kwargs):
        events.append("pin-attempt")
        raise dvc.ProtocolError("capture failed after pin")

    def clear(_base_url: str) -> dict:
        events.append("clear")
        return {"success": True, "active": False, "restored": True}

    summary = dvc.VerificationSummary(result="RUNNING", run_id="error-run")
    with (
        mock.patch.object(dvc, "fetch_json", side_effect=fetch),
        mock.patch.object(dvc, "pin_and_capture", side_effect=capture),
        mock.patch.object(dvc, "clear_visual_pin", side_effect=clear),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        try:
            dvc.run_device(args, summary=summary)
        except dvc.ProtocolError as exc:
            assert_true("capture failed" in str(exc), str(exc))
        else:
            raise AssertionError("capture error should propagate to the CLI boundary")

    assert_true(events == ["pin-attempt", "clear"], events)
    assert_true(summary.cleanup_attempted and summary.cleanup_succeeded, summary.to_json())
    assert_true(not summary.verification_completed, summary.to_json())
    assert_true(not summary.flush_shadow_checked, summary.to_json())
    assert_true(summary.flush_shadow_compares == 0, summary.to_json())
    assert_true("+flushshadow" not in summary.scope_label(), summary.scope_label())


def test_cleanup_failure_promotes_pass_to_error() -> None:
    steps, layout, frame, args = runner_fixture()

    def fetch(_base_url: str, path: str) -> dict:
        return copy.deepcopy(steps if path.endswith("/steps") else layout)

    summary = dvc.VerificationSummary(result="RUNNING", run_id="cleanup-error-run")
    with (
        mock.patch.object(dvc, "fetch_json", side_effect=fetch),
        mock.patch.object(dvc, "pin_and_capture", return_value=(copy.deepcopy(frame), 7)),
        mock.patch.object(dvc, "fetch_flush_shadow", side_effect=lambda _u: shadow_response_for(frame)),
        mock.patch.object(dvc, "clear_visual_pin", side_effect=dvc.ProtocolError("clear refused")),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        dvc.run_device(args, summary=summary)

    assert_true(summary.result == "ERROR", summary.to_json())
    assert_true(summary.verification_completed, summary.to_json())
    assert_true(summary.cleanup_attempted and not summary.cleanup_succeeded, summary.to_json())
    assert_true(any("visual cleanup failed" in error for error in summary.errors), summary.errors)
    assert_true(dvc.summary_exit_code(summary) == 2, summary.to_json())


def test_definitive_console_result_is_last_and_not_raw_json() -> None:
    summary = dvc.VerificationSummary(
        result="PASS",
        run_id="console-run",
        verification_completed=True,
        exit_code=0,
        steps_expected=44,
        steps_checked=44,
        transitions_expected=9,
        transitions_checked=9,
        cleanup_attempted=True,
        cleanup_succeeded=True,
    )
    summary.captures.extend(
        dvc.CaptureRecord(f"capture-{index}", "step", index, f"step-{index}") for index in range(100)
    )
    output = io.StringIO()
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
        dvc.print_final_result(summary, ".artifacts/test")
    rendered = output.getvalue()
    assert_true(rendered.rstrip().splitlines()[-1].startswith("RESULT PASS exit=0"), rendered)
    assert_true('"captures"' not in rendered and not rendered.lstrip().startswith("{"), rendered)


def test_exit_code_contract_and_error_precedence() -> None:
    passed = dvc.VerificationSummary(
        result="PASS",
        verification_completed=True,
        cleanup_attempted=True,
        cleanup_succeeded=True,
    )
    failed = copy.deepcopy(passed)
    failed.add_failure("element", "semantic failure")
    errored = copy.deepcopy(failed)
    errored.add_error("tool error")
    errored.add_failure("later", "must not downgrade the error")
    interrupted = copy.deepcopy(passed)
    interrupted.interrupted = True

    assert_true(dvc.summary_exit_code(passed) == 0, passed.to_json())
    assert_true(dvc.summary_exit_code(failed) == 1, failed.to_json())
    assert_true(dvc.summary_exit_code(errored) == 2, errored.to_json())
    assert_true(errored.result == "ERROR", errored.to_json())
    assert_true(dvc.summary_exit_code(interrupted) == 130, interrupted.to_json())


def test_main_reports_protocol_error_and_interrupt_without_raw_json() -> None:
    cases = (
        (dvc.ProtocolError("broken contract"), 2, "RESULT ERROR exit=2"),
        (KeyboardInterrupt(), 130, "RESULT INTERRUPTED exit=130"),
    )
    for raised, expected_exit, final_prefix in cases:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(dvc, "run_device", side_effect=raised),
            mock.patch.object(dvc, "write_reports"),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            exit_code = dvc.main(["test-device", "--step", "0"])
        rendered = stdout.getvalue()
        assert_true(exit_code == expected_exit, rendered)
        assert_true(rendered.rstrip().splitlines()[-1].startswith(final_prefix), rendered)
        assert_true('"captures"' not in rendered and not rendered.lstrip().startswith("{"), rendered)


def test_main_success_finalizes_auditable_run_and_cleanup() -> None:
    steps, layout, frame, _args = runner_fixture()

    def fetch(_base_url: str, path: str) -> dict:
        return copy.deepcopy(steps if path.endswith("/steps") else layout)

    clear_response = {"success": True, "active": False, "restored": True}
    with tempfile.TemporaryDirectory() as tmp:
        stdout = io.StringIO()
        with (
            mock.patch.object(dvc, "fetch_json", side_effect=fetch),
            mock.patch.object(dvc, "pin_and_capture", return_value=(copy.deepcopy(frame), 7)),
            mock.patch.object(dvc, "fetch_flush_shadow", side_effect=lambda _u: shadow_response_for(frame)),
            mock.patch.object(dvc, "clear_visual_pin", return_value=clear_response),
            contextlib.redirect_stdout(stdout),
        ):
            exit_code = dvc.main(["test-device", "--step", "0", "--output-dir", tmp])

        rendered = stdout.getvalue()
        payload = json.loads((Path(tmp) / "display_visual_summary.json").read_text(encoding="utf-8"))
        run_summary = Path(tmp) / "runs" / payload["run_id"] / "display_visual_summary.json"
        assert_true(exit_code == 0, rendered)
        assert_true(rendered.rstrip().splitlines()[-1].startswith("RESULT PASS exit=0"), rendered)
        assert_true(payload["verification_completed"] is True, payload)
        assert_true(payload["steps_checked"] == payload["steps_expected"] == 1, payload)
        assert_true(payload["cleanup_succeeded"] is True, payload)
        assert_true(payload["cleanup_response"] == clear_response, payload)
        assert_true(payload["artifacts_requested"] is True, payload)
        assert_true(payload["artifacts_finalized"] is True, payload)
        assert_true(payload["exit_code"] == 0, payload)
        assert_true(bool(payload["binding"]["firmware_sha"]), payload)
        assert_true(bool(payload["verifier_sha256"]), payload)
        assert_true(run_summary.exists(), "immutable run summary missing")


def test_report_finalize_failure_rewrites_an_error_summary() -> None:
    report_calls = 0
    persisted: list[dict] = []

    def fake_run(_args, *, summary):
        summary.verification_completed = True
        summary.steps_expected = 1
        summary.steps_checked = 1
        summary.cleanup_attempted = True
        summary.cleanup_succeeded = True
        summary.finalize_result()
        return summary

    def fake_reports(_summary, _output_dir, *, no_report):
        nonlocal report_calls
        del no_report
        report_calls += 1
        if report_calls == 2:
            raise OSError("html write failed")

    def fake_summary(summary, _output_dir):
        persisted.append(copy.deepcopy(summary.to_json()))

    stdout = io.StringIO()
    with (
        mock.patch.object(dvc, "run_device", side_effect=fake_run),
        mock.patch.object(dvc, "write_reports", side_effect=fake_reports),
        mock.patch.object(dvc, "write_summary", side_effect=fake_summary),
        contextlib.redirect_stdout(stdout),
        contextlib.redirect_stderr(io.StringIO()),
    ):
        exit_code = dvc.main(["test-device", "--step", "0", "--output-dir", ".artifacts/test"])

    assert_true(exit_code == 2, stdout.getvalue())
    assert_true(persisted and persisted[-1]["result"] == "ERROR", persisted)
    assert_true(persisted[-1]["exit_code"] == 2, persisted[-1])
    assert_true(persisted[-1]["artifacts_finalized"] is False, persisted[-1])
    assert_true(stdout.getvalue().rstrip().splitlines()[-1].startswith("RESULT ERROR exit=2"), stdout.getvalue())


def test_report_initialization_failure_persists_error_without_running_device() -> None:
    persisted: list[dict] = []
    stdout = io.StringIO()

    def fake_summary(summary, _output_dir):
        persisted.append(copy.deepcopy(summary.to_json()))

    with (
        mock.patch.object(dvc, "write_reports", side_effect=OSError("html init failed")),
        mock.patch.object(dvc, "write_summary", side_effect=fake_summary),
        mock.patch.object(dvc, "run_device") as run_device,
        contextlib.redirect_stdout(stdout),
        contextlib.redirect_stderr(io.StringIO()),
    ):
        exit_code = dvc.main(["test-device", "--step", "0", "--output-dir", ".artifacts/test"])

    assert_true(exit_code == 2, stdout.getvalue())
    assert_true(not run_device.called, "device run must not start without an in-progress marker")
    assert_true(persisted and persisted[-1]["result"] == "ERROR", persisted)
    assert_true(persisted[-1]["exit_code"] == 2, persisted[-1])
    assert_true(persisted[-1]["verification_completed"] is False, persisted[-1])


def test_report_artifacts_include_json_html_and_png() -> None:
    steps, layout, frame = validated_known_good()
    step = steps["steps"][0]
    with tempfile.TemporaryDirectory() as tmp:
        output_dir = Path(tmp)
        summary = dvc.VerificationSummary(run_id="artifact-run")
        dvc.record_capture(
            summary,
            frame,
            output_dir,
            capture_id="step_000_ka_front_34700",
            kind="step",
            step=step,
            render_seq=7,
        )
        dvc.verify_step(frame, layout, step, summary=summary)
        summary.l2_decodes_expected = summary.l2_decodes
        summary.flush_shadow_checked = True
        summary.flush_shadow_compares = 71
        dvc.write_run_inputs(steps, layout, str(output_dir), summary.run_id)
        dvc.write_reports(summary, str(output_dir), no_report=False)

        summary_path = output_dir / "display_visual_summary.json"
        report_json_path = output_dir / "display_visual_report.json"
        html_path = output_dir / "display_visual_report.html"
        archived_summary_path = output_dir / "runs" / "artifact-run" / "display_visual_summary.json"
        archived_steps_path = output_dir / "runs" / "artifact-run" / "display_visual_steps.json"
        archived_layout_path = output_dir / "runs" / "artifact-run" / "display_visual_layout.json"
        png_path = output_dir / "captures" / "artifact-run" / "step_000_ka_front_34700.png"
        assert_true(summary_path.exists(), "summary JSON should be written")
        assert_true(report_json_path.exists(), "report JSON should be written")
        assert_true(archived_summary_path.exists(), "run-specific summary JSON should be written")
        assert_true(archived_steps_path.exists(), "run-specific steps manifest should be written")
        assert_true(archived_layout_path.exists(), "run-specific layout manifest should be written")
        assert_true(html_path.exists(), "HTML report should be written")
        assert_true(png_path.exists(), "capture PNG should be written")
        assert_true(png_path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"), "capture should be a PNG")
        payload = json.loads(summary_path.read_text(encoding="utf-8"))
        assert_true(
            payload["captures"][0]["path"] == "captures/artifact-run/step_000_ka_front_34700.png",
            payload,
        )
        assert_true(payload["run_id"] == "artifact-run", payload)
        assert_true(len(payload["captures"][0]["sha256"]) == 64, payload)
        rendered_html = html_path.read_text(encoding="utf-8")
        assert_true("Display Visual Verification Report" in rendered_html, "HTML title missing")
        assert_true(summary.scope_label() in rendered_html, "HTML scope must use the dynamic summary scope")
        assert_true("L2 decodes" in rendered_html, "HTML must label the L2 decode count")
        assert_true(
            f">{summary.l2_decodes} / {summary.l2_decodes_expected}<" in rendered_html,
            "HTML must include actual/expected L2 decode coverage",
        )
        assert_true("Flush-shadow comparisons" in rendered_html, "HTML must label flush-shadow comparisons")
        assert_true(
            f">{summary.flush_shadow_compares}<" in rendered_html,
            "HTML must include flush-shadow count",
        )


def test_dynamic_scope_omits_unexecuted_levels_in_html() -> None:
    summary = dvc.VerificationSummary(run_id="zero-l2")
    assert_true(summary.scope_label() == "framebuffer-semantic-none", summary.scope_label())
    summary.transitions_checked = 1
    assert_true(summary.scope_label() == "framebuffer-semantic-l1-l3", summary.scope_label())
    with tempfile.TemporaryDirectory() as tmp:
        dvc.write_html_report(summary, tmp)
        rendered = (Path(tmp) / "display_visual_report.html").read_text(encoding="utf-8")
        assert_true("framebuffer-semantic-l1-l3" in rendered, rendered)
        assert_true("framebuffer-semantic-l1-l2-l3" not in rendered, rendered)


def main() -> int:
    test_known_good_frame_passes()
    test_fixture_matches_renderer_contract()
    test_ku_activates_the_shared_k_cell()
    test_resolved_dynamic_role_fields_are_required()
    test_renderer_faithful_muted_frame_passes_and_normal_colors_fail()
    test_dynamic_badge_roles_drive_verification()
    test_card_meter_present_inactive_and_absent_are_distinct()
    test_missing_active_main_bar_fails()
    test_extra_stale_main_bar_fails()
    test_wrong_active_color_fails()
    test_wrong_inactive_state_fails()
    test_text_coverage_below_threshold_fails()
    test_text_coverage_uses_the_text_role_over_a_colored_backdrop()
    test_ghost_card_pixels_fail()
    test_outside_declared_region_fails_cleanliness()
    test_framebuffer_rotation_mapping_and_wrong_rotation_failure()
    test_manifest_missing_required_geometry_fails_closed()
    test_manifest_empty_status_badges_fails_closed()
    test_manifest_duplicate_fixed_geometry_fails_closed()
    test_manifest_k_cell_requires_the_ku_mask()
    test_unknown_dynamic_palette_role_fails_before_capture()
    test_manifest_binding_mismatch_fails_closed()
    test_framebuffer_binding_headers_are_required()
    test_mutating_copy_does_not_hide_known_good_result()
    test_transition_discovery_finds_curated_cases()
    test_flashing_discovery_requires_arrow_and_band_flash_coverage()
    test_clean_reference_regions_are_the_exact_40_region_contract()
    test_transition_known_good_dirty_target_passes()
    test_status_badges_respect_sparse_manifest_thresholds()
    test_sparse_band_glyph_fails_host_floor()
    test_band_glyph_bottom_truncation_fails()
    test_band_glyph_top_chop_fails()
    test_band_glyph_width_truncation_fails()
    test_transition_stale_bar_fails_cleanup()
    test_transition_band_cell_erase_fails_clean_reference()
    test_l2_wrong_frequency_text_fails()
    test_l2_frequency_translation_and_uniform_scale_fail_geometry()
    test_l2_interior_frequency_glyph_translation_and_scale_fail_geometry()
    test_l2_frequency_dot_shrink_fails_geometry()
    test_l2_frequency_eight_reduced_to_one_segment_fails()
    test_l2_decodes_the_complete_packet_parser_bogey_domain()
    test_l2_decodes_every_embedded_v1sevenx_protocol_glyph()
    test_l2_embedded_counter_translation_and_scale_changes_fail()
    test_l2_every_single_segment_corruption_changes_the_decode()
    test_l2_no_noncanonical_mask_aliases_a_protocol_character()
    test_l2_mirrored_one_and_shifted_counter_fail_end_to_end()
    test_l2_one_segment_cannot_pass_as_expected_eight()
    test_l2_invalid_bogey_character_contract_fails_closed()
    test_l2_fixed_oracle_matches_the_production_packet_parser_switch()
    test_l2_device_manifest_requires_the_complete_physical_corpus()
    test_l2_half_erased_one_does_not_decode_as_one()
    test_l2_wrong_counter_symbol_fails()
    test_transition_status_text_erase_fails_clean_reference()
    test_transition_missing_clean_reference_is_an_error()
    test_frequency_transition_compares_dirty_target_to_clean_reference()
    test_flashing_transition_targets_the_authored_flash_regions()
    test_transition_invalid_source_is_error_not_display_failure()
    test_clear_visual_pin_requires_release_and_restore_ack()
    test_flush_shadow_divergence_fails_with_named_pixels()
    test_flush_shadow_sequence_drift_and_missing_marker_are_protocol_errors()
    test_run_device_verifies_flush_shadow_per_pin_and_flag_skips_it()
    test_run_device_cleans_up_after_success()
    test_run_device_l2_decode_coverage_mismatch_is_error()
    test_run_device_cleans_up_after_capture_error()
    test_cleanup_failure_promotes_pass_to_error()
    test_definitive_console_result_is_last_and_not_raw_json()
    test_exit_code_contract_and_error_precedence()
    test_main_reports_protocol_error_and_interrupt_without_raw_json()
    test_main_success_finalizes_auditable_run_and_cleanup()
    test_report_finalize_failure_rewrites_an_error_summary()
    test_report_initialization_failure_persists_error_without_running_device()
    test_report_artifacts_include_json_html_and_png()
    test_dynamic_scope_omits_unexecuted_levels_in_html()
    print("display visual verifier synthetic tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
