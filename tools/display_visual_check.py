#!/usr/bin/env python3
"""Bench-only semantic verifier for display visual captures.

The host contract validates manifests and framebuffer decode/rotation, applies
L1 semantic and L2 literal seven-segment assertions, and runs L3 transition
and cleanup checks. Reports can also bind framebuffer pixels to the panel
interface through flush-shadow comparisons. Golden-drift and mask-debt
workflows remain future work.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import sys
import urllib.error
import urllib.request
import uuid
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from PIL import Image
except ImportError:  # pragma: no cover - exercised only on hosts missing Pillow.
    Image = None

RAW_WIDTH = 172
RAW_HEIGHT = 640
LOGICAL_WIDTH = 640
LOGICAL_HEIGHT = 172
FRAMEBUFFER_FORMAT = "RGB565LE"
FRAMEBUFFER_TRANSFORM = "canvas-rotation-1"

BAND_MASKS = {
    "laser": 1,
    "l": 1,
    "ka": 2,
    "k": 4,
    "x": 8,
    "ku": 16,
}

DIRECTION_MASKS = {
    "front": 1,
    "side": 2,
    "rear": 4,
}

REQUIRED_TRANSITION_CATEGORIES = frozenset(
    {
        "multi_alert_to_single",
        "single_alert_to_multi",
        "high_main_bars_to_low",
        "alp_badge_on_to_off",
        "obd_badge_on_to_off",
        "ble_badge_on_to_off",
        "frequency_wide_to_narrow",
        "frequency_alpha_to_numeric",
        "flashing_to_steady",
    }
)

# Categories that must additionally produce category-specific reference
# regions (frequency rect / authored flash regions). Every category gets the
# blanket deterministic dirty-vs-clean regions and a clean reference capture;
# see deterministic_reference_regions() and verify_transition().
CLEAN_REFERENCE_TRANSITION_CATEGORIES = frozenset(
    {
        "frequency_wide_to_narrow",
        "frequency_alpha_to_numeric",
        "flashing_to_steady",
    }
)

FILLED_MIN_COVERAGE = 0.90
INACTIVE_MIN_COVERAGE = 0.98
TEXT_MIN_COVERAGE = 0.02
TEXT_MAX_COVERAGE = 0.90

# Band-letter floors are host-owned: the firmware manifest may declare
# stricter values, never weaker. Bench run 8a599c91 passed six framebuffer
# band-letter chops because the manifest's 0.01 floors governed. Measured
# baselines across that run's 245 healthy band cells: expected-color
# coverage >= 0.427 and glyph bbox span >= 0.967 per axis; the chopped
# letters measured 0.372 coverage and 0.714 height span. The floors below
# sit between those populations with margin on both sides. Do not lower
# them to make a bench run pass; a failing band cell is the harness doing
# its job. See docs/reviews/2026-07-10-display-visual-check-usefulness.md.
BAND_MIN_COVERAGE = 0.35
BAND_GLYPH_MIN_SPAN = 0.90


class ProtocolError(RuntimeError):
    """A device response or manifest violates the visual-verification contract."""


@dataclass(frozen=True)
class Rect:
    x: int
    y: int
    w: int
    h: int


@dataclass
class Framebuffer:
    width: int
    height: int
    pixels: list[int]

    def pixel(self, x: int, y: int) -> int:
        return self.pixels[y * self.width + x]

    def set_pixel(self, x: int, y: int, color: int) -> None:
        self.pixels[y * self.width + x] = color

    def rect_pixels(self, rect: Rect) -> list[int]:
        assert_rect_in_bounds(rect, self.width, self.height, "framebuffer")
        values: list[int] = []
        for y in range(rect.y, rect.y + rect.h):
            start = y * self.width + rect.x
            values.extend(self.pixels[start : start + rect.w])
        return values


@dataclass
class Failure:
    element: str
    message: str


@dataclass
class CaptureRecord:
    capture_id: str
    kind: str
    step_index: int
    step_id: str
    render_seq: int | None = None
    transition: str = ""
    path: str = ""
    sha256: str = ""


@dataclass
class TransitionCase:
    category: str
    source_index: int
    target_index: int
    source_id: str
    target_id: str


@dataclass
class TransitionRecord:
    category: str
    source_index: int
    target_index: int
    source_id: str
    target_id: str
    stale_regions_checked: int = 0
    result: str = "PASS"
    failures: list[str] = field(default_factory=list)


@dataclass
class CleanupRegion:
    element: str
    rect: Rect
    source_kind: str
    source_color: int
    source_min_coverage: float
    source_max_coverage: float | None
    target_color: int
    target_min_coverage: float
    allowed_target_colors: set[int]


@dataclass
class VerificationSummary:
    result: str = "PASS"
    run_id: str = ""
    started_at_utc: str = ""
    completed_at_utc: str = ""
    verifier_sha256: str = ""
    verification_completed: bool = False
    interrupted: bool = False
    exit_code: int | None = None
    artifacts_requested: bool = False
    artifacts_finalized: bool = False
    steps_expected: int = 0
    steps_checked: int = 0
    transitions_expected: int = 0
    transitions_checked: int = 0
    cleanup_attempted: bool = False
    cleanup_succeeded: bool = False
    cleanup_response: dict[str, Any] = field(default_factory=dict)
    assertion_count: int = 0
    checked_regions: int = 0
    checked_pixels: int = 0
    mask_pixels: int = 0
    flush_shadow_checked: bool = False
    flush_shadow_compares: int = 0
    l2_decodes: int = 0
    l2_decodes_expected: int = 0
    failures: list[Failure] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    captures: list[CaptureRecord] = field(default_factory=list)
    transition_records: list[TransitionRecord] = field(default_factory=list)
    binding: dict[str, Any] = field(default_factory=dict)
    options: dict[str, Any] = field(default_factory=dict)

    def add_failure(self, element: str, message: str) -> None:
        if not self.errors:
            self.result = "FAIL"
        self.failures.append(Failure(element, message))

    def add_error(self, message: str) -> None:
        self.result = "ERROR"
        self.errors.append(message)

    def finalize_result(self) -> None:
        if self.errors:
            self.result = "ERROR"
        elif self.failures:
            self.result = "FAIL"
        else:
            self.result = "PASS"

    def scope_label(self) -> str:
        levels = []
        if self.steps_checked or self.transitions_checked or self.assertion_count:
            levels.append("l1")
        if self.l2_decodes:
            levels.append("l2")
        if self.transitions_checked:
            levels.append("l3")
        return "framebuffer-semantic-" + ("-".join(levels) or "none") + (
            "+flushshadow" if self.flush_shadow_checked else ""
        )

    def to_json(self) -> dict[str, Any]:
        return {
            "schema_version": 1,
            "result": self.result,
            "run_id": self.run_id,
            "started_at_utc": self.started_at_utc,
            "completed_at_utc": self.completed_at_utc,
            "verifier_sha256": self.verifier_sha256,
            "verification_completed": self.verification_completed,
            "interrupted": self.interrupted,
            "exit_code": self.exit_code,
            "artifacts_requested": self.artifacts_requested,
            "artifacts_finalized": self.artifacts_finalized,
            "steps_expected": self.steps_expected,
            "steps_checked": self.steps_checked,
            "transitions_expected": self.transitions_expected,
            "transitions_checked": self.transitions_checked,
            "cleanup_attempted": self.cleanup_attempted,
            "cleanup_succeeded": self.cleanup_succeeded,
            "cleanup_response": self.cleanup_response,
            "binding": self.binding,
            "options": self.options,
            "required_transition_categories": sorted(REQUIRED_TRANSITION_CATEGORIES),
            "verdict_scope": {
                "level": self.scope_label(),
                "goldens_checked": False,
                # The flush shadow proves delivery to the panel interface,
                # not the glass itself — bezel/controller defects still need
                # the operator eyes-on-glass step.
                "flush_shadow_checked": self.flush_shadow_checked,
                "physical_panel_checked": False,
            },
            "assertion_count": self.assertion_count,
            "checked_regions": self.checked_regions,
            "checked_pixels": self.checked_pixels,
            "mask_pixels": self.mask_pixels,
            "flush_shadow_compares": self.flush_shadow_compares,
            "l2_decodes": self.l2_decodes,
            "l2_decodes_expected": self.l2_decodes_expected,
            "failures": [failure.__dict__ for failure in self.failures],
            "errors": self.errors,
            "captures": [capture.__dict__ for capture in self.captures],
            "transitions": [transition.__dict__ for transition in self.transition_records],
            "thresholds": {
                "filled_min_coverage": FILLED_MIN_COVERAGE,
                "inactive_min_coverage": INACTIVE_MIN_COVERAGE,
                "text_min_coverage": TEXT_MIN_COVERAGE,
                "text_max_coverage": TEXT_MAX_COVERAGE,
                # Host-owned band floors; manifest values may strengthen,
                # never weaken (verify_step clamps with max()).
                "band_min_coverage": BAND_MIN_COVERAGE,
                "band_glyph_min_span": BAND_GLYPH_MIN_SPAN,
            },
        }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", help="Device host/IP or URL, for example 192.168.4.1")
    parser.add_argument("--step", type=int, help="Verify exactly one preview step")
    parser.add_argument("--filter", default="", help="Verify steps whose id contains this substring")
    parser.add_argument("--transitions-only", action="store_true", help="Run only curated Phase 3 transitions")
    parser.add_argument("--no-report", action="store_true", help="Suppress the HTML report when --output-dir is used")
    parser.add_argument("--output-dir", default="", help="Directory for the JSON report and PNG captures")
    parser.add_argument("--goldens", action="store_true", help="Phase 4 golden drift mode")
    parser.add_argument("--update-goldens", action="store_true", help="Phase 4 golden update mode")
    parser.add_argument("--strict-masks", action="store_true", help="Fail when any masks are declared")
    parser.add_argument(
        "--no-flush-shadow",
        action="store_true",
        help="Skip the flush-shadow == framebuffer comparison (compatibility escape for "
        "firmware without /api/display/visual/flushshadow; reduces verdict scope and is "
        "recorded in the summary)",
    )
    return parser.parse_args(argv)


def require_dict(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ProtocolError(f"{path} must be an object")
    return value


def require_list(value: Any, path: str) -> list[Any]:
    if not isinstance(value, list):
        raise ProtocolError(f"{path} must be an array")
    return value


def require_bool(value: Any, path: str) -> bool:
    if not isinstance(value, bool):
        raise ProtocolError(f"{path} must be a boolean")
    return value


def require_int(value: Any, path: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ProtocolError(f"{path} must be an integer")
    return value


def require_str(value: Any, path: str) -> str:
    if not isinstance(value, str):
        raise ProtocolError(f"{path} must be a string")
    return value


def parse_color(value: Any, path: str) -> int:
    text = require_str(value, path).strip()
    try:
        if text.lower().startswith("0x"):
            color = int(text, 16)
        else:
            color = int(text)
    except ValueError as exc:
        raise ProtocolError(f"{path} must be an RGB565 color string") from exc
    if color < 0 or color > 0xFFFF:
        raise ProtocolError(f"{path} color is out of RGB565 range")
    return color


def parse_rect(value: Any, path: str) -> Rect:
    obj = require_dict(value, path)
    rect = Rect(
        x=require_int(obj.get("x"), f"{path}.x"),
        y=require_int(obj.get("y"), f"{path}.y"),
        w=require_int(obj.get("w"), f"{path}.w"),
        h=require_int(obj.get("h"), f"{path}.h"),
    )
    if rect.w <= 0 or rect.h <= 0:
        raise ProtocolError(f"{path} must have positive width and height")
    return rect


def assert_rect_in_bounds(rect: Rect, width: int, height: int, path: str) -> None:
    if rect.x < 0 or rect.y < 0 or rect.x + rect.w > width or rect.y + rect.h > height:
        raise ProtocolError(f"{path} rect is out of bounds: {rect}")
    if rect.w * rect.h >= width * height:
        raise ProtocolError(f"{path} rect cannot cover the full screen")


def normalize_key(value: Any) -> str:
    return str(value or "").strip().lower()


def binding_tuple(manifest: dict[str, Any], path: str) -> tuple[int, str, str, str]:
    return (
        require_int(manifest.get("schemaVersion"), f"{path}.schemaVersion"),
        require_str(manifest.get("firmwareVersion"), f"{path}.firmwareVersion"),
        require_str(manifest.get("firmwareSha"), f"{path}.firmwareSha"),
        require_str(manifest.get("settingsFingerprint"), f"{path}.settingsFingerprint"),
    )


def validate_steps_manifest(payload: Any) -> dict[str, Any]:
    manifest = require_dict(payload, "steps")
    if require_str(manifest.get("manifest"), "steps.manifest") != "display-visual-steps":
        raise ProtocolError("steps.manifest must be display-visual-steps")
    require_bool(manifest.get("complete"), "steps.complete")
    if manifest["complete"] is not True:
        raise ProtocolError("steps.complete marker must be true")
    binding_tuple(manifest, "steps")

    steps = require_list(manifest.get("steps"), "steps.steps")
    step_count = require_int(manifest.get("stepCount"), "steps.stepCount")
    if step_count != len(steps):
        raise ProtocolError(f"steps.stepCount {step_count} does not match {len(steps)} steps")
    if not steps:
        raise ProtocolError("steps manifest contains zero steps")

    seen: set[int] = set()
    for i, raw_step in enumerate(steps):
        step = require_dict(raw_step, f"steps.steps[{i}]")
        index = require_int(step.get("index"), f"steps.steps[{i}].index")
        if index in seen:
            raise ProtocolError(f"duplicate step index {index}")
        seen.add(index)
        require_str(step.get("id"), f"steps.steps[{i}].id")
        resolved = require_dict(step.get("resolved"), f"steps.steps[{i}].resolved")
        for alert_name in ("primary", "secondary", "third"):
            alert = require_dict(resolved.get(alert_name), f"steps.steps[{i}].resolved.{alert_name}")
            require_bool(alert.get("present"), f"steps.steps[{i}].resolved.{alert_name}.present")
            require_str(alert.get("band"), f"steps.steps[{i}].resolved.{alert_name}.band")
            require_str(alert.get("direction"), f"steps.steps[{i}].resolved.{alert_name}.direction")
            require_str(alert.get("frequencyText"), f"steps.steps[{i}].resolved.{alert_name}.frequencyText")
            require_int(alert.get("cardBarCount"), f"steps.steps[{i}].resolved.{alert_name}.cardBarCount")
        for field_name in (
            "activeBandMask",
            "activeDirectionMask",
            "flashMask",
            "bandFlashMask",
            "mainMeterCount",
            "alertCount",
        ):
            require_int(resolved.get(field_name), f"steps.steps[{i}].resolved.{field_name}")
        require_bool(resolved.get("muted"), f"steps.steps[{i}].resolved.muted")
        require_bool(resolved.get("photo"), f"steps.steps[{i}].resolved.photo")
        require_str(resolved.get("frequencyRole"), f"steps.steps[{i}].resolved.frequencyRole")
        status = require_dict(resolved.get("status"), f"steps.steps[{i}].resolved.status")
        require_bool(status.get("muted"), f"steps.steps[{i}].resolved.status.muted")
        for field_name in ("bogeyChar", "modeChar"):
            status_char = require_str(
                status.get(field_name),
                f"steps.steps[{i}].resolved.status.{field_name}",
            )
            if field_name == "bogeyChar":
                require_protocol_bogey_char(
                    status_char,
                    f"steps.steps[{i}].resolved.status.{field_name}",
                )
        for field_name in (
            "topCounterRole",
            "muteBadgeRole",
            "alpBadgeRole",
            "obdBadgeRole",
            "bleBadgeRole",
        ):
            require_str(status.get(field_name), f"steps.steps[{i}].resolved.status.{field_name}")
        for field_name in (
            "profileSlot",
            "alpState",
            "alpHbByte1",
            "obdState",
            "bleState",
            "mainVolume",
            "muteVolume",
        ):
            require_int(status.get(field_name), f"steps.steps[{i}].resolved.status.{field_name}")
    return manifest


def flatten_palette(payload: dict[str, Any]) -> dict[str, int]:
    palette = require_dict(payload.get("palette"), "layout.palette")
    flat: dict[str, int] = {}

    def visit(prefix: str, value: Any) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                child_prefix = f"{prefix}.{key}" if prefix else str(key)
                visit(child_prefix, child)
            return
        if isinstance(value, list):
            for index, child in enumerate(value):
                child_prefix = f"{prefix}.{index}" if prefix else str(index)
                visit(child_prefix, child)
            return
        flat[prefix] = parse_color(value, f"layout.palette.{prefix}")

    visit("", palette)
    if "background" not in flat:
        raise ProtocolError("layout.palette.background is required")
    return flat


def require_elements(layout: dict[str, Any]) -> dict[str, Any]:
    elements = require_dict(layout.get("elements"), "layout.elements")
    required = (
        "bandCells",
        "directionArrows",
        "mainSignalBars",
        "cardSlots",
        "cardMeterBars",
        "frequency",
        "statusText",
        "statusBadges",
    )
    for key in required:
        if key not in elements:
            raise ProtocolError(f"layout.elements.{key} is required for Phase 2 L1 assertions")
    minimum_counts = {
        "bandCells": 4,
        "directionArrows": 3,
        "mainSignalBars": 8,
        "cardSlots": 2,
        "cardMeterBars": 12,
        "statusText": 1,
        "statusBadges": 4,
    }
    for key, minimum in minimum_counts.items():
        count = len(require_list(elements.get(key), f"layout.elements.{key}"))
        if count < minimum:
            raise ProtocolError(
                f"layout.elements.{key} must contain at least {minimum} semantic element(s), found {count}"
            )
    exact_counts = {
        "bandCells": 4,
        "directionArrows": 3,
        "mainSignalBars": 8,
        "cardSlots": 2,
        "cardMeterBars": 12,
    }
    for key, expected in exact_counts.items():
        count = len(require_list(elements.get(key), f"layout.elements.{key}"))
        if count != expected:
            raise ProtocolError(f"layout.elements.{key} must contain exactly {expected} elements, found {count}")
    return elements


def validate_layout_manifest(payload: Any, *, strict_masks: bool = False) -> dict[str, Any]:
    layout = require_dict(payload, "layout")
    if require_str(layout.get("manifest"), "layout.manifest") != "display-visual-layout":
        raise ProtocolError("layout.manifest must be display-visual-layout")
    require_bool(layout.get("complete"), "layout.complete")
    if layout["complete"] is not True:
        raise ProtocolError("layout.complete marker must be true")
    binding_tuple(layout, "layout")

    screen = require_dict(layout.get("screen"), "layout.screen")
    logical = require_dict(screen.get("logical"), "layout.screen.logical")
    raw = require_dict(screen.get("raw"), "layout.screen.raw")
    if require_int(logical.get("width"), "layout.screen.logical.width") != LOGICAL_WIDTH:
        raise ProtocolError("logical framebuffer width must be 640")
    if require_int(logical.get("height"), "layout.screen.logical.height") != LOGICAL_HEIGHT:
        raise ProtocolError("logical framebuffer height must be 172")
    if require_int(raw.get("width"), "layout.screen.raw.width") != RAW_WIDTH:
        raise ProtocolError("raw framebuffer width must be 172")
    if require_int(raw.get("height"), "layout.screen.raw.height") != RAW_HEIGHT:
        raise ProtocolError("raw framebuffer height must be 640")
    if require_str(raw.get("format"), "layout.screen.raw.format") != FRAMEBUFFER_FORMAT:
        raise ProtocolError("raw framebuffer format must be RGB565LE")
    if require_str(raw.get("transform"), "layout.screen.raw.transform") != FRAMEBUFFER_TRANSFORM:
        raise ProtocolError("raw framebuffer transform must be canvas-rotation-1")

    flatten_palette(layout)
    elements = require_elements(layout)
    for key, value in elements.items():
        if key == "frequency":
            rect = parse_rect(require_dict(value, "layout.elements.frequency").get("rect"), "layout.elements.frequency.rect")
            assert_rect_in_bounds(rect, LOGICAL_WIDTH, LOGICAL_HEIGHT, "layout.elements.frequency")
            continue
        for index, item in enumerate(require_list(value, f"layout.elements.{key}")):
            element = require_dict(item, f"layout.elements.{key}[{index}]")
            rect = parse_rect(element.get("rect"), f"layout.elements.{key}[{index}].rect")
            assert_rect_in_bounds(rect, LOGICAL_WIDTH, LOGICAL_HEIGHT, f"layout.elements.{key}[{index}]")
            for supplemental_rect_name in ("emptyRect", "coverageRect"):
                if supplemental_rect_name not in element:
                    continue
                supplemental_rect = parse_rect(
                    element.get(supplemental_rect_name),
                    f"layout.elements.{key}[{index}].{supplemental_rect_name}",
                )
                assert_rect_in_bounds(
                    supplemental_rect,
                    LOGICAL_WIDTH,
                    LOGICAL_HEIGHT,
                    f"layout.elements.{key}[{index}].{supplemental_rect_name}",
                )

    def element_objects(key: str) -> list[dict[str, Any]]:
        return [
            require_dict(item, f"layout.elements.{key}[{index}]")
            for index, item in enumerate(require_list(elements.get(key), f"layout.elements.{key}"))
        ]

    band_cells = element_objects("bandCells")
    band_indexes = {
        require_int(item.get("index"), "layout.elements.bandCells[].index")
        for item in band_cells
    }
    if band_indexes != set(range(4)):
        raise ProtocolError(f"layout.elements.bandCells indexes must be 0..3, found {sorted(band_indexes)}")
    expected_band_masks = {0: 1, 1: 2, 2: 4 | 16, 3: 8}
    actual_band_masks = {
        require_int(item.get("index"), "layout.elements.bandCells[].index"):
        require_int(item.get("bandMask"), "layout.elements.bandCells[].bandMask")
        for item in band_cells
    }
    if actual_band_masks != expected_band_masks:
        raise ProtocolError(
            "layout.elements.bandCells band masks must map L/Ka/K+Ku/X to "
            f"{expected_band_masks}, found {actual_band_masks}"
        )

    directions = {
        normalize_key(item.get("direction") or item.get("id"))
        for item in element_objects("directionArrows")
    }
    if not {"front", "side", "rear"}.issubset(directions):
        raise ProtocolError(f"layout.elements.directionArrows must include front/side/rear, found {sorted(directions)}")

    main_bar_indexes = {
        require_int(item.get("index"), "layout.elements.mainSignalBars[].index")
        for item in element_objects("mainSignalBars")
    }
    if main_bar_indexes != set(range(8)):
        raise ProtocolError(
            f"layout.elements.mainSignalBars indexes must be 0..7, found {sorted(main_bar_indexes)}"
        )

    card_slots = element_objects("cardSlots")
    slot_indexes = {
        require_int(item.get("slot"), "layout.elements.cardSlots[].slot")
        for item in card_slots
    }
    if slot_indexes != {0, 1}:
        raise ProtocolError(f"layout.elements.cardSlots slots must be 0 and 1, found {sorted(slot_indexes)}")
    for index, slot in enumerate(card_slots):
        if "emptyRect" not in slot:
            raise ProtocolError(f"layout.elements.cardSlots[{index}].emptyRect is required")

    meter_keys = {
        (
            require_int(item.get("slot"), "layout.elements.cardMeterBars[].slot"),
            require_int(item.get("index"), "layout.elements.cardMeterBars[].index"),
        )
        for item in element_objects("cardMeterBars")
    }
    expected_meter_keys = {(slot, index) for slot in range(2) for index in range(6)}
    if meter_keys != expected_meter_keys:
        raise ProtocolError("layout.elements.cardMeterBars must contain every unique slot/index pair")

    frequency = require_dict(elements.get("frequency"), "layout.elements.frequency")
    require_str(frequency.get("roleSource"), "layout.elements.frequency.roleSource")

    badge_ids = {
        require_str(item.get("id"), "layout.elements.statusBadges[].id")
        for item in element_objects("statusBadges")
    }
    required_badges = {"mute", "alp", "obd", "ble"}
    if not required_badges.issubset(badge_ids):
        raise ProtocolError(
            f"layout.elements.statusBadges must include mute/alp/obd/ble, found {sorted(badge_ids)}"
        )
    for index, badge in enumerate(element_objects("statusBadges")):
        require_str(badge.get("roleSource"), f"layout.elements.statusBadges[{index}].roleSource")

    overlaps = require_list(layout.get("overlaps", []), "layout.overlaps")
    for index, raw_overlap in enumerate(overlaps):
        overlap = require_dict(raw_overlap, f"layout.overlaps[{index}]")
        require_str(overlap.get("id"), f"layout.overlaps[{index}].id")
        require_str(overlap.get("reason"), f"layout.overlaps[{index}].reason")
        overlap_elements = require_list(overlap.get("elements"), f"layout.overlaps[{index}].elements")
        if len(overlap_elements) < 2:
            raise ProtocolError(f"layout.overlaps[{index}].elements must name at least two elements")
        for element_index, element_name in enumerate(overlap_elements):
            require_str(element_name, f"layout.overlaps[{index}].elements[{element_index}]")

    masks = require_list(layout.get("masks", []), "layout.masks")
    if strict_masks and masks:
        raise ProtocolError("--strict-masks forbids non-empty layout.masks")
    for index, raw_mask in enumerate(masks):
        mask = require_dict(raw_mask, f"layout.masks[{index}]")
        require_str(mask.get("reason"), f"layout.masks[{index}].reason")
        require_str(mask.get("date"), f"layout.masks[{index}].date")
        require_str(mask.get("owner"), f"layout.masks[{index}].owner")
        require_str(mask.get("evidence"), f"layout.masks[{index}].evidence")
        rect = parse_rect(mask.get("rect"), f"layout.masks[{index}].rect")
        assert_rect_in_bounds(rect, LOGICAL_WIDTH, LOGICAL_HEIGHT, f"layout.masks[{index}]")
    return layout


def validate_manifest_binding(steps: dict[str, Any], layout: dict[str, Any]) -> tuple[int, str, str, str]:
    step_binding = binding_tuple(steps, "steps")
    layout_binding = binding_tuple(layout, "layout")
    if step_binding != layout_binding:
        raise ProtocolError(f"manifest binding mismatch: steps={step_binding} layout={layout_binding}")

    palette = flatten_palette(layout)
    elements = require_elements(layout)
    for group_name, static_role_fields in (
        ("bandCells", ("activeRole", "inactiveRole")),
        ("directionArrows", ("activeRole", "inactiveRole")),
        ("mainSignalBars", ("activeRole", "inactiveRole")),
        ("cardSlots", ("textRole",)),
        ("cardMeterBars", ("activeRole", "inactiveRole")),
        ("statusText", ("textRole",)),
        ("statusBadges", ("activeRole", "inactiveRole")),
    ):
        for index, raw_element in enumerate(require_list(elements.get(group_name), f"layout.elements.{group_name}")):
            element = require_dict(raw_element, f"layout.elements.{group_name}[{index}]")
            for field_name in static_role_fields:
                if field_name in element:
                    role = require_str(
                        element.get(field_name),
                        f"layout.elements.{group_name}[{index}].{field_name}",
                    )
                    role_color(palette, role, f"layout.elements.{group_name}[{index}]")

    frequency = require_dict(elements.get("frequency"), "layout.elements.frequency")
    frequency_source = require_str(frequency.get("roleSource"), "layout.elements.frequency.roleSource")
    status_text = [
        require_dict(item, f"layout.elements.statusText[{index}]")
        for index, item in enumerate(require_list(elements.get("statusText"), "layout.elements.statusText"))
    ]
    status_badges = [
        require_dict(item, f"layout.elements.statusBadges[{index}]")
        for index, item in enumerate(require_list(elements.get("statusBadges"), "layout.elements.statusBadges"))
    ]
    for step_index_value, raw_step in enumerate(require_list(steps.get("steps"), "steps.steps")):
        step = require_dict(raw_step, f"steps.steps[{step_index_value}]")
        resolved = require_dict(step.get("resolved"), f"steps.steps[{step_index_value}].resolved")
        status = require_dict(resolved.get("status"), f"steps.steps[{step_index_value}].resolved.status")
        if frequency_source not in resolved:
            raise ProtocolError(
                f"steps.steps[{step_index_value}].resolved is missing frequency role source {frequency_source!r}"
            )
        frequency_role = require_str(
            resolved[frequency_source],
            f"steps.steps[{step_index_value}].resolved.{frequency_source}",
        )
        role_color(palette, frequency_role, f"steps.steps[{step_index_value}].resolved.{frequency_source}")
        for group_name, items in (("statusText", status_text), ("statusBadges", status_badges)):
            for element_index, item in enumerate(items):
                if "roleSource" not in item:
                    continue
                source = require_str(
                    item.get("roleSource"),
                    f"layout.elements.{group_name}[{element_index}].roleSource",
                )
                if source not in status:
                    raise ProtocolError(
                        f"steps.steps[{step_index_value}].resolved.status is missing role source {source!r}"
                    )
                dynamic_role = require_str(
                    status[source],
                    f"steps.steps[{step_index_value}].resolved.status.{source}",
                )
                role_color(
                    palette,
                    dynamic_role,
                    f"steps.steps[{step_index_value}].resolved.status.{source}",
                )
    return step_binding


def role_color(palette: dict[str, int], role: str, path: str) -> int:
    normalized = role.strip()
    if normalized in palette:
        return palette[normalized]
    dotted = normalized.replace("[", ".").replace("]", "")
    if dotted in palette:
        return palette[dotted]
    raise ProtocolError(f"{path} references unknown palette role {role!r}")


def rect_mask(rect: Rect, width: int) -> set[int]:
    indexes: set[int] = set()
    for y in range(rect.y, rect.y + rect.h):
        start = y * width + rect.x
        indexes.update(range(start, start + rect.w))
    return indexes


def decode_rgb565le(data: bytes, raw_width: int, raw_height: int) -> list[int]:
    expected_len = raw_width * raw_height * 2
    if len(data) != expected_len:
        raise ProtocolError(f"framebuffer byte length {len(data)} != expected {expected_len}")
    colors: list[int] = []
    for i in range(0, len(data), 2):
        colors.append(data[i] | (data[i + 1] << 8))
    return colors


def decode_framebuffer(data: bytes, headers: dict[str, str], binding: tuple[int, str, str, str] | None = None) -> Framebuffer:
    raw_width = int_header(headers, "X-FB-Raw-Width")
    raw_height = int_header(headers, "X-FB-Raw-Height")
    logical_width = int_header(headers, "X-FB-Logical-Width")
    logical_height = int_header(headers, "X-FB-Logical-Height")
    if raw_width != RAW_WIDTH or raw_height != RAW_HEIGHT:
        raise ProtocolError("framebuffer raw dimensions do not match the display contract")
    if logical_width != LOGICAL_WIDTH or logical_height != LOGICAL_HEIGHT:
        raise ProtocolError("framebuffer logical dimensions do not match the display contract")
    if headers.get("X-FB-Format") != FRAMEBUFFER_FORMAT:
        raise ProtocolError("framebuffer format must be RGB565LE")
    if headers.get("X-FB-Transform") != FRAMEBUFFER_TRANSFORM:
        raise ProtocolError("framebuffer transform must be canvas-rotation-1")
    if binding is not None:
        frame_binding = (
            int_header(headers, "X-Display-Manifest-Schema-Version"),
            required_header(headers, "X-Display-Firmware-Version"),
            required_header(headers, "X-Display-Firmware-Sha"),
            required_header(headers, "X-Display-Settings-Fingerprint"),
        )
        if frame_binding != binding:
            raise ProtocolError(f"framebuffer binding mismatch: frame={frame_binding} manifests={binding}")

    raw_pixels = decode_rgb565le(data, raw_width, raw_height)
    logical_pixels = [0] * (logical_width * logical_height)
    for ly in range(logical_height):
        for lx in range(logical_width):
            px = raw_width - 1 - ly
            py = lx
            logical_pixels[ly * logical_width + lx] = raw_pixels[py * raw_width + px]
    return Framebuffer(width=logical_width, height=logical_height, pixels=logical_pixels)


def int_header(headers: dict[str, str], name: str) -> int:
    value = required_header(headers, name)
    try:
        return int(value)
    except ValueError as exc:
        raise ProtocolError(f"{name} must be an integer") from exc


def required_header(headers: dict[str, str], name: str) -> str:
    value = headers.get(name)
    if value is None or value == "":
        raise ProtocolError(f"missing required framebuffer header {name}")
    return value


def analyze_rect(frame: Framebuffer, rect: Rect, expected: int, background: int) -> tuple[int, int, int, Counter[int]]:
    pixels = frame.rect_pixels(rect)
    counts = Counter(pixels)
    total = len(pixels)
    expected_count = counts.get(expected, 0)
    non_background = total - counts.get(background, 0)
    return total, expected_count, non_background, counts


def add_region_stats(summary: VerificationSummary, rect: Rect) -> None:
    summary.assertion_count += 1
    summary.checked_regions += 1
    summary.checked_pixels += rect.w * rect.h


def assert_color_coverage(
    frame: Framebuffer,
    summary: VerificationSummary,
    *,
    element: str,
    rect: Rect,
    expected_color: int,
    background_color: int,
    min_coverage: float,
    allowed_colors: set[int] | None = None,
) -> None:
    add_region_stats(summary, rect)
    total, expected_count, _, counts = analyze_rect(frame, rect, expected_color, background_color)
    coverage = expected_count / total
    if coverage < min_coverage:
        summary.add_failure(
            element,
            f"expected color 0x{expected_color:04X} coverage >= {min_coverage:.0%}, found {coverage:.1%}",
        )
    if allowed_colors is not None:
        bad = set(counts) - allowed_colors
        if bad:
            rendered = ", ".join(f"0x{color:04X}" for color in sorted(bad)[:4])
            summary.add_failure(element, f"unexpected palette color(s) in primitive region: {rendered}")


def assert_text_coverage(
    frame: Framebuffer,
    summary: VerificationSummary,
    *,
    element: str,
    rect: Rect,
    text_color: int,
    background_color: int,
    min_coverage: float,
    max_coverage: float,
) -> None:
    add_region_stats(summary, rect)
    total, text_pixels, _, _ = analyze_rect(frame, rect, text_color, background_color)
    coverage = text_pixels / total
    if coverage < min_coverage or coverage > max_coverage:
        summary.add_failure(
            element,
            "expected text-role color "
            f"0x{text_color:04X} coverage {min_coverage:.1%}..{max_coverage:.1%}, found {coverage:.1%}",
        )


# ---------------------------------------------------------------------------
# L2 — exact numeric-frequency and bogey-character decoding.
#
# The design doc sketched L2 as template matching against a firmware-exported
# digit atlas. That atlas IS the render cache: matching pixels against the
# data that drew them cannot catch a corrupted cache. The host instead maps
# seven independently sampled zones to protocol characters. Frequency glyphs
# are located and contacted digits are split at ink valleys; the qualified
# top-counter path samples a fixed two-dimensional V1SevenX cell so missing,
# added, mirrored, or translated segments cannot be hidden by bbox
# normalization. Archived run d1da756f replays 39/39 numeric frequency texts
# and 61/61 counter symbols (44 steps plus 17 transition targets), for 100/100
# L2 decodes with zero failures. A new run bound to the final committed
# firmware and verifier is still required for acceptance of the hardened path.
# ---------------------------------------------------------------------------

# packet_parser.cpp treats bits 0..6 as A..G. Keep the protocol masks visible
# here instead of hand-transcribing tuples: this is the complete recognized
# nonblank character domain that decodeBogeyCounterByte() can expose to the
# manifest. Packet bit 7 is the separate, currently unexported counter-dot bit.
SEG7_PROTOCOL_MASKS = {
    0x3F: "0",
    0x06: "1",
    0x5B: "2",
    0x4F: "3",
    0x66: "4",
    0x6D: "5",
    0x7D: "6",
    0x07: "7",
    0x7F: "8",
    0x6F: "9",
    0x18: "&",
    0x1C: "u",
    0x1E: "J",
    0x38: "L",
    0x39: "C",
    0x3E: "U",
    0x49: "#",
    0x58: "c",
    0x5E: "d",
    0x71: "F",
    0x73: "P",
    0x77: "A",
    0x79: "E",
    0x7C: "b",
}


def seg7_pattern_from_mask(mask: int) -> tuple[int, ...]:
    """Return protocol bits A..G in the decoder's zone order."""
    return tuple((mask >> bit) & 1 for bit in range(7))


SEG7_PATTERNS = {
    seg7_pattern_from_mask(mask): char for mask, char in SEG7_PROTOCOL_MASKS.items()
}
# The renderer also accepts a middle-bar minus sign. It is useful to the
# generic text decoder but is deliberately not in the packet-parser bogey
# domain used by the fail-closed manifest check below.
SEG7_PATTERNS[seg7_pattern_from_mask(0x40)] = "-"
SEG7_PROTOCOL_CHARS = frozenset(SEG7_PROTOCOL_MASKS.values())
# The embedded TTF's full glyphs top out near 0.62x their height, so a wider
# production run contains kerned-into-contact glyphs.
SEG7_SPLIT_RATIO = 0.72
SEG7_SMALL_GLYPH_SPLIT_RATIO = 0.95
SEG7_SMALL_GLYPH_MAX_HEIGHT = 24
SEG7_MIN_FREQUENCY_GLYPH_HEIGHT_RATIO = 0.75

# The qualified top-counter renderer is the embedded V1SevenX face at 60 px
# inside DisplayLayout::kTopCounterRect (68 px high). Sampling this host-owned
# physical LED-cell envelope, rather than normalizing the glyph's ink bbox,
# keeps sparse legitimate symbols exact and makes every added/removed segment
# observable. If the embedded face fails and firmware falls back to different
# geometry, L2 intentionally fails closed until that renderer is qualified.
SEG7_COUNTER_RECT_HEIGHT = 68
SEG7_COUNTER_RECT_WIDTH = 55
SEG7_COUNTER_CELL_LEFT = 14
SEG7_COUNTER_CELL_WIDTH = 30
SEG7_COUNTER_CELL_TOP = 3
SEG7_COUNTER_CELL_HEIGHT = 48
SEG7_COUNTER_POSITION_TOLERANCE = 3
SEG7_COUNTER_SIZE_TOLERANCE = 3
SEG7_COUNTER_BOUND_TOLERANCE = 1

# Qualified inclusive ink bounds for the embedded V1SevenX face at 60 px,
# relative to the 55x68 top-counter rect. These are deliberately host-owned
# geometry, not data fetched from the live firmware cache. Pattern decoding
# proves the character; center and size checks additionally prove that the
# raster still occupies its qualified physical cell instead of being shifted
# or uniformly rescaled into the same seven sampled zones.
SEG7_COUNTER_GLYPH_BBOXES = {
    "0": (14, 3, 43, 49),
    "1": (36, 3, 43, 47),
    "2": (14, 3, 43, 49),
    "3": (14, 3, 43, 49),
    "4": (15, 4, 43, 48),
    "5": (14, 3, 42, 49),
    "6": (14, 3, 42, 49),
    "7": (17, 3, 43, 47),
    "8": (14, 3, 43, 49),
    "9": (14, 3, 43, 49),
    "&": (14, 26, 40, 49),
    "u": (14, 26, 42, 49),
    "J": (14, 3, 43, 49),
    "L": (14, 6, 40, 49),
    "C": (14, 3, 42, 49),
    "U": (14, 3, 43, 49),
    "#": (14, 3, 42, 49),
    "c": (14, 23, 40, 49),
    "d": (14, 4, 43, 49),
    "F": (14, 3, 42, 46),
    "P": (14, 3, 43, 46),
    "A": (14, 3, 43, 47),
    "E": (14, 3, 42, 49),
    "b": (14, 6, 42, 49),
}

# Numeric preview frequencies use the qualified 82 px embedded V1SevenX face
# in a fixed 305x85 lane. All accepted preview values are NN.NNN. The first
# and last digit determine the small bearing variation at the outer ink edges;
# the vertical bounds are invariant across the fixed physical corpus.
SEG7_FREQUENCY_RECT_WIDTH = 305
SEG7_FREQUENCY_RECT_HEIGHT = 85
SEG7_FREQUENCY_LEFT_BY_FIRST_DIGIT = {
    "0": 33,
    "1": 63,
    "2": 33,
    "3": 33,
    "4": 34,
    "5": 33,
    "6": 33,
    "7": 37,
    "8": 33,
    "9": 33,
}
SEG7_FREQUENCY_RIGHT_BY_LAST_DIGIT = {
    "0": 269,
    "1": 269,
    "2": 269,
    "3": 269,
    "4": 269,
    "5": 267,
    "6": 267,
    "7": 269,
    "8": 269,
    "9": 269,
}
SEG7_FREQUENCY_TOP = 16
SEG7_FREQUENCY_BOTTOM = 80
SEG7_FREQUENCY_BOUND_TOLERANCE = 2
SEG7_FREQUENCY_CELL_BOUND_TOLERANCE = 1
# A one-pixel allowance at both horizontal edges would permit an 18% width
# loss on the qualified 11 px narrow `1`; the 10x10 decimal point has the same
# problem on both axes. Their deterministic bounds therefore stay exact while
# the wider/taller digit cells retain the declared one-pixel raster allowance.
SEG7_FREQUENCY_EXACT_X_BOUND_CHARS = frozenset({"1", "."})
SEG7_FREQUENCY_EXACT_Y_BOUND_CHARS = frozenset({"."})
SEG7_FREQUENCY_DIGIT_ORIGINS = {
    0: 33,
    1: 74,
    3: 134,
    4: 181,
    5: 229,
}
SEG7_FREQUENCY_DIGIT_BBOXES = {
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
SEG7_FREQUENCY_DOT_BBOX = (117, 72, 126, 81)
DISPLAY_VISUAL_EXPECTED_STEP_COUNT = 44
DISPLAY_VISUAL_EXPECTED_NUMERIC_FREQUENCY_STEP_COUNT = 39
DISPLAY_VISUAL_EXPECTED_NONBLANK_COUNTER_STEP_COUNT = 44
DISPLAY_VISUAL_EXPECTED_ALPHA_FREQUENCY_STEP_INDICES = frozenset({15, 16, 17, 30, 31})
DISPLAY_VISUAL_EXPECTED_FULL_L2_DECODES = 100
DISPLAY_VISUAL_REQUIRED_NUMERIC_FREQUENCIES = frozenset(
    {"10.525", "24.150", "24.199", "33.800", "34.700", "35.500"}
)


def require_protocol_bogey_char(value: str, path: str) -> None:
    """Require packet_parser's exact one-character bogey contract."""
    if value == " ":
        return
    if len(value) != 1 or value not in SEG7_PROTOCOL_CHARS:
        supported = "".join(sorted(SEG7_PROTOCOL_CHARS))
        raise ProtocolError(
            f"{path} has unsupported bogeyChar {value!r}; expected one "
            f"character from {supported!r} or a single blank space"
        )


def validate_protocol_bogey_corpus(steps: dict[str, Any]) -> None:
    """Require the fixed physical preview to exercise the complete L2 workload."""
    raw_steps = require_list(steps.get("steps"), "steps.steps")
    if len(raw_steps) != DISPLAY_VISUAL_EXPECTED_STEP_COUNT:
        raise ProtocolError(
            "display preview corpus has "
            f"{len(raw_steps)} steps; expected {DISPLAY_VISUAL_EXPECTED_STEP_COUNT}"
        )

    observed: set[str] = set()
    nonblank_counter_steps = 0
    numeric_frequency_steps = 0
    numeric_frequencies: set[str] = set()
    alpha_frequency_step_indices: set[int] = set()
    for index, raw_step in enumerate(raw_steps):
        step = require_dict(raw_step, f"steps.steps[{index}]")
        manifest_index = require_int(step.get("index"), f"steps.steps[{index}].index")
        if manifest_index != index:
            raise ProtocolError(
                "display preview step order drifted: "
                f"steps[{index}] declares index {manifest_index}; expected {index}"
            )
        resolved = require_dict(step.get("resolved"), f"steps.steps[{index}].resolved")
        status = require_dict(
            resolved.get("status"),
            f"steps.steps[{index}].resolved.status",
        )
        value = require_str(
            status.get("bogeyChar"),
            f"steps.steps[{index}].resolved.status.bogeyChar",
        )
        if value not in ("", " "):
            observed.add(value)
            nonblank_counter_steps += 1

        primary = require_dict(
            resolved.get("primary"),
            f"steps.steps[{index}].resolved.primary",
        )
        frequency_text = require_str(
            primary.get("frequencyText"),
            f"steps.steps[{index}].resolved.primary.frequencyText",
        )
        if not frequency_text:
            raise ProtocolError(
                f"steps.steps[{index}].resolved.primary.frequencyText must not be empty"
            )
        if seg7_is_numeric_text(frequency_text):
            if (
                len(frequency_text) != 6
                or frequency_text[2] != "."
                or not (frequency_text[:2] + frequency_text[3:]).isdigit()
            ):
                raise ProtocolError(
                    f"steps.steps[{index}].resolved.primary.frequencyText has "
                    f"unqualified numeric format {frequency_text!r}; expected NN.NNN"
                )
            numeric_frequency_steps += 1
            numeric_frequencies.add(frequency_text)
        else:
            alpha_frequency_step_indices.add(index)

    missing = sorted(SEG7_PROTOCOL_CHARS - observed)
    if missing:
        raise ProtocolError(
            "display preview corpus does not exercise the complete L2 bogey "
            f"domain; missing {missing!r}"
        )
    if nonblank_counter_steps != DISPLAY_VISUAL_EXPECTED_NONBLANK_COUNTER_STEP_COUNT:
        raise ProtocolError(
            "display preview corpus has "
            f"{nonblank_counter_steps} nonblank counter steps; expected "
            f"{DISPLAY_VISUAL_EXPECTED_NONBLANK_COUNTER_STEP_COUNT}"
        )
    if numeric_frequency_steps != DISPLAY_VISUAL_EXPECTED_NUMERIC_FREQUENCY_STEP_COUNT:
        raise ProtocolError(
            "display preview corpus has "
            f"{numeric_frequency_steps} numeric frequency steps; expected "
            f"{DISPLAY_VISUAL_EXPECTED_NUMERIC_FREQUENCY_STEP_COUNT}"
        )
    if numeric_frequencies != DISPLAY_VISUAL_REQUIRED_NUMERIC_FREQUENCIES:
        missing_frequencies = sorted(
            DISPLAY_VISUAL_REQUIRED_NUMERIC_FREQUENCIES - numeric_frequencies
        )
        unexpected_frequencies = sorted(
            numeric_frequencies - DISPLAY_VISUAL_REQUIRED_NUMERIC_FREQUENCIES
        )
        raise ProtocolError(
            "display preview numeric frequency corpus drifted: "
            f"missing={missing_frequencies!r}, unexpected={unexpected_frequencies!r}"
        )
    if alpha_frequency_step_indices != DISPLAY_VISUAL_EXPECTED_ALPHA_FREQUENCY_STEP_INDICES:
        raise ProtocolError(
            "display preview numeric/alpha step classification drifted: "
            f"observed_alpha_indices={sorted(alpha_frequency_step_indices)!r}, "
            "expected_alpha_indices="
            f"{sorted(DISPLAY_VISUAL_EXPECTED_ALPHA_FREQUENCY_STEP_INDICES)!r}"
        )


def seg7_is_numeric_text(text: str) -> bool:
    return bool(text) and all(char.isdigit() or char == "." for char in text)


def expected_l2_decodes_for_step(step: dict[str, Any]) -> int:
    """Return the exact number of L2 assertions verify_step must execute."""
    resolved = require_dict(step.get("resolved"), "step.resolved")
    primary = require_dict(resolved.get("primary"), "step.resolved.primary")
    status = require_dict(resolved.get("status"), "step.resolved.status")
    frequency_text = require_str(
        primary.get("frequencyText"),
        "step.resolved.primary.frequencyText",
    )
    bogey_char = require_str(status.get("bogeyChar"), "step.resolved.status.bogeyChar")
    return int(seg7_is_numeric_text(frequency_text)) + int(bogey_char != " ")


def expected_l2_decodes_for_run(
    selected_steps: list[dict[str, Any]],
    transition_cases: list[TransitionCase],
    steps_by_index: dict[int, dict[str, Any]],
) -> int:
    """Return the exact L2 workload for selected steps and transition targets."""
    return sum(expected_l2_decodes_for_step(step) for step in selected_steps) + sum(
        expected_l2_decodes_for_step(
            require_dict(
                steps_by_index.get(case.target_index),
                f"transition.{case.category}.target",
            )
        )
        for case in transition_cases
    )


def require_full_default_l2_workload(expected: int, *, full_default_run: bool) -> None:
    """Fail before pinning if the authored full run no longer executes 100 L2 reads."""
    if full_default_run and expected != DISPLAY_VISUAL_EXPECTED_FULL_L2_DECODES:
        raise ProtocolError(
            "full display preview L2 workload drifted: expected run would execute "
            f"{expected} decodes; required {DISPLAY_VISUAL_EXPECTED_FULL_L2_DECODES}"
        )


def scale_inclusive_bounds(
    bounds: tuple[int, int, int, int],
    *,
    width: int,
    height: int,
    reference_width: int,
    reference_height: int,
) -> tuple[int, int, int, int]:
    """Scale inclusive local-pixel bounds without losing their far edge."""
    x0, y0, x1, y1 = bounds
    return (
        round(x0 * width / reference_width),
        round(y0 * height / reference_height),
        round((x1 + 1) * width / reference_width) - 1,
        round((y1 + 1) * height / reference_height) - 1,
    )


def seg7_counter_geometry_matches(
    char: str,
    *,
    rect: Rect,
    observed: tuple[int, int, int, int],
) -> bool:
    """Check one decoded counter glyph's qualified center and ink size."""
    reference = SEG7_COUNTER_GLYPH_BBOXES.get(char)
    if reference is None:
        return False
    expected = scale_inclusive_bounds(
        reference,
        width=rect.w,
        height=rect.h,
        reference_width=SEG7_COUNTER_RECT_WIDTH,
        reference_height=SEG7_COUNTER_RECT_HEIGHT,
    )
    ox0, oy0, ox1, oy1 = observed
    ex0, ey0, ex1, ey1 = expected
    observed_width = ox1 - ox0 + 1
    observed_height = oy1 - oy0 + 1
    expected_width = ex1 - ex0 + 1
    expected_height = ey1 - ey0 + 1
    x_position_tolerance = max(
        1.0,
        rect.w * SEG7_COUNTER_POSITION_TOLERANCE / SEG7_COUNTER_RECT_WIDTH,
    )
    y_position_tolerance = max(
        1.0,
        rect.h * SEG7_COUNTER_POSITION_TOLERANCE / SEG7_COUNTER_RECT_HEIGHT,
    )
    width_tolerance = max(
        1.0,
        rect.w * SEG7_COUNTER_SIZE_TOLERANCE / SEG7_COUNTER_RECT_WIDTH,
    )
    height_tolerance = max(
        1.0,
        rect.h * SEG7_COUNTER_SIZE_TOLERANCE / SEG7_COUNTER_RECT_HEIGHT,
    )
    x_bound_tolerance = max(
        1.0,
        rect.w * SEG7_COUNTER_BOUND_TOLERANCE / SEG7_COUNTER_RECT_WIDTH,
    )
    y_bound_tolerance = max(
        1.0,
        rect.h * SEG7_COUNTER_BOUND_TOLERANCE / SEG7_COUNTER_RECT_HEIGHT,
    )
    return (
        abs((ox0 + ox1) / 2 - (ex0 + ex1) / 2) <= x_position_tolerance
        and abs((oy0 + oy1) / 2 - (ey0 + ey1) / 2) <= y_position_tolerance
        and abs(observed_width - expected_width) <= width_tolerance
        and abs(observed_height - expected_height) <= height_tolerance
        and abs(ox0 - ex0) <= x_bound_tolerance
        and abs(ox1 - ex1) <= x_bound_tolerance
        and abs(oy0 - ey0) <= y_bound_tolerance
        and abs(oy1 - ey1) <= y_bound_tolerance
    )


def seg7_non_dot_ink_bbox(
    frame: Framebuffer,
    rect: Rect,
    background: int,
) -> tuple[int, int, int, int] | None:
    """Return the local bbox of full-height frequency ink, excluding dots."""
    columns = [
        [
            y - rect.y
            for y in range(rect.y, rect.y + rect.h)
            if frame.pixel(x, y) != background
        ]
        for x in range(rect.x, rect.x + rect.w)
    ]
    runs: list[tuple[int, int]] = []
    start: int | None = None
    for index in range(rect.w + 1):
        active = bool(columns[index]) if index < rect.w else False
        if active and start is None:
            start = index
        elif not active and start is not None:
            runs.append((start, index - 1))
            start = None
    runs = [(x0, x1) for x0, x1 in runs if x0 > 2 and x1 < rect.w - 1]
    boxes: list[tuple[int, int, int, int]] = []
    for x0, x1 in runs:
        ys = [y for x in range(x0, x1 + 1) for y in columns[x]]
        if ys:
            boxes.append((x0, min(ys), x1, max(ys)))
    max_height = max((y1 - y0 + 1 for _, y0, _, y1 in boxes), default=0)
    glyph_boxes = [
        box
        for box in boxes
        if not (
            box[3] - box[1] + 1 <= 0.3 * max_height
            and box[2] - box[0] + 1 <= 0.35 * max_height
        )
    ]
    if not glyph_boxes:
        return None
    return (
        min(box[0] for box in glyph_boxes),
        min(box[1] for box in glyph_boxes),
        max(box[2] for box in glyph_boxes),
        max(box[3] for box in glyph_boxes),
    )


def seg7_frequency_reference_cells(expected_text: str) -> list[tuple[int, int, int, int]]:
    """Return qualified inclusive local bounds for each DD.DDD glyph cell."""
    cells: list[tuple[int, int, int, int]] = []
    for index, char in enumerate(expected_text):
        if index == 2:
            cells.append(SEG7_FREQUENCY_DOT_BBOX)
            continue
        origin = SEG7_FREQUENCY_DIGIT_ORIGINS[index]
        x0, y0, x1, y1 = SEG7_FREQUENCY_DIGIT_BBOXES[char]
        cells.append((origin + x0, y0, origin + x1, y1))
    return cells


def seg7_frequency_cell_geometry_error(
    frame: Framebuffer,
    rect: Rect,
    background: int,
    expected_text: str,
) -> str | None:
    """Check every frequency glyph's fixed cell position and size."""
    expected_cells = [
        scale_inclusive_bounds(
            bounds,
            width=rect.w,
            height=rect.h,
            reference_width=SEG7_FREQUENCY_RECT_WIDTH,
            reference_height=SEG7_FREQUENCY_RECT_HEIGHT,
        )
        for bounds in seg7_frequency_reference_cells(expected_text)
    ]
    x_tolerance = max(
        1,
        round(
            SEG7_FREQUENCY_CELL_BOUND_TOLERANCE
            * rect.w
            / SEG7_FREQUENCY_RECT_WIDTH
        ),
    )
    y_tolerance = max(
        1,
        round(
            SEG7_FREQUENCY_CELL_BOUND_TOLERANCE
            * rect.h
            / SEG7_FREQUENCY_RECT_HEIGHT
        ),
    )

    # Any non-background frequency ink must belong to one of the six
    # qualified cells. The three left-edge columns remain the declared Ka
    # overlap handled by the decoder's existing edge-run rule.
    for local_y in range(rect.h):
        for local_x in range(3, rect.w - 1):
            if frame.pixel(rect.x + local_x, rect.y + local_y) == background:
                continue
            if not any(
                x0 <= local_x <= x1 and y0 <= local_y <= y1
                for x0, y0, x1, y1 in expected_cells
            ):
                return (
                    "numeric frequency has ink outside its qualified glyph "
                    f"cells at local pixel ({local_x}, {local_y})"
                )

    for index, (char, expected) in enumerate(zip(expected_text, expected_cells)):
        ex0, ey0, ex1, ey1 = expected
        points = [
            (local_x, local_y)
            for local_y in range(ey0, ey1 + 1)
            for local_x in range(ex0, ex1 + 1)
            if frame.pixel(rect.x + local_x, rect.y + local_y) != background
        ]
        if not points:
            return f"numeric frequency cell {index} ({char!r}) has no ink"
        observed = (
            min(x for x, _ in points),
            min(y for _, y in points),
            max(x for x, _ in points),
            max(y for _, y in points),
        )
        cell_x_tolerance = (
            0 if char in SEG7_FREQUENCY_EXACT_X_BOUND_CHARS else x_tolerance
        )
        cell_y_tolerance = (
            0 if char in SEG7_FREQUENCY_EXACT_Y_BOUND_CHARS else y_tolerance
        )
        deltas = tuple(actual - wanted for actual, wanted in zip(observed, expected))
        if (
            abs(deltas[0]) > cell_x_tolerance
            or abs(deltas[2]) > cell_x_tolerance
            or abs(deltas[1]) > cell_y_tolerance
            or abs(deltas[3]) > cell_y_tolerance
        ):
            return (
                f"numeric frequency cell {index} ({char!r}) ink geometry is "
                f"outside qualified bounds: observed={observed}, "
                f"expected={expected}, tolerance=({cell_x_tolerance}px x, "
                f"{cell_y_tolerance}px y)"
            )
    return None


def seg7_frequency_geometry_error(
    frame: Framebuffer,
    rect: Rect,
    background: int,
    expected_text: str,
) -> str | None:
    """Return a fail-closed geometry error for the qualified NN.NNN lane."""
    if (
        len(expected_text) != 6
        or expected_text[2] != "."
        or not (expected_text[:2] + expected_text[3:]).isdigit()
    ):
        return f"unqualified numeric frequency format {expected_text!r}; expected NN.NNN"
    observed = seg7_non_dot_ink_bbox(frame, rect, background)
    if observed is None:
        return "numeric frequency has no full-height glyph ink"
    reference = (
        SEG7_FREQUENCY_LEFT_BY_FIRST_DIGIT[expected_text[0]],
        SEG7_FREQUENCY_TOP,
        SEG7_FREQUENCY_RIGHT_BY_LAST_DIGIT[expected_text[-1]],
        SEG7_FREQUENCY_BOTTOM,
    )
    expected = scale_inclusive_bounds(
        reference,
        width=rect.w,
        height=rect.h,
        reference_width=SEG7_FREQUENCY_RECT_WIDTH,
        reference_height=SEG7_FREQUENCY_RECT_HEIGHT,
    )
    x_tolerance = max(
        1,
        round(
            SEG7_FREQUENCY_BOUND_TOLERANCE
            * rect.w
            / SEG7_FREQUENCY_RECT_WIDTH
        ),
    )
    y_tolerance = max(
        1,
        round(
            SEG7_FREQUENCY_BOUND_TOLERANCE
            * rect.h
            / SEG7_FREQUENCY_RECT_HEIGHT
        ),
    )
    deltas = tuple(actual - wanted for actual, wanted in zip(observed, expected))
    if (
        abs(deltas[0]) > x_tolerance
        or abs(deltas[2]) > x_tolerance
        or abs(deltas[1]) > y_tolerance
        or abs(deltas[3]) > y_tolerance
    ):
        return (
            "numeric frequency ink geometry is outside the qualified "
            f"V1SevenX bounds: observed={observed}, expected={expected}, "
            f"tolerance=({x_tolerance}px x, {y_tolerance}px y)"
        )
    return seg7_frequency_cell_geometry_error(
        frame,
        rect,
        background,
        expected_text,
    )


def seg7_decode_region(
    frame: Framebuffer,
    rect: Rect,
    background: int,
    *,
    drop_dots: bool = False,
    single_cell: bool = False,
) -> str:
    """Decode 7-segment text inside rect. Unknown glyphs decode to '?'."""
    lit_cols: list[list[int]] = []
    for x in range(rect.x, rect.x + rect.w):
        column = [y for y in range(rect.y, rect.y + rect.h) if frame.pixel(x, y) != background]
        lit_cols.append(column)
    sums = [len(col) for col in lit_cols]

    runs: list[tuple[int, int]] = []
    start: int | None = None
    for i in range(rect.w + 1):
        value = sums[i] if i < rect.w else 0
        if value > 0 and start is None:
            start = i
        elif value == 0 and start is not None:
            runs.append((start, i - 1))
            start = None
    # Ink touching the rect edge belongs to a declared-overlap neighbor
    # (e.g. the Ka glyph's last columns inside the frequency rect).
    runs = [(a, b) for a, b in runs if a > 2 and b < rect.w - 1]

    def run_height(a: int, b: int) -> int:
        ys = [y for x in range(a, b + 1) for y in lit_cols[x]]
        return (max(ys) - min(ys) + 1) if ys else 0

    if single_cell:
        # The top counter is one physical LED cell (plus an optional dot), so
        # a wide sparse glyph must never be mistaken for contacted text.
        split_runs = list(runs)
    else:
        # Valley-splitting applies to full-height glyphs only: the Segment7
        # face kerns adjacent frequency digits into contact, but dots are
        # short AND wide relative to their own height and must never be split.
        tallest = max((run_height(a, b) for a, b in runs), default=0)
        split_runs: list[tuple[int, int]] = []

        def split(a: int, b: int) -> None:
            height = run_height(a, b)
            margin = max(3, int(height * 0.15))
            split_ratio = (
                SEG7_SMALL_GLYPH_SPLIT_RATIO
                if height <= SEG7_SMALL_GLYPH_MAX_HEIGHT
                else SEG7_SPLIT_RATIO
            )
            if (
                height == 0
                or height <= 0.45 * tallest
                or (b - a + 1) <= split_ratio * height
                or (b - a + 1) <= 2 * margin + 1
            ):
                split_runs.append((a, b))
                return
            window = range(a + margin, b - margin + 1)
            valley = min(window, key=lambda x: sums[x])
            split(a, valley - 1)
            split(valley, b)

        for a, b in runs:
            split(a, b)

    heights = [run_height(a, b) for a, b in split_runs]
    max_height = max(heights, default=0)

    decoded = ""
    for (a, b), height in zip(split_runs, heights):
        ys = [y for x in range(a, b + 1) for y in lit_cols[x]]
        y0, y1 = min(ys), max(ys)
        xs = [x for x in range(a, b + 1) if lit_cols[x]]
        x0, x1 = min(xs), max(xs)
        width = x1 - x0 + 1
        if height <= 0.3 * max_height and width <= 0.35 * max_height:
            if not drop_dots:
                decoded += "."
            continue
        if (
            not single_cell
            and max_height > 0
            and height < SEG7_MIN_FREQUENCY_GLYPH_HEIGHT_RATIO * max_height
        ):
            # Numeric frequency glyphs share one font/cell height. Reject a
            # shortened non-dot run before bbox normalization can inflate a
            # surviving fragment into a complete digit (notably an erased
            # '8' reduced to one vertical segment).
            decoded += "?"
            continue
        if width < 0.25 * height:
            # A real Segment7 "1" is not an arbitrary narrow blob: it has
            # distinct upper-right and lower-right strokes separated at the
            # cell midpoint. Requiring two substantial vertical components
            # prevents an erased half-stroke from being re-normalized to its
            # shortened ink bbox and accepted as a complete "1".
            row_lit = [
                any(y in lit_cols[x] for x in range(x0, x1 + 1))
                for y in range(y0, y1 + 1)
            ]
            components: list[tuple[int, int]] = []
            component_start: int | None = None
            for row, lit in enumerate(row_lit + [False]):
                if lit and component_start is None:
                    component_start = row
                elif not lit and component_start is not None:
                    components.append((component_start, row - 1))
                    component_start = None
            min_stroke_height = max(2, int(round(height * 0.25)))
            substantial = [
                (start, end)
                for start, end in components
                if end - start + 1 >= min_stroke_height
            ]
            midpoint = (height - 1) / 2
            has_upper_right = any(start < midpoint for start, _ in substantial)
            has_lower_right = any(end > midpoint for _, end in substantial)
            has_midpoint_separation = any(
                upper_end < midpoint < lower_start
                for _, upper_end in substantial
                for lower_start, _ in substantial
            )
            # In the qualified single counter cell, B/C also have fixed ink
            # bounds. Without this host-owned geometry check, the mirrored
            # F/E pair has the same topology and centered scale changes can be
            # hidden by normalizing to the surviving ink.
            geometry_matches = not single_cell or seg7_counter_geometry_matches(
                "1",
                rect=rect,
                observed=(x0, y0 - rect.y, x1, y1 - rect.y),
            )
            if (
                has_upper_right
                and has_lower_right
                and has_midpoint_separation
                and geometry_matches
            ):
                decoded += "1"
            else:
                decoded += "?"
            continue

        def sample_segments(sample_y0: int, sample_height: int) -> tuple[float, ...]:
            if single_cell:
                sample_x0 = int(
                    round(
                        rect.w * SEG7_COUNTER_CELL_LEFT / SEG7_COUNTER_RECT_WIDTH
                    )
                )
                sample_width = max(
                    1,
                    int(
                        round(
                            rect.w
                            * SEG7_COUNTER_CELL_WIDTH
                            / SEG7_COUNTER_RECT_WIDTH
                        )
                    ),
                )
            else:
                sample_x0 = x0
                sample_width = width

            def zone(r0: float, r1: float, c0: float, c1: float) -> float:
                rr0, rr1 = sample_y0 + int(r0), sample_y0 + int(r1)
                cc0, cc1 = sample_x0 + int(c0), sample_x0 + int(c1)
                total = lit = 0
                for x in range(
                    max(0, cc0),
                    min(rect.w, cc1),
                ):
                    col = lit_cols[x]
                    for y in range(
                        max(rect.y, rr0),
                        min(rect.y + rect.h, rr1),
                    ):
                        total += 1
                        if y in col:
                            lit += 1
                return (lit / total) if total else 0.0

            h, w = sample_height, sample_width
            t = max(3, int(round(h * 0.12)))
            return (
                zone(0, t, w * 0.25, w * 0.75),
                zone(h * 0.12, h * 0.44, w - t, w),
                zone(h * 0.56, h * 0.88, w - t, w),
                zone(h - t, h, w * 0.25, w * 0.75),
                zone(h * 0.56, h * 0.88, 0, t),
                zone(h * 0.12, h * 0.44, 0, t),
                zone(h * 0.5 - t / 2, h * 0.5 + t / 2, w * 0.25, w * 0.75),
            )

        if single_cell:
            # Use the fixed, host-owned V1SevenX cell envelope. Scaling the
            # constants with the manifest rect keeps synthetic contract tests
            # readable while preserving the production 55x68 geometry.
            sample_y0 = rect.y + int(
                round(rect.h * SEG7_COUNTER_CELL_TOP / SEG7_COUNTER_RECT_HEIGHT)
            )
            sample_height = max(
                1,
                int(
                    round(
                        rect.h
                        * SEG7_COUNTER_CELL_HEIGHT
                        / SEG7_COUNTER_RECT_HEIGHT
                    )
                ),
            )
        else:
            sample_y0 = y0
            sample_height = height

        segments = sample_segments(sample_y0, sample_height)
        key = tuple(1 if value > 0.5 else 0 for value in segments)
        char = SEG7_PATTERNS.get(key, "?")
        if single_cell and char in SEG7_PROTOCOL_CHARS:
            geometry_matches = seg7_counter_geometry_matches(
                char,
                rect=rect,
                observed=(x0, y0 - rect.y, x1, y1 - rect.y),
            )
            decoded += char if geometry_matches else "?"
        else:
            decoded += char
    return decoded


def assert_seg7_text(
    frame: Framebuffer,
    summary: VerificationSummary,
    *,
    element: str,
    rect: Rect,
    expected: str,
    background: int,
    drop_dots: bool = False,
    single_cell: bool = False,
    qualified_frequency: bool = False,
) -> None:
    add_region_stats(summary, rect)
    summary.l2_decodes += 1
    if qualified_frequency:
        geometry_error = seg7_frequency_geometry_error(
            frame,
            rect,
            background,
            expected,
        )
        if geometry_error is not None:
            summary.add_failure(element, f"L2 geometry: {geometry_error}")
    decoded = seg7_decode_region(
        frame,
        rect,
        background,
        drop_dots=drop_dots,
        single_cell=single_cell,
    )
    if decoded != expected:
        summary.add_failure(
            element,
            f"L2 decode: expected text {expected!r}, decoded {decoded!r} from rendered segments",
        )


def assert_glyph_shape(
    frame: Framebuffer,
    summary: VerificationSummary,
    *,
    element: str,
    rect: Rect,
    glyph_color: int,
    min_span: float = BAND_GLYPH_MIN_SPAN,
) -> None:
    """Fail when a 1-bit glyph does not span its declared tight cell.

    Coverage floors alone cannot see truncation: a band letter keeps most of
    its expected-color pixels long after its top or bottom has been chopped
    off (run 8a599c91's chopped K retained 37% cell coverage). The glyph
    bounding box, by contrast, must reach all four sides of a tight cell;
    a span below min_span on either axis means the letter is truncated,
    clipped, or partially erased.
    """
    add_region_stats(summary, rect)
    min_x = min_y = None
    max_x = max_y = None
    for y in range(rect.y, rect.y + rect.h):
        row_start = y * frame.width + rect.x
        for offset, color in enumerate(frame.pixels[row_start : row_start + rect.w]):
            if color != glyph_color:
                continue
            x = rect.x + offset
            if min_x is None or x < min_x:
                min_x = x
            if max_x is None or x > max_x:
                max_x = x
            if min_y is None:
                min_y = y
            max_y = y
    if min_y is None:
        summary.add_failure(
            element,
            f"glyph shape: no pixels of expected color 0x{glyph_color:04X} in cell",
        )
        return
    span_h = (max_y - min_y + 1) / rect.h
    span_w = (max_x - min_x + 1) / rect.w
    if span_h < min_span:
        summary.add_failure(
            element,
            f"glyph bbox spans {span_h:.1%} of cell height (>= {min_span:.0%} required); "
            f"rows {min_y}..{max_y} of cell {rect.y}..{rect.y + rect.h - 1} — "
            "truncated, clipped, or partially erased glyph",
        )
    if span_w < min_span:
        summary.add_failure(
            element,
            f"glyph bbox spans {span_w:.1%} of cell width (>= {min_span:.0%} required); "
            f"cols {min_x}..{max_x} of cell {rect.x}..{rect.x + rect.w - 1} — "
            "truncated, clipped, or partially erased glyph",
        )


def band_mask_for_cell(cell: dict[str, Any]) -> int:
    if "bandMask" in cell:
        return require_int(cell.get("bandMask"), "bandCells.bandMask")
    band = normalize_key(cell.get("band") or cell.get("label"))
    if band not in BAND_MASKS:
        raise ProtocolError(f"unknown band cell label {cell.get('label')!r}")
    return BAND_MASKS[band]


def direction_mask_for_arrow(arrow: dict[str, Any]) -> int:
    if "directionMask" in arrow:
        return require_int(arrow.get("directionMask"), "directionArrows.directionMask")
    direction = normalize_key(arrow.get("direction") or arrow.get("id"))
    if direction not in DIRECTION_MASKS:
        raise ProtocolError(f"unknown direction arrow {arrow.get('direction')!r}")
    return DIRECTION_MASKS[direction]


def role_for_element(
    element: dict[str, Any],
    field_name: str,
    fallback: str,
    path: str,
) -> str:
    role = element.get(field_name, fallback)
    return require_str(role, f"{path}.{field_name}")


def role_from_source(
    element: dict[str, Any],
    values: dict[str, Any],
    *,
    static_field: str,
    fallback: str,
    path: str,
) -> str:
    if "roleSource" in element:
        source = require_str(element.get("roleSource"), f"{path}.roleSource")
        if source not in values:
            raise ProtocolError(f"{path}.roleSource {source!r} is missing from resolved expectations")
        return require_str(values[source], f"resolved.{source}")
    return role_for_element(element, static_field, fallback, path)


def card_alerts(resolved: dict[str, Any]) -> list[dict[str, Any]]:
    alerts: list[dict[str, Any]] = []
    for name in ("secondary", "third"):
        alert = require_dict(resolved.get(name), f"resolved.{name}")
        if require_bool(alert.get("present"), f"resolved.{name}.present"):
            alerts.append(alert)
    return alerts


def status_value(status: dict[str, Any], source: str) -> Any:
    if source not in status:
        raise ProtocolError(f"status badge source {source!r} missing from step status")
    return status[source]


def is_status_active(badge: dict[str, Any], status: dict[str, Any]) -> bool:
    if "roleSource" in badge:
        role = role_from_source(
            badge,
            status,
            static_field="activeRole",
            fallback="text",
            path="statusBadges",
        )
        inactive_role = role_for_element(badge, "inactiveRole", "background", "statusBadges")
        return role != inactive_role
    source = require_str(badge.get("source"), "statusBadges.source")
    value = status_value(status, source)
    if "activeValues" in badge:
        active_values = require_list(badge.get("activeValues"), "statusBadges.activeValues")
        return value in active_values
    if badge.get("activeWhen") == "nonzero":
        return value not in (0, "", False, None)
    if badge.get("activeWhen") == "present":
        return value not in ("", None)
    return bool(value)


def drawable_rects(elements: dict[str, Any]) -> list[Rect]:
    rects: list[Rect] = []
    for key, value in elements.items():
        if key == "frequency":
            rects.append(parse_rect(require_dict(value, "layout.elements.frequency").get("rect"), "layout.elements.frequency.rect"))
            continue
        for index, item in enumerate(require_list(value, f"layout.elements.{key}")):
            element = require_dict(item, f"layout.elements.{key}[{index}]")
            rects.append(parse_rect(element.get("rect"), f"layout.elements.{key}[{index}].rect"))
            for supplemental_rect_name in ("emptyRect", "coverageRect"):
                if supplemental_rect_name in element:
                    rects.append(
                        parse_rect(
                            element.get(supplemental_rect_name),
                            f"layout.elements.{key}[{index}].{supplemental_rect_name}",
                        )
                    )
    return rects


def verify_step(
    frame: Framebuffer,
    layout: dict[str, Any],
    step: dict[str, Any],
    *,
    summary: VerificationSummary | None = None,
    count_step: bool = True,
) -> VerificationSummary:
    local_summary = summary or VerificationSummary()
    if count_step:
        local_summary.steps_checked += 1

    palette = flatten_palette(layout)
    background = palette["background"]
    elements = require_elements(layout)
    resolved = require_dict(step.get("resolved"), "step.resolved")
    status = require_dict(resolved.get("status"), "step.resolved.status")
    active_band_mask = require_int(resolved.get("activeBandMask"), "step.resolved.activeBandMask")
    active_direction_mask = require_int(resolved.get("activeDirectionMask"), "step.resolved.activeDirectionMask")
    main_meter_count = require_int(resolved.get("mainMeterCount"), "step.resolved.mainMeterCount")
    muted = require_bool(resolved.get("muted"), "step.resolved.muted")
    cards = card_alerts(resolved)

    for index, raw_cell in enumerate(require_list(elements.get("bandCells"), "layout.elements.bandCells")):
        cell = require_dict(raw_cell, f"layout.elements.bandCells[{index}]")
        rect = parse_rect(cell.get("rect"), f"layout.elements.bandCells[{index}].rect")
        active = (active_band_mask & band_mask_for_cell(cell)) != 0
        element_name = f"{step.get('id', step.get('index'))}.band_cells.{normalize_key(cell.get('label') or cell.get('band'))}"
        if active:
            role = (
                "muted"
                if muted
                else role_for_element(
                    cell,
                    "activeRole",
                    f"bands.{normalize_key(cell.get('band') or cell.get('label'))}",
                    "bandCells",
                )
            )
            color = role_color(palette, role, element_name)
            # Host-owned floor: the manifest may strengthen but never weaken.
            assert_color_coverage(
                frame,
                local_summary,
                element=element_name,
                rect=rect,
                expected_color=color,
                background_color=background,
                min_coverage=max(float(cell.get("minCoverage", FILLED_MIN_COVERAGE)), BAND_MIN_COVERAGE),
                allowed_colors={color, background},
            )
            assert_glyph_shape(
                frame,
                local_summary,
                element=element_name,
                rect=rect,
                glyph_color=color,
            )
        else:
            role = role_for_element(cell, "inactiveRole", "gray", "bandCells")
            color = role_color(palette, role, element_name)
            assert_color_coverage(
                frame,
                local_summary,
                element=element_name,
                rect=rect,
                expected_color=color,
                background_color=background,
                min_coverage=max(
                    float(cell.get("inactiveMinCoverage", INACTIVE_MIN_COVERAGE)),
                    BAND_MIN_COVERAGE,
                ),
                allowed_colors={color, background},
            )
            assert_glyph_shape(
                frame,
                local_summary,
                element=element_name,
                rect=rect,
                glyph_color=color,
            )

    for index, raw_arrow in enumerate(require_list(elements.get("directionArrows"), "layout.elements.directionArrows")):
        arrow = require_dict(raw_arrow, f"layout.elements.directionArrows[{index}]")
        rect = parse_rect(arrow.get("rect"), f"layout.elements.directionArrows[{index}].rect")
        direction = normalize_key(arrow.get("direction") or arrow.get("id"))
        active = (active_direction_mask & direction_mask_for_arrow(arrow)) != 0
        element_name = f"{step.get('id', step.get('index'))}.direction_arrows.{direction}"
        role_name = "activeRole" if active else "inactiveRole"
        fallback = f"arrows.{direction}" if active else "gray"
        role = "muted" if active and muted else role_for_element(arrow, role_name, fallback, "directionArrows")
        color = role_color(palette, role, element_name)
        assert_color_coverage(
            frame,
            local_summary,
            element=element_name,
            rect=rect,
            expected_color=color,
            background_color=background,
            min_coverage=FILLED_MIN_COVERAGE if active else INACTIVE_MIN_COVERAGE,
            allowed_colors={color, background},
        )

    for index, raw_bar in enumerate(require_list(elements.get("mainSignalBars"), "layout.elements.mainSignalBars")):
        bar = require_dict(raw_bar, f"layout.elements.mainSignalBars[{index}]")
        bar_index = require_int(bar.get("index"), f"layout.elements.mainSignalBars[{index}].index")
        rect = parse_rect(bar.get("rect"), f"layout.elements.mainSignalBars[{index}].rect")
        active = bar_index < main_meter_count
        element_name = f"{step.get('id', step.get('index'))}.primary.signal_bars[{bar_index}]"
        if active:
            role = "muted" if muted else role_for_element(bar, "activeRole", f"mainMeterRamp.{bar_index}", "mainSignalBars")
            color = role_color(palette, role, element_name)
            assert_color_coverage(
                frame,
                local_summary,
                element=element_name,
                rect=rect,
                expected_color=color,
                background_color=background,
                min_coverage=FILLED_MIN_COVERAGE,
                allowed_colors={color, background},
            )
        else:
            color = role_color(palette, role_for_element(bar, "inactiveRole", "gray", "mainSignalBars"), element_name)
            assert_color_coverage(
                frame,
                local_summary,
                element=element_name,
                rect=rect,
                expected_color=color,
                background_color=background,
                min_coverage=INACTIVE_MIN_COVERAGE,
                allowed_colors={color, background},
            )

    frequency = require_dict(elements.get("frequency"), "layout.elements.frequency")
    freq_rect = parse_rect(frequency.get("rect"), "layout.elements.frequency.rect")
    frequency_role = role_from_source(
        frequency,
        resolved,
        static_field="textRole",
        fallback="text",
        path="frequency",
    )
    freq_color = role_color(palette, frequency_role, "frequency")
    assert_text_coverage(
        frame,
        local_summary,
        element=f"{step.get('id', step.get('index'))}.frequency",
        rect=freq_rect,
        text_color=freq_color,
        background_color=background,
        min_coverage=float(frequency.get("minCoverage", TEXT_MIN_COVERAGE)),
        max_coverage=float(frequency.get("maxCoverage", TEXT_MAX_COVERAGE)),
    )
    # L2: read the literal frequency digits back from the framebuffer.
    # Numeric strings only; alpha texts (LASER, ALP gun abbreviations) keep
    # the coverage checks above and are outside seg7 decoding.
    freq_expected = require_str(
        require_dict(resolved.get("primary"), "step.resolved.primary").get("frequencyText"),
        "step.resolved.primary.frequencyText",
    )
    if seg7_is_numeric_text(freq_expected):
        assert_seg7_text(
            frame,
            local_summary,
            element=f"{step.get('id', step.get('index'))}.frequency.l2",
            rect=freq_rect,
            expected=freq_expected,
            background=background,
            qualified_frequency=True,
        )

    for index, raw_slot in enumerate(require_list(elements.get("cardSlots"), "layout.elements.cardSlots")):
        slot = require_dict(raw_slot, f"layout.elements.cardSlots[{index}]")
        slot_index = require_int(slot.get("slot"), f"layout.elements.cardSlots[{index}].slot")
        rect = parse_rect(slot.get("rect"), f"layout.elements.cardSlots[{index}].rect")
        empty_rect = parse_rect(
            slot.get("emptyRect", slot.get("rect")),
            f"layout.elements.cardSlots[{index}].emptyRect",
        )
        element_name = f"{step.get('id', step.get('index'))}.cards.slot{slot_index}"
        if slot_index < len(cards):
            text_role = "muted" if muted else role_for_element(slot, "textRole", "text", "cardSlots")
            text_color = role_color(palette, text_role, element_name)
            assert_text_coverage(
                frame,
                local_summary,
                element=element_name,
                rect=rect,
                text_color=text_color,
                background_color=background,
                min_coverage=float(slot.get("minCoverage", TEXT_MIN_COVERAGE)),
                max_coverage=float(slot.get("maxCoverage", TEXT_MAX_COVERAGE)),
            )
        else:
            assert_color_coverage(
                frame,
                local_summary,
                element=element_name,
                rect=empty_rect,
                expected_color=background,
                background_color=background,
                min_coverage=INACTIVE_MIN_COVERAGE,
                allowed_colors={background},
            )

    for index, raw_meter in enumerate(require_list(elements.get("cardMeterBars"), "layout.elements.cardMeterBars")):
        meter = require_dict(raw_meter, f"layout.elements.cardMeterBars[{index}]")
        slot_index = require_int(meter.get("slot"), f"layout.elements.cardMeterBars[{index}].slot")
        bar_index = require_int(meter.get("index"), f"layout.elements.cardMeterBars[{index}].index")
        rect = parse_rect(meter.get("rect"), f"layout.elements.cardMeterBars[{index}].rect")
        element_name = f"{step.get('id', step.get('index'))}.cards.slot{slot_index}.bars[{bar_index}]"
        card_present = slot_index < len(cards)
        bar_active = card_present and bar_index < require_int(cards[slot_index].get("cardBarCount"), "card.cardBarCount")
        if bar_active:
            role = (
                "muted"
                if muted
                else role_for_element(meter, "activeRole", f"cardMeterRamp.{bar_index}", "cardMeterBars")
            )
            color = role_color(palette, role, element_name)
            threshold = FILLED_MIN_COVERAGE
            allowed_colors = {color, background}
        elif card_present:
            role = role_for_element(meter, "inactiveRole", "gray", "cardMeterBars")
            color = role_color(palette, role, element_name)
            threshold = float(meter.get("inactiveMinCoverage", INACTIVE_MIN_COVERAGE))
            allowed_colors = {color, palette.get("gray", background), background}
        else:
            color = background
            threshold = INACTIVE_MIN_COVERAGE
            allowed_colors = {background}
        assert_color_coverage(
            frame,
            local_summary,
            element=element_name,
            rect=rect,
            expected_color=color,
            background_color=background,
            min_coverage=threshold,
            allowed_colors=allowed_colors,
        )

    for index, raw_status in enumerate(require_list(elements.get("statusText"), "layout.elements.statusText")):
        item = require_dict(raw_status, f"layout.elements.statusText[{index}]")
        source = require_str(item.get("source"), f"layout.elements.statusText[{index}].source")
        value = status_value(status, source)
        rect = parse_rect(item.get("rect"), f"layout.elements.statusText[{index}].rect")
        element_name = f"{step.get('id', step.get('index'))}.status.{source}"
        if value in ("", None) or (source == "bogeyChar" and value == " "):
            assert_color_coverage(
                frame,
                local_summary,
                element=element_name,
                rect=rect,
                expected_color=background,
                background_color=background,
                min_coverage=INACTIVE_MIN_COVERAGE,
                allowed_colors={background},
            )
        else:
            role = role_from_source(
                item,
                status,
                static_field="textRole",
                fallback="text",
                path="statusText",
            )
            color = role_color(palette, role, element_name)
            assert_text_coverage(
                frame,
                local_summary,
                element=element_name,
                rect=rect,
                text_color=color,
                background_color=background,
                min_coverage=float(item.get("minCoverage", TEXT_MIN_COVERAGE)),
                max_coverage=float(item.get("maxCoverage", TEXT_MAX_COVERAGE)),
            )
            # L2: the bogey counter is the quantity clue the driver reads —
            # decode the literal symbol. Dot glyphs are dropped because the
            # resolved manifest does not expose whether the counter dot is
            # expected; render-path/flush coherence can see drift, but the dot
            # is not yet a semantic assertion.
            if source == "bogeyChar":
                bogey = str(value)
                require_protocol_bogey_char(bogey, f"step.resolved.status.{source}")
                assert_seg7_text(
                    frame,
                    local_summary,
                    element=f"{element_name}.l2",
                    rect=rect,
                    expected=bogey,
                    background=background,
                    drop_dots=True,
                    single_cell=True,
                )

    for index, raw_badge in enumerate(require_list(elements.get("statusBadges"), "layout.elements.statusBadges")):
        badge = require_dict(raw_badge, f"layout.elements.statusBadges[{index}]")
        badge_id = require_str(badge.get("id"), f"layout.elements.statusBadges[{index}].id")
        rect = parse_rect(badge.get("rect"), f"layout.elements.statusBadges[{index}].rect")
        active = is_status_active(badge, status)
        element_name = f"{step.get('id', step.get('index'))}.status.{badge_id}"
        if active:
            role = role_from_source(
                badge,
                status,
                static_field="activeRole",
                fallback="text",
                path="statusBadges",
            )
            color = role_color(palette, role, element_name)
            threshold = float(badge.get("minCoverage", FILLED_MIN_COVERAGE))
        else:
            role = role_for_element(badge, "inactiveRole", "background", "statusBadges")
            color = role_color(palette, role, element_name)
            threshold = float(badge.get("inactiveMinCoverage", INACTIVE_MIN_COVERAGE))
        assert_color_coverage(
            frame,
            local_summary,
            element=element_name,
            rect=rect,
            expected_color=color,
            background_color=background,
            min_coverage=threshold,
            allowed_colors={color, background},
        )

    assert_cleanliness(frame, layout, local_summary, background)
    if local_summary.assertion_count == 0 or local_summary.checked_regions == 0 or local_summary.checked_pixels == 0:
        local_summary.add_failure(str(step.get("id", step.get("index"))), "suspiciously empty verifier run")
    return local_summary


def assert_cleanliness(frame: Framebuffer, layout: dict[str, Any], summary: VerificationSummary, background: int) -> None:
    elements = require_elements(layout)
    covered: set[int] = set()
    for rect in drawable_rects(elements):
        covered.update(rect_mask(rect, frame.width))
    masks = require_list(layout.get("masks", []), "layout.masks")
    for raw_mask in masks:
        rect = parse_rect(require_dict(raw_mask, "layout.masks[]").get("rect"), "layout.masks[].rect")
        mask_indexes = rect_mask(rect, frame.width)
        summary.mask_pixels += len(mask_indexes)
        covered.update(mask_indexes)

    summary.assertion_count += 1
    checked = 0
    stray: list[tuple[int, int, int]] = []
    for index, color in enumerate(frame.pixels):
        if index in covered:
            continue
        checked += 1
        if color != background:
            if len(stray) >= 5:
                continue
            x = index % frame.width
            y = index // frame.width
            stray.append((x, y, color))
    summary.checked_regions += 1
    summary.checked_pixels += checked
    if stray:
        sample = ", ".join(f"({x},{y})=0x{color:04X}" for x, y, color in stray)
        summary.add_failure("cleanliness", f"non-background pixels outside declared regions: {sample}")


def step_index(step: dict[str, Any]) -> int:
    return require_int(step.get("index"), "step.index")


def step_id(step: dict[str, Any]) -> str:
    return require_str(step.get("id"), "step.id")


def step_resolved(step: dict[str, Any]) -> dict[str, Any]:
    return require_dict(step.get("resolved"), "step.resolved")


def step_status(step: dict[str, Any]) -> dict[str, Any]:
    return require_dict(step_resolved(step).get("status"), "step.resolved.status")


def step_alert_count(step: dict[str, Any]) -> int:
    return require_int(step_resolved(step).get("alertCount"), "step.resolved.alertCount")


def step_main_meter_count(step: dict[str, Any]) -> int:
    return require_int(step_resolved(step).get("mainMeterCount"), "step.resolved.mainMeterCount")


def step_frequency_text(step: dict[str, Any]) -> str:
    primary = require_dict(step_resolved(step).get("primary"), "step.resolved.primary")
    return require_str(primary.get("frequencyText"), "step.resolved.primary.frequencyText")


def step_has_flash(step: dict[str, Any]) -> bool:
    resolved = step_resolved(step)
    return (
        require_int(resolved.get("flashMask"), "step.resolved.flashMask") != 0
        or require_int(resolved.get("bandFlashMask"), "step.resolved.bandFlashMask") != 0
    )


def step_has_arrow_and_band_flash(step: dict[str, Any]) -> bool:
    resolved = step_resolved(step)
    return (
        require_int(resolved.get("flashMask"), "step.resolved.flashMask") != 0
        and require_int(resolved.get("bandFlashMask"), "step.resolved.bandFlashMask") != 0
    )


def badge_active_for_step(layout: dict[str, Any], step: dict[str, Any], badge_id: str) -> bool | None:
    status = step_status(step)
    badges = require_list(require_elements(layout).get("statusBadges"), "layout.elements.statusBadges")
    for raw_badge in badges:
        badge = require_dict(raw_badge, "layout.elements.statusBadges[]")
        if require_str(badge.get("id"), "layout.elements.statusBadges[].id") == badge_id:
            return is_status_active(badge, status)
    return None


def discover_transition_cases(steps_manifest: dict[str, Any], layout: dict[str, Any]) -> list[TransitionCase]:
    steps = [require_dict(raw_step, "steps.steps[]") for raw_step in require_list(steps_manifest.get("steps"), "steps.steps")]
    steps.sort(key=step_index)
    cases: list[TransitionCase] = []

    def add_first(category: str, predicate) -> None:
        for source in steps:
            for target in steps:
                if step_index(source) == step_index(target):
                    continue
                if predicate(source, target):
                    cases.append(
                        TransitionCase(
                            category=category,
                            source_index=step_index(source),
                            target_index=step_index(target),
                            source_id=step_id(source),
                            target_id=step_id(target),
                        )
                    )
                    return

    add_first("multi_alert_to_single", lambda source, target: step_alert_count(source) > 1 and step_alert_count(target) <= 1)
    add_first("single_alert_to_multi", lambda source, target: step_alert_count(source) <= 1 and step_alert_count(target) > 1)
    add_first("high_main_bars_to_low", lambda source, target: step_main_meter_count(source) > step_main_meter_count(target))
    add_first("alert_to_idle", lambda source, target: step_alert_count(source) > 0 and step_alert_count(target) == 0)

    for badge_id in ("alp", "obd", "ble"):
        add_first(
            f"{badge_id}_badge_on_to_off",
            lambda source, target, badge_id=badge_id: badge_active_for_step(layout, source, badge_id) is True
            and badge_active_for_step(layout, target, badge_id) is False,
        )

    add_first(
        "frequency_wide_to_narrow",
        lambda source, target: len(step_frequency_text(source).strip()) > len(step_frequency_text(target).strip()),
    )
    add_first(
        "frequency_alpha_to_numeric",
        lambda source, target: any(ch.isalpha() for ch in step_frequency_text(source))
        and any(ch.isdigit() for ch in step_frequency_text(target)),
    )
    add_first(
        "flashing_to_steady",
        lambda source, target: step_has_arrow_and_band_flash(source) and not step_has_flash(target),
    )

    deduped: list[TransitionCase] = []
    seen_categories: set[str] = set()
    for case in cases:
        if case.category in seen_categories:
            continue
        seen_categories.add(case.category)
        deduped.append(case)
    return deduped


def source_color_region(
    element: str,
    rect: Rect,
    *,
    source_color: int,
    target_color: int,
    target_min_coverage: float,
    source_min_coverage: float = FILLED_MIN_COVERAGE,
) -> CleanupRegion:
    return CleanupRegion(
        element=element,
        rect=rect,
        source_kind="color",
        source_color=source_color,
        source_min_coverage=source_min_coverage,
        source_max_coverage=None,
        target_color=target_color,
        target_min_coverage=target_min_coverage,
        allowed_target_colors={target_color},
    )


def source_text_region(
    element: str,
    rect: Rect,
    *,
    source_color: int,
    source_min_coverage: float,
    source_max_coverage: float,
    target_color: int,
    target_min_coverage: float,
) -> CleanupRegion:
    return CleanupRegion(
        element=element,
        rect=rect,
        source_kind="text",
        source_color=source_color,
        source_min_coverage=source_min_coverage,
        source_max_coverage=source_max_coverage,
        target_color=target_color,
        target_min_coverage=target_min_coverage,
        allowed_target_colors={target_color},
    )


def transition_cleanup_regions(layout: dict[str, Any], source_step: dict[str, Any], target_step: dict[str, Any]) -> list[CleanupRegion]:
    palette = flatten_palette(layout)
    background = palette["background"]
    elements = require_elements(layout)
    source = step_resolved(source_step)
    target = step_resolved(target_step)
    source_status = require_dict(source.get("status"), "source.resolved.status")
    target_status = require_dict(target.get("status"), "target.resolved.status")
    source_muted = require_bool(source.get("muted"), "source.resolved.muted")
    regions: list[CleanupRegion] = []

    source_band_mask = require_int(source.get("activeBandMask"), "source.resolved.activeBandMask")
    target_band_mask = require_int(target.get("activeBandMask"), "target.resolved.activeBandMask")
    for index, raw_cell in enumerate(require_list(elements.get("bandCells"), "layout.elements.bandCells")):
        cell = require_dict(raw_cell, f"layout.elements.bandCells[{index}]")
        mask = band_mask_for_cell(cell)
        if (source_band_mask & mask) == 0 or (target_band_mask & mask) != 0:
            continue
        rect = parse_rect(cell.get("rect"), f"layout.elements.bandCells[{index}].rect")
        band = normalize_key(cell.get("band") or cell.get("label"))
        source_role = "muted" if source_muted else role_for_element(cell, "activeRole", f"bands.{band}", "bandCells")
        target_role = role_for_element(cell, "inactiveRole", "gray", "bandCells")
        target_color = role_color(palette, target_role, f"transition.band_cells.{band}")
        region = source_color_region(
            f"transition.band_cells.{band}",
            rect,
            source_color=role_color(palette, source_role, f"transition.band_cells.{band}"),
            target_color=target_color,
            target_min_coverage=float(cell.get("inactiveMinCoverage", INACTIVE_MIN_COVERAGE)),
            source_min_coverage=float(cell.get("minCoverage", FILLED_MIN_COVERAGE)),
        )
        region.allowed_target_colors = {target_color, background}
        regions.append(region)

    source_direction_mask = require_int(source.get("activeDirectionMask"), "source.resolved.activeDirectionMask")
    target_direction_mask = require_int(target.get("activeDirectionMask"), "target.resolved.activeDirectionMask")
    for index, raw_arrow in enumerate(require_list(elements.get("directionArrows"), "layout.elements.directionArrows")):
        arrow = require_dict(raw_arrow, f"layout.elements.directionArrows[{index}]")
        mask = direction_mask_for_arrow(arrow)
        if (source_direction_mask & mask) == 0 or (target_direction_mask & mask) != 0:
            continue
        rect = parse_rect(arrow.get("rect"), f"layout.elements.directionArrows[{index}].rect")
        direction = normalize_key(arrow.get("direction") or arrow.get("id"))
        source_role = (
            "muted"
            if source_muted
            else role_for_element(arrow, "activeRole", f"arrows.{direction}", "directionArrows")
        )
        target_role = role_for_element(arrow, "inactiveRole", "gray", "directionArrows")
        target_color = role_color(palette, target_role, f"transition.direction_arrows.{direction}")
        region = source_color_region(
            f"transition.direction_arrows.{direction}",
            rect,
            source_color=role_color(palette, source_role, f"transition.direction_arrows.{direction}"),
            target_color=target_color,
            target_min_coverage=INACTIVE_MIN_COVERAGE,
        )
        region.allowed_target_colors = {target_color, background}
        regions.append(region)

    source_meter_count = require_int(source.get("mainMeterCount"), "source.resolved.mainMeterCount")
    target_meter_count = require_int(target.get("mainMeterCount"), "target.resolved.mainMeterCount")
    for index, raw_bar in enumerate(require_list(elements.get("mainSignalBars"), "layout.elements.mainSignalBars")):
        bar = require_dict(raw_bar, f"layout.elements.mainSignalBars[{index}]")
        bar_index = require_int(bar.get("index"), f"layout.elements.mainSignalBars[{index}].index")
        if bar_index >= source_meter_count or bar_index < target_meter_count:
            continue
        rect = parse_rect(bar.get("rect"), f"layout.elements.mainSignalBars[{index}].rect")
        source_role = "muted" if source_muted else role_for_element(bar, "activeRole", f"mainMeterRamp.{bar_index}", "mainSignalBars")
        target_role = role_for_element(bar, "inactiveRole", "gray", "mainSignalBars")
        target_color = role_color(palette, target_role, f"transition.primary.signal_bars[{bar_index}]")
        region = source_color_region(
            f"transition.primary.signal_bars[{bar_index}]",
            rect,
            source_color=role_color(palette, source_role, f"transition.primary.signal_bars[{bar_index}]"),
            target_color=target_color,
            target_min_coverage=INACTIVE_MIN_COVERAGE,
        )
        region.allowed_target_colors = {target_color, background}
        regions.append(region)

    source_cards = card_alerts(source)
    target_cards = card_alerts(target)
    for index, raw_slot in enumerate(require_list(elements.get("cardSlots"), "layout.elements.cardSlots")):
        slot = require_dict(raw_slot, f"layout.elements.cardSlots[{index}]")
        slot_index = require_int(slot.get("slot"), f"layout.elements.cardSlots[{index}].slot")
        if slot_index >= len(source_cards) or slot_index < len(target_cards):
            continue
        rect = parse_rect(slot.get("rect"), f"layout.elements.cardSlots[{index}].rect")
        source_role = "muted" if source_muted else role_for_element(slot, "textRole", "text", "cardSlots")
        regions.append(
            source_text_region(
                f"transition.cards.slot{slot_index}",
                rect,
                source_color=role_color(palette, source_role, f"transition.cards.slot{slot_index}"),
                source_min_coverage=float(slot.get("minCoverage", TEXT_MIN_COVERAGE)),
                source_max_coverage=float(slot.get("maxCoverage", TEXT_MAX_COVERAGE)),
                target_color=background,
                target_min_coverage=INACTIVE_MIN_COVERAGE,
            )
        )

    for index, raw_meter in enumerate(require_list(elements.get("cardMeterBars"), "layout.elements.cardMeterBars")):
        meter = require_dict(raw_meter, f"layout.elements.cardMeterBars[{index}]")
        slot_index = require_int(meter.get("slot"), f"layout.elements.cardMeterBars[{index}].slot")
        bar_index = require_int(meter.get("index"), f"layout.elements.cardMeterBars[{index}].index")
        source_active = slot_index < len(source_cards) and bar_index < require_int(source_cards[slot_index].get("cardBarCount"), "source.card.cardBarCount")
        target_active = slot_index < len(target_cards) and bar_index < require_int(target_cards[slot_index].get("cardBarCount"), "target.card.cardBarCount")
        if not source_active or target_active:
            continue
        rect = parse_rect(meter.get("rect"), f"layout.elements.cardMeterBars[{index}].rect")
        source_role = (
            "muted"
            if source_muted
            else role_for_element(meter, "activeRole", f"cardMeterRamp.{bar_index}", "cardMeterBars")
        )
        target_card_present = slot_index < len(target_cards)
        if target_card_present:
            target_role = role_for_element(meter, "inactiveRole", "gray", "cardMeterBars")
            target_color = role_color(palette, target_role, f"transition.cards.slot{slot_index}.bars[{bar_index}]")
            target_min_coverage = float(meter.get("inactiveMinCoverage", INACTIVE_MIN_COVERAGE))
            allowed_target_colors = {target_color, palette.get("gray", background), background}
        else:
            target_color = background
            target_min_coverage = INACTIVE_MIN_COVERAGE
            allowed_target_colors = {background}
        region = source_color_region(
            f"transition.cards.slot{slot_index}.bars[{bar_index}]",
            rect,
            source_color=role_color(palette, source_role, f"transition.cards.slot{slot_index}.bars[{bar_index}]"),
            target_color=target_color,
            target_min_coverage=target_min_coverage,
        )
        region.allowed_target_colors = allowed_target_colors
        regions.append(region)

    for index, raw_status in enumerate(require_list(elements.get("statusText"), "layout.elements.statusText")):
        item = require_dict(raw_status, f"layout.elements.statusText[{index}]")
        source_name = require_str(item.get("source"), f"layout.elements.statusText[{index}].source")
        source_value = status_value(source_status, source_name)
        target_value = status_value(target_status, source_name)
        if source_value in ("", None) or target_value not in ("", None):
            continue
        rect = parse_rect(item.get("rect"), f"layout.elements.statusText[{index}].rect")
        source_role = role_from_source(
            item,
            source_status,
            static_field="textRole",
            fallback="text",
            path="statusText",
        )
        regions.append(
            source_text_region(
                f"transition.status.{source_name}",
                rect,
                source_color=role_color(palette, source_role, f"transition.status.{source_name}"),
                source_min_coverage=float(item.get("minCoverage", TEXT_MIN_COVERAGE)),
                source_max_coverage=float(item.get("maxCoverage", TEXT_MAX_COVERAGE)),
                target_color=background,
                target_min_coverage=INACTIVE_MIN_COVERAGE,
            )
        )

    for index, raw_badge in enumerate(require_list(elements.get("statusBadges"), "layout.elements.statusBadges")):
        badge = require_dict(raw_badge, f"layout.elements.statusBadges[{index}]")
        badge_id = require_str(badge.get("id"), f"layout.elements.statusBadges[{index}].id")
        if not is_status_active(badge, source_status) or is_status_active(badge, target_status):
            continue
        rect = parse_rect(badge.get("rect"), f"layout.elements.statusBadges[{index}].rect")
        source_role = role_from_source(
            badge,
            source_status,
            static_field="activeRole",
            fallback="text",
            path="statusBadges",
        )
        target_role = role_for_element(badge, "inactiveRole", "background", "statusBadges")
        target_color = role_color(palette, target_role, f"transition.status.{badge_id}")
        region = source_color_region(
            f"transition.status.{badge_id}",
            rect,
            source_color=role_color(palette, source_role, f"transition.status.{badge_id}"),
            target_color=target_color,
            target_min_coverage=float(badge.get("inactiveMinCoverage", INACTIVE_MIN_COVERAGE)),
            source_min_coverage=float(badge.get("minCoverage", FILLED_MIN_COVERAGE)),
        )
        region.allowed_target_colors = {target_color, background}
        regions.append(region)

    return regions


def deterministic_reference_regions(layout: dict[str, Any], category: str) -> list[tuple[str, Rect]]:
    """Blanket dirty-vs-clean regions: every declared element, pixel-exact.

    Every transition's dirty target must be pixel-identical to a clean render
    of the same step inside these regions. This is the check that catches a
    neighboring element's clear rect partially erasing an *unchanged* element
    on the dirty path — the class that chopped band letters in run 8a599c91
    (six of nine transitions) while every coverage assertion passed.

    Originally scoped to flat single-color draws only; frequency, status
    text, and badges were excluded because their rasters varied between pins
    of the same step. That variance was diagnosed as OpenFontRender's stale
    first-char bearing adjustment and removed (scripts/
    patch_openfontrender.py), after which run fa221725 measured zero
    dirty-vs-clean violations across every element group — so the blanket now
    covers the full declared set: band cells, arrows, main/card meter bars,
    status text, status badges (incl. coverage rects), frequency, and card
    slots (incl. empty rects).
    """
    elements = require_elements(layout)
    regions: list[tuple[str, Rect]] = []

    def add(name: str, raw_rect: Any, path: str) -> None:
        regions.append((f"transition.{category}.clean_reference.{name}", parse_rect(raw_rect, path)))

    for index, raw_cell in enumerate(require_list(elements.get("bandCells"), "layout.elements.bandCells")):
        cell = require_dict(raw_cell, f"layout.elements.bandCells[{index}]")
        band = normalize_key(cell.get("band") or cell.get("label"))
        add(f"band_cells.{band}", cell.get("rect"), f"layout.elements.bandCells[{index}].rect")
    for index, raw_arrow in enumerate(require_list(elements.get("directionArrows"), "layout.elements.directionArrows")):
        arrow = require_dict(raw_arrow, f"layout.elements.directionArrows[{index}]")
        direction = normalize_key(arrow.get("direction") or arrow.get("id"))
        add(f"direction_arrows.{direction}", arrow.get("rect"), f"layout.elements.directionArrows[{index}].rect")
    for index, raw_bar in enumerate(require_list(elements.get("mainSignalBars"), "layout.elements.mainSignalBars")):
        bar = require_dict(raw_bar, f"layout.elements.mainSignalBars[{index}]")
        bar_index = require_int(bar.get("index"), f"layout.elements.mainSignalBars[{index}].index")
        add(f"primary.signal_bars[{bar_index}]", bar.get("rect"), f"layout.elements.mainSignalBars[{index}].rect")
    for index, raw_meter in enumerate(require_list(elements.get("cardMeterBars"), "layout.elements.cardMeterBars")):
        meter = require_dict(raw_meter, f"layout.elements.cardMeterBars[{index}]")
        slot_index = require_int(meter.get("slot"), f"layout.elements.cardMeterBars[{index}].slot")
        bar_index = require_int(meter.get("index"), f"layout.elements.cardMeterBars[{index}].index")
        add(
            f"cards.slot{slot_index}.bars[{bar_index}]",
            meter.get("rect"),
            f"layout.elements.cardMeterBars[{index}].rect",
        )
    for index, raw_text in enumerate(require_list(elements.get("statusText"), "layout.elements.statusText")):
        item = require_dict(raw_text, f"layout.elements.statusText[{index}]")
        source = require_str(item.get("source"), f"layout.elements.statusText[{index}].source")
        add(f"status.{source}", item.get("rect"), f"layout.elements.statusText[{index}].rect")
    for index, raw_badge in enumerate(require_list(elements.get("statusBadges"), "layout.elements.statusBadges")):
        badge = require_dict(raw_badge, f"layout.elements.statusBadges[{index}]")
        badge_id = require_str(badge.get("id"), f"layout.elements.statusBadges[{index}].id")
        add(f"status.{badge_id}", badge.get("rect"), f"layout.elements.statusBadges[{index}].rect")
        if "coverageRect" in badge:
            add(
                f"status.{badge_id}.coverage",
                badge.get("coverageRect"),
                f"layout.elements.statusBadges[{index}].coverageRect",
            )
    frequency = require_dict(elements.get("frequency"), "layout.elements.frequency")
    add("frequency", frequency.get("rect"), "layout.elements.frequency.rect")
    for index, raw_slot in enumerate(require_list(elements.get("cardSlots"), "layout.elements.cardSlots")):
        slot = require_dict(raw_slot, f"layout.elements.cardSlots[{index}]")
        slot_index = require_int(slot.get("slot"), f"layout.elements.cardSlots[{index}].slot")
        add(f"cards.slot{slot_index}", slot.get("rect"), f"layout.elements.cardSlots[{index}].rect")
        if "emptyRect" in slot:
            add(
                f"cards.slot{slot_index}.empty",
                slot.get("emptyRect"),
                f"layout.elements.cardSlots[{index}].emptyRect",
            )
    return regions


def clean_reference_regions(
    layout: dict[str, Any],
    source_step: dict[str, Any],
    category: str,
) -> list[tuple[str, Rect]]:
    elements = require_elements(layout)
    regions = deterministic_reference_regions(layout, category)
    if category in {"frequency_wide_to_narrow", "frequency_alpha_to_numeric"}:
        frequency = require_dict(elements.get("frequency"), "layout.elements.frequency")
        regions.append(
            (f"transition.{category}.frequency", parse_rect(frequency.get("rect"), "layout.elements.frequency.rect"))
        )
        return regions
    if category != "flashing_to_steady":
        return regions

    resolved = step_resolved(source_step)
    flash_mask = require_int(resolved.get("flashMask"), "source.resolved.flashMask")
    band_flash_mask = require_int(resolved.get("bandFlashMask"), "source.resolved.bandFlashMask")
    # Flash-specific regions append to the blanket deterministic set. Their
    # rects duplicate blanket band/arrow rects; the duplicate assertion is
    # intentional — the flashing names carry the authored-flash semantics the
    # discovery test pins, while the blanket names carry erased-neighbor
    # semantics for every category.
    arrow_flash_bits = {1: 0x20, 2: 0x40, 4: 0x80}
    for index, raw_arrow in enumerate(require_list(elements.get("directionArrows"), "layout.elements.directionArrows")):
        arrow = require_dict(raw_arrow, f"layout.elements.directionArrows[{index}]")
        direction_mask = direction_mask_for_arrow(arrow)
        if (flash_mask & arrow_flash_bits[direction_mask]) == 0:
            continue
        direction = normalize_key(arrow.get("direction") or arrow.get("id"))
        regions.append(
            (
                f"transition.flashing_to_steady.direction_arrows.{direction}",
                parse_rect(arrow.get("rect"), f"layout.elements.directionArrows[{index}].rect"),
            )
        )
    for index, raw_cell in enumerate(require_list(elements.get("bandCells"), "layout.elements.bandCells")):
        cell = require_dict(raw_cell, f"layout.elements.bandCells[{index}]")
        if (band_flash_mask & band_mask_for_cell(cell)) == 0:
            continue
        band = normalize_key(cell.get("band") or cell.get("label"))
        regions.append(
            (
                f"transition.flashing_to_steady.band_cells.{band}",
                parse_rect(cell.get("rect"), f"layout.elements.bandCells[{index}].rect"),
            )
        )
    return regions


def assert_matches_clean_reference(
    source_frame: Framebuffer,
    dirty_target_frame: Framebuffer,
    clean_target_frame: Framebuffer,
    summary: VerificationSummary,
    *,
    element: str,
    rect: Rect,
    require_source_change: bool = True,
) -> None:
    add_region_stats(summary, rect)
    source_pixels = source_frame.rect_pixels(rect)
    dirty_pixels = dirty_target_frame.rect_pixels(rect)
    clean_pixels = clean_target_frame.rect_pixels(rect)
    source_changes = sum(source != clean for source, clean in zip(source_pixels, clean_pixels))
    if require_source_change and source_changes == 0:
        summary.add_error(f"{element}: source and clean target are identical in the targeted stale region")

    mismatches = [index for index, (dirty, clean) in enumerate(zip(dirty_pixels, clean_pixels)) if dirty != clean]
    if not mismatches:
        return
    samples: list[str] = []
    for index in mismatches[:4]:
        x = rect.x + (index % rect.w)
        y = rect.y + (index // rect.w)
        samples.append(f"({x},{y}) dirty=0x{dirty_pixels[index]:04X} clean=0x{clean_pixels[index]:04X}")
    summary.add_failure(
        element,
        f"dirty target differs from clean target at {len(mismatches)}/{len(clean_pixels)} pixels; "
        + ", ".join(samples),
    )


def assert_cleanup_region(
    source_frame: Framebuffer,
    target_frame: Framebuffer,
    summary: VerificationSummary,
    region: CleanupRegion,
    *,
    background: int,
) -> None:
    add_region_stats(summary, region.rect)
    source_total, source_expected, _, _ = analyze_rect(
        source_frame,
        region.rect,
        region.source_color,
        background,
    )
    if region.source_kind == "text":
        source_coverage = source_expected / source_total
        max_coverage = region.source_max_coverage if region.source_max_coverage is not None else 1.0
        if source_coverage < region.source_min_coverage or source_coverage > max_coverage:
            summary.add_error(
                f"{region.element}: transition source did not exercise previous text pixels: "
                f"coverage {source_coverage:.1%}, expected "
                f"{region.source_min_coverage:.1%}..{max_coverage:.1%}"
            )
    else:
        source_coverage = source_expected / source_total
        if source_coverage < region.source_min_coverage:
            summary.add_error(
                f"{region.element}: transition source did not exercise previous active pixels: "
                f"coverage {source_coverage:.1%}, expected >= {region.source_min_coverage:.0%}",
            )

    add_region_stats(summary, region.rect)
    target_total, target_expected, _, target_counts = analyze_rect(
        target_frame,
        region.rect,
        region.target_color,
        background,
    )
    target_coverage = target_expected / target_total
    if target_coverage < region.target_min_coverage:
        summary.add_failure(
            region.element,
            "transition cleanup did not clear previous pixels: "
            f"target color 0x{region.target_color:04X} coverage {target_coverage:.1%}, "
            f"expected >= {region.target_min_coverage:.0%}",
        )
    bad_colors = set(target_counts) - region.allowed_target_colors
    if bad_colors:
        rendered = ", ".join(f"0x{color:04X}" for color in sorted(bad_colors)[:4])
        summary.add_failure(region.element, f"transition cleanup left unexpected color(s): {rendered}")


def verify_transition(
    source_frame: Framebuffer,
    target_frame: Framebuffer,
    layout: dict[str, Any],
    source_step: dict[str, Any],
    target_step: dict[str, Any],
    *,
    category: str,
    clean_target_frame: Framebuffer | None = None,
    summary: VerificationSummary | None = None,
) -> VerificationSummary:
    local_summary = summary or VerificationSummary()
    local_summary.transitions_checked += 1
    failures_before = len(local_summary.failures)
    errors_before = len(local_summary.errors)

    verify_step(target_frame, layout, target_step, summary=local_summary, count_step=False)
    palette = flatten_palette(layout)
    background = palette["background"]
    cleanup_regions = transition_cleanup_regions(layout, source_step, target_step)
    reference_regions = clean_reference_regions(layout, source_step, category)
    # Every transition requires a clean reference render: the blanket
    # deterministic regions (band cells, arrows, meter bars) are compared
    # dirty-vs-clean on all categories, so a missing reference silently
    # removes the check that catches erased-neighbor defects. Fail closed.
    if clean_target_frame is None:
        local_summary.add_error(f"transition {category} is missing its clean target reference capture")
    if category in CLEAN_REFERENCE_TRANSITION_CATEGORIES and not any(
        ".clean_reference." not in element for element, _rect in reference_regions
    ):
        local_summary.add_error(f"transition {category} produced zero category-specific reference regions")
    if not cleanup_regions and not reference_regions:
        local_summary.add_error(f"transition {category} produced zero cleanup regions")
    for region in cleanup_regions:
        assert_cleanup_region(source_frame, target_frame, local_summary, region, background=background)
    if clean_target_frame is not None:
        frequency_categories = {"frequency_wide_to_narrow", "frequency_alpha_to_numeric"}
        for element, rect in reference_regions:
            # Source-change proof applies only to the category-specific
            # frequency region: blanket regions cover elements that may be
            # legitimately identical between source and clean target.
            require_change = category in frequency_categories and ".clean_reference." not in element
            assert_matches_clean_reference(
                source_frame,
                target_frame,
                clean_target_frame,
                local_summary,
                element=element,
                rect=rect,
                require_source_change=require_change,
            )

    new_failures = local_summary.failures[failures_before:]
    new_errors = local_summary.errors[errors_before:]
    record = TransitionRecord(
        category=category,
        source_index=step_index(source_step),
        target_index=step_index(target_step),
        source_id=step_id(source_step),
        target_id=step_id(target_step),
        stale_regions_checked=len(cleanup_regions) + len(reference_regions),
        result="ERROR" if new_errors else ("FAIL" if new_failures else "PASS"),
        failures=[f"{failure.element}: {failure.message}" for failure in new_failures]
        + [f"ERROR: {error}" for error in new_errors],
    )
    local_summary.transition_records.append(record)
    return local_summary


def encode_logical_to_raw(frame: Framebuffer) -> bytes:
    if frame.width != LOGICAL_WIDTH or frame.height != LOGICAL_HEIGHT:
        raise ProtocolError("synthetic encoder requires 640x172 logical framebuffer")
    raw = [0] * (RAW_WIDTH * RAW_HEIGHT)
    for ly in range(LOGICAL_HEIGHT):
        for lx in range(LOGICAL_WIDTH):
            px = RAW_WIDTH - 1 - ly
            py = lx
            raw[py * RAW_WIDTH + px] = frame.pixel(lx, ly)
    out = bytearray()
    for color in raw:
        out.append(color & 0xFF)
        out.append((color >> 8) & 0xFF)
    return bytes(out)


def framebuffer_headers(
    *,
    render_seq: int = 1,
    pinned_step: int = 0,
    binding: tuple[int, str, str, str] | None = None,
) -> dict[str, str]:
    headers = {
        "X-FB-Raw-Width": str(RAW_WIDTH),
        "X-FB-Raw-Height": str(RAW_HEIGHT),
        "X-FB-Logical-Width": str(LOGICAL_WIDTH),
        "X-FB-Logical-Height": str(LOGICAL_HEIGHT),
        "X-FB-Format": FRAMEBUFFER_FORMAT,
        "X-FB-Transform": FRAMEBUFFER_TRANSFORM,
        "X-Display-Render-Seq": str(render_seq),
        "X-Display-Pinned-Step": str(pinned_step),
    }
    if binding is not None:
        headers.update(
            {
                "X-Display-Manifest-Schema-Version": str(binding[0]),
                "X-Display-Firmware-Version": binding[1],
                "X-Display-Firmware-Sha": binding[2],
                "X-Display-Settings-Fingerprint": binding[3],
            }
        )
    return headers


def normalize_base_url(device: str) -> str:
    if device.startswith("http://") or device.startswith("https://"):
        return device.rstrip("/")
    return f"http://{device.rstrip('/')}"


def fetch_json(base_url: str, path: str) -> dict[str, Any]:
    request = urllib.request.Request(f"{base_url}{path}", method="GET")
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            body = response.read()
    except urllib.error.URLError as exc:
        raise ProtocolError(f"failed to fetch {path}: {exc}") from exc
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError(f"{path} returned invalid JSON") from exc
    return require_dict(payload, path)


def post_json(base_url: str, path: str, payload: dict[str, Any]) -> dict[str, Any]:
    body = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        f"{base_url}{path}",
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "X-V1Simple-Request": "maintenance-ui",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            response_body = response.read()
    except urllib.error.URLError as exc:
        raise ProtocolError(f"failed to post {path}: {exc}") from exc
    try:
        decoded = json.loads(response_body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError(f"{path} returned invalid JSON") from exc
    return require_dict(decoded, path)


def clear_visual_pin(base_url: str) -> dict[str, Any]:
    response = post_json(base_url, "/api/display/visual/clear", {})
    if response.get("success") is not True:
        raise ProtocolError("visual clear did not return success")
    if response.get("active") is not False:
        raise ProtocolError("visual clear did not confirm active=false")
    if response.get("restored") is not True:
        raise ProtocolError("visual clear did not confirm maintenance-screen restoration")
    return response


def fetch_framebuffer(base_url: str) -> tuple[bytes, dict[str, str]]:
    request = urllib.request.Request(f"{base_url}/api/display/visual/framebuffer", method="GET")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            body = response.read()
            headers = {key: value for key, value in response.headers.items()}
    except urllib.error.URLError as exc:
        raise ProtocolError(f"failed to fetch framebuffer: {exc}") from exc
    content_length = headers.get("Content-Length")
    if content_length is None:
        raise ProtocolError("framebuffer response missing exact Content-Length")
    if int(content_length) != len(body):
        raise ProtocolError(f"framebuffer Content-Length {content_length} != bytes read {len(body)}")
    return body, headers


def fetch_flush_shadow(base_url: str) -> tuple[bytes, dict[str, str]]:
    request = urllib.request.Request(f"{base_url}/api/display/visual/flushshadow", method="GET")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            body = response.read()
            headers = {key: value for key, value in response.headers.items()}
    except urllib.error.URLError as exc:
        raise ProtocolError(f"failed to fetch flush shadow: {exc}") from exc
    content_length = headers.get("Content-Length")
    if content_length is None:
        raise ProtocolError("flush shadow response missing exact Content-Length")
    if int(content_length) != len(body):
        raise ProtocolError(f"flush shadow Content-Length {content_length} != bytes read {len(body)}")
    return body, headers


def verify_flush_shadow(
    base_url: str,
    frame: Framebuffer,
    render_seq: int,
    binding: tuple[int, str, str, str],
    summary: VerificationSummary,
    *,
    element: str,
) -> None:
    """Assert the panel received exactly what the framebuffer holds.

    The firmware mirrors every flush (full and partial region blits) into the
    shadow while a visual pin is active. shadow == framebuffer proves the
    dirty-window accounting covered every painted pixel; a divergence names
    pixels that were painted but never flushed, or flushed into the wrong
    region — the on-device class (clipped band labels, Diag14) that a
    framebuffer capture alone cannot see. Protocol gaps (route missing,
    shadow unavailable, sequence drift) are errors, never silent skips.
    """
    body, headers = fetch_flush_shadow(base_url)
    if required_header(headers, "X-FB-Shadow") != "1":
        raise ProtocolError(f"{element}: flush shadow response missing X-FB-Shadow marker")
    shadow_seq = int_header(headers, "X-Display-Render-Seq")
    if shadow_seq != render_seq:
        raise ProtocolError(
            f"{element}: render sequence changed between capture and flush-shadow fetch: "
            f"{render_seq} -> {shadow_seq}"
        )
    shadow = decode_framebuffer(body, headers, binding=binding)
    summary.assertion_count += 1
    summary.checked_regions += 1
    summary.checked_pixels += shadow.width * shadow.height
    summary.flush_shadow_compares += 1
    summary.flush_shadow_checked = True
    if shadow.pixels == frame.pixels:
        return
    total = len(frame.pixels)
    samples: list[str] = []
    mismatches = 0
    for index, (fb_color, shadow_color) in enumerate(zip(frame.pixels, shadow.pixels)):
        if fb_color == shadow_color:
            continue
        mismatches += 1
        if len(samples) < 4:
            x = index % frame.width
            y = index // frame.width
            samples.append(f"({x},{y}) fb=0x{fb_color:04X} shadow=0x{shadow_color:04X}")
    summary.add_failure(
        element,
        f"flush shadow diverges from framebuffer at {mismatches}/{total} pixels; "
        + ", ".join(samples)
        + " — painted pixels were never flushed to the panel (dirty-window "
        "under-coverage) or were flushed into the wrong region",
    )


def pin_and_capture(
    base_url: str,
    index: int,
    *,
    clear: bool,
    binding: tuple[int, str, str, str],
) -> tuple[Framebuffer, int]:
    pin = post_json(base_url, "/api/display/visual/pin", {"index": index, "clear": clear})
    if pin.get("success") is not True:
        raise ProtocolError(f"pin step {index} did not return success")
    render_seq = require_int(pin.get("renderSeq"), "pin.renderSeq")
    body, headers = fetch_framebuffer(base_url)
    frame_seq = int_header(headers, "X-Display-Render-Seq")
    pinned_step = int_header(headers, "X-Display-Pinned-Step")
    if frame_seq != render_seq:
        raise ProtocolError(f"render sequence changed between pin and capture: {render_seq} -> {frame_seq}")
    if pinned_step != index:
        raise ProtocolError(f"captured pinned step {pinned_step}, expected {index}")
    frame = decode_framebuffer(body, headers, binding=binding)
    return frame, render_seq


def selected_steps(steps_manifest: dict[str, Any], step_index: int | None, filter_text: str) -> list[dict[str, Any]]:
    steps = require_list(steps_manifest.get("steps"), "steps.steps")
    selected = []
    for raw_step in steps:
        step = require_dict(raw_step, "steps.steps[]")
        if step_index is not None and require_int(step.get("index"), "step.index") != step_index:
            continue
        if filter_text and filter_text not in require_str(step.get("id"), "step.id"):
            continue
        selected.append(step)
    if not selected:
        raise ProtocolError("step selection matched zero steps")
    return selected


def rgb565_to_rgb888(color: int) -> tuple[int, int, int]:
    red = ((color >> 11) & 0x1F) * 255 // 31
    green = ((color >> 5) & 0x3F) * 255 // 63
    blue = (color & 0x1F) * 255 // 31
    return red, green, blue


def safe_slug(value: str) -> str:
    cleaned = [char if char.isalnum() or char in ("-", "_", ".") else "_" for char in value]
    slug = "".join(cleaned).strip("_")
    return slug[:96] or "capture"


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def verifier_sha256() -> str:
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def atomic_write_text(path: Path, payload: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_text(payload, encoding="utf-8")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def save_frame_png(frame: Framebuffer, path: Path) -> None:
    if Image is None:
        raise ProtocolError("Pillow is required for PNG capture export")
    path.parent.mkdir(parents=True, exist_ok=True)
    image = Image.new("RGB", (frame.width, frame.height))
    image.putdata([rgb565_to_rgb888(color) for color in frame.pixels])
    temporary = path.with_name(f".{path.stem}.{uuid.uuid4().hex}.tmp.png")
    try:
        image.save(temporary, format="PNG")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def record_capture(
    summary: VerificationSummary,
    frame: Framebuffer,
    output_dir: Path | None,
    *,
    capture_id: str,
    kind: str,
    step: dict[str, Any],
    render_seq: int | None,
    transition: str = "",
) -> None:
    relative_path = ""
    capture_sha256 = ""
    if output_dir is not None:
        filename = f"{safe_slug(capture_id)}.png"
        run_path = safe_slug(summary.run_id) if summary.run_id else "unidentified-run"
        relative_path = f"captures/{run_path}/{filename}"
        capture_path = output_dir / relative_path
        save_frame_png(frame, capture_path)
        capture_sha256 = hashlib.sha256(capture_path.read_bytes()).hexdigest()
    summary.captures.append(
        CaptureRecord(
            capture_id=capture_id,
            kind=kind,
            step_index=step_index(step),
            step_id=step_id(step),
            render_seq=render_seq,
            transition=transition,
            path=relative_path,
            sha256=capture_sha256,
        )
    )


def write_summary(summary: VerificationSummary, output_dir: str) -> None:
    if not output_dir:
        return
    path = Path(output_dir)
    path.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(summary.to_json(), indent=2, sort_keys=True) + "\n"
    if summary.run_id:
        atomic_write_text(path / "runs" / safe_slug(summary.run_id) / "display_visual_summary.json", payload)
    atomic_write_text(path / "display_visual_summary.json", payload)
    atomic_write_text(path / "display_visual_report.json", payload)


def write_run_inputs(
    steps: dict[str, Any],
    layout: dict[str, Any],
    output_dir: str,
    run_id: str,
) -> None:
    if not output_dir:
        return
    path = Path(output_dir)
    targets = [path]
    if run_id:
        targets.insert(0, path / "runs" / safe_slug(run_id))
    steps_payload = json.dumps(steps, indent=2, sort_keys=True) + "\n"
    layout_payload = json.dumps(layout, indent=2, sort_keys=True) + "\n"
    for target in targets:
        atomic_write_text(target / "display_visual_steps.json", steps_payload)
        atomic_write_text(target / "display_visual_layout.json", layout_payload)


def write_html_report(summary: VerificationSummary, output_dir: str) -> None:
    if not output_dir:
        return
    path = Path(output_dir)
    path.mkdir(parents=True, exist_ok=True)

    def esc(value: Any) -> str:
        return html.escape(str(value), quote=True)

    cleanup_status = (
        "PASS"
        if summary.cleanup_succeeded
        else ("FAIL" if summary.cleanup_attempted else "NOT ATTEMPTED")
    )
    if summary.flush_shadow_checked:
        flush_shadow_note = (
            f"{summary.flush_shadow_compares} flush-shadow comparisons checked delivery "
            "to the panel interface, not the physical glass."
        )
    else:
        flush_shadow_note = "Flush-shadow delivery checking was not run."

    failure_items = "\n".join(
        f"<li><strong>{esc(failure.element)}</strong>: {esc(failure.message)}</li>" for failure in summary.failures
    ) or "<li>none</li>"
    error_items = "\n".join(f"<li>{esc(error)}</li>" for error in summary.errors) or "<li>none</li>"
    transition_rows = "\n".join(
        "<tr>"
        f"<td>{esc(record.category)}</td>"
        f"<td>{record.source_index:03d} {esc(record.source_id)}</td>"
        f"<td>{record.target_index:03d} {esc(record.target_id)}</td>"
        f"<td>{esc(record.result)}</td>"
        f"<td>{record.stale_regions_checked}</td>"
        "</tr>"
        for record in summary.transition_records
    ) or '<tr><td colspan="5">none</td></tr>'

    def capture_link(capture: CaptureRecord) -> str:
        if not capture.path:
            return ""
        return f'<a href="{esc(capture.path)}">PNG</a>'

    capture_rows = "\n".join(
        "<tr>"
        f"<td>{esc(capture.capture_id)}</td>"
        f"<td>{esc(capture.kind)}</td>"
        f"<td>{capture.step_index:03d} {esc(capture.step_id)}</td>"
        f"<td>{esc(capture.transition)}</td>"
        f"<td>{capture.render_seq if capture.render_seq is not None else ''}</td>"
        f"<td>{capture_link(capture)}</td>"
        f"<td><code>{esc(capture.sha256)}</code></td>"
        "</tr>"
        for capture in summary.captures
    ) or '<tr><td colspan="7">none</td></tr>'

    html_body = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Display Visual Verification Report</title>
  <style>
    body {{ font-family: system-ui, sans-serif; margin: 24px; color: #17202a; }}
    table {{ border-collapse: collapse; width: 100%; margin: 12px 0 24px; }}
    th, td {{ border: 1px solid #d5d8dc; padding: 6px 8px; text-align: left; vertical-align: top; }}
    th {{ background: #f4f6f7; }}
    code {{ background: #f4f6f7; padding: 1px 4px; }}
  </style>
</head>
<body>
  <h1>Display Visual Verification Report</h1>
  <p><strong>Result:</strong> {esc(summary.result)}</p>
  <p><strong>Scope:</strong> <code>{esc(summary.scope_label())}</code></p>
  <p><strong>Evidence limits:</strong> Golden-image comparison was not run. Automated physical-panel/glass verification was not run. {esc(flush_shadow_note)}</p>
  <table>
    <tr><th>Run ID</th><th>Started</th><th>Completed</th><th>Run complete</th><th>Exit code</th><th>Cleanup</th></tr>
    <tr>
      <td><code>{esc(summary.run_id)}</code></td>
      <td>{esc(summary.started_at_utc)}</td>
      <td>{esc(summary.completed_at_utc)}</td>
      <td>{'yes' if summary.verification_completed else 'no'}</td>
      <td>{'' if summary.exit_code is None else summary.exit_code}</td>
      <td>{esc(cleanup_status)}</td>
    </tr>
  </table>
  <table>
    <tr><th>Firmware version</th><th>Firmware SHA</th><th>Settings fingerprint</th><th>Verifier SHA-256</th></tr>
    <tr>
      <td>{esc(summary.binding.get('firmware_version', ''))}</td>
      <td><code>{esc(summary.binding.get('firmware_sha', ''))}</code></td>
      <td><code>{esc(summary.binding.get('settings_fingerprint', ''))}</code></td>
      <td><code>{esc(summary.verifier_sha256)}</code></td>
    </tr>
  </table>
  <table>
    <tr><th>Steps</th><th>Transitions</th><th>Assertions</th><th>Regions</th><th>Pixels</th><th>Masked pixels</th><th>L2 decodes</th><th>Flush-shadow comparisons</th></tr>
    <tr>
      <td>{summary.steps_checked} / {summary.steps_expected}</td>
      <td>{summary.transitions_checked} / {summary.transitions_expected}</td>
      <td>{summary.assertion_count}</td>
      <td>{summary.checked_regions}</td>
      <td>{summary.checked_pixels}</td>
      <td>{summary.mask_pixels}</td>
      <td>{summary.l2_decodes} / {summary.l2_decodes_expected}</td>
      <td>{summary.flush_shadow_compares}</td>
    </tr>
  </table>
  <h2>Thresholds</h2>
  <p>
    filled <code>{FILLED_MIN_COVERAGE:.0%}</code>,
    inactive <code>{INACTIVE_MIN_COVERAGE:.0%}</code>,
    text <code>{TEXT_MIN_COVERAGE:.1%}..{TEXT_MAX_COVERAGE:.1%}</code>
  </p>
  <h2>Failures</h2>
  <ul>{failure_items}</ul>
  <h2>Errors</h2>
  <ul>{error_items}</ul>
  <h2>Transitions</h2>
  <table>
    <tr><th>Category</th><th>Source</th><th>Target</th><th>Result</th><th>Cleanup regions</th></tr>
    {transition_rows}
  </table>
  <h2>Captures</h2>
  <table>
    <tr><th>ID</th><th>Kind</th><th>Step</th><th>Transition</th><th>Render seq</th><th>File</th><th>SHA-256</th></tr>
    {capture_rows}
  </table>
</body>
</html>
"""
    atomic_write_text(path / "display_visual_report.html", html_body)


def write_reports(summary: VerificationSummary, output_dir: str, *, no_report: bool) -> None:
    write_summary(summary, output_dir)
    if output_dir and not no_report:
        write_html_report(summary, output_dir)


def run_device(
    args: argparse.Namespace,
    *,
    summary: VerificationSummary | None = None,
) -> VerificationSummary:
    if args.goldens or args.update_goldens:
        raise ProtocolError("--goldens and --update-goldens are deferred Phase 4 work")

    local_summary = summary or VerificationSummary()
    base_url = normalize_base_url(args.device)
    steps = validate_steps_manifest(fetch_json(base_url, "/api/display/visual/steps"))
    validate_protocol_bogey_corpus(steps)
    layout = validate_layout_manifest(fetch_json(base_url, "/api/display/visual/layout"), strict_masks=args.strict_masks)
    binding = validate_manifest_binding(steps, layout)
    local_summary.binding = {
        "manifest_schema_version": binding[0],
        "firmware_version": binding[1],
        "firmware_sha": binding[2],
        "settings_fingerprint": binding[3],
    }
    write_run_inputs(steps, layout, args.output_dir, local_summary.run_id)

    output_dir = Path(args.output_dir) if args.output_dir else None
    selected = [] if args.transitions_only else selected_steps(steps, args.step, args.filter)
    should_run_transitions = args.transitions_only or (args.step is None and not args.filter)
    transition_cases = discover_transition_cases(steps, layout) if should_run_transitions else []
    all_steps_by_index = {
        step_index(step): step
        for step in require_list(steps.get("steps"), "steps.steps")
    }
    local_summary.steps_expected = len(selected)
    local_summary.transitions_expected = len(transition_cases)
    local_summary.l2_decodes_expected = expected_l2_decodes_for_run(
        selected,
        transition_cases,
        all_steps_by_index,
    )
    require_full_default_l2_workload(
        local_summary.l2_decodes_expected,
        full_default_run=(
            not args.transitions_only
            and args.step is None
            and not args.filter
        ),
    )
    if should_run_transitions:
        discovered_categories = {case.category for case in transition_cases}
        missing_categories = sorted(REQUIRED_TRANSITION_CATEGORIES - discovered_categories)
        if missing_categories:
            local_summary.add_error(
                "required transition categories were not discoverable: " + ", ".join(missing_categories)
            )
    pin_may_be_active = False
    check_flush_shadow = not args.no_flush_shadow

    try:
        for step in selected:
            index = step_index(step)
            failures_before = len(local_summary.failures)
            pin_may_be_active = True
            frame, render_seq = pin_and_capture(base_url, index, clear=True, binding=binding)
            step_capture_id = f"step_{index:03d}_{step_id(step)}"
            record_capture(
                local_summary,
                frame,
                output_dir,
                capture_id=step_capture_id,
                kind="step",
                step=step,
                render_seq=render_seq,
            )
            if check_flush_shadow:
                verify_flush_shadow(
                    base_url, frame, render_seq, binding, local_summary,
                    element=f"flush_shadow.{step_capture_id}",
                )
            verify_step(frame, layout, step, summary=local_summary)
            new_failures = local_summary.failures[failures_before:]
            if new_failures:
                failure = new_failures[0]
                print(f"FAIL step {index:03d} {step_id(step)} {failure.element}: {failure.message}")
            else:
                print(f"PASS step {index:03d} {step_id(step)}")

        for ordinal, case in enumerate(transition_cases):
            source_step = require_dict(
                all_steps_by_index.get(case.source_index),
                f"transition.{case.category}.source",
            )
            target_step = require_dict(
                all_steps_by_index.get(case.target_index),
                f"transition.{case.category}.target",
            )
            failures_before = len(local_summary.failures)
            errors_before = len(local_summary.errors)
            pin_may_be_active = True
            source_frame, source_seq = pin_and_capture(base_url, case.source_index, clear=True, binding=binding)
            source_capture_id = (
                f"transition_{ordinal:02d}_{case.category}_source_{case.source_index:03d}_{case.source_id}"
            )
            record_capture(
                local_summary,
                source_frame,
                output_dir,
                capture_id=source_capture_id,
                kind="transition-source",
                step=source_step,
                render_seq=source_seq,
                transition=case.category,
            )
            if check_flush_shadow:
                verify_flush_shadow(
                    base_url, source_frame, source_seq, binding, local_summary,
                    element=f"flush_shadow.{source_capture_id}",
                )
            pin_may_be_active = True
            target_frame, target_seq = pin_and_capture(base_url, case.target_index, clear=False, binding=binding)
            target_capture_id = (
                f"transition_{ordinal:02d}_{case.category}_target_{case.target_index:03d}_{case.target_id}"
            )
            record_capture(
                local_summary,
                target_frame,
                output_dir,
                capture_id=target_capture_id,
                kind="transition-target",
                step=target_step,
                render_seq=target_seq,
                transition=case.category,
            )
            if check_flush_shadow:
                # The dirty-path pin is the load-bearing shadow comparison:
                # partial region flushes only happen here, so this is where
                # dirty-window under-coverage would surface.
                verify_flush_shadow(
                    base_url, target_frame, target_seq, binding, local_summary,
                    element=f"flush_shadow.{target_capture_id}",
                )
            # Every transition captures a clean reference render of its
            # target: verify_transition fails closed without it, because the
            # blanket dirty-vs-clean regions are what catch erased-neighbor
            # defects on the dirty path (run 8a599c91: chopped band letters
            # in six of nine transitions, all previously unchecked).
            pin_may_be_active = True
            clean_target_frame, clean_target_seq = pin_and_capture(
                base_url,
                case.target_index,
                clear=True,
                binding=binding,
            )
            reference_capture_id = (
                f"transition_{ordinal:02d}_{case.category}_reference_"
                f"{case.target_index:03d}_{case.target_id}"
            )
            record_capture(
                local_summary,
                clean_target_frame,
                output_dir,
                capture_id=reference_capture_id,
                kind="transition-reference",
                step=target_step,
                render_seq=clean_target_seq,
                transition=case.category,
            )
            if check_flush_shadow:
                verify_flush_shadow(
                    base_url, clean_target_frame, clean_target_seq, binding, local_summary,
                    element=f"flush_shadow.{reference_capture_id}",
                )
            verify_transition(
                source_frame,
                target_frame,
                layout,
                source_step,
                target_step,
                category=case.category,
                clean_target_frame=clean_target_frame,
                summary=local_summary,
            )
            new_failures = local_summary.failures[failures_before:]
            new_errors = local_summary.errors[errors_before:]
            label = f"{case.category} {case.source_index:03d}->{case.target_index:03d}"
            if new_errors:
                print(f"ERROR transition {label}: {new_errors[0]}")
            elif new_failures:
                failure = new_failures[0]
                print(f"FAIL transition {label} {failure.element}: {failure.message}")
            else:
                print(f"PASS transition {label}")

        if local_summary.steps_checked != local_summary.steps_expected:
            local_summary.add_error(
                f"step coverage incomplete: checked {local_summary.steps_checked}, "
                f"expected {local_summary.steps_expected}"
            )
        if local_summary.transitions_checked != local_summary.transitions_expected:
            local_summary.add_error(
                f"transition coverage incomplete: checked {local_summary.transitions_checked}, "
                f"expected {local_summary.transitions_expected}"
            )
        if should_run_transitions and local_summary.transitions_checked == 0:
            local_summary.add_error("zero transitions checked")
        if local_summary.l2_decodes != local_summary.l2_decodes_expected:
            local_summary.add_error(
                "L2 decode coverage incomplete: checked "
                f"{local_summary.l2_decodes}, expected "
                f"{local_summary.l2_decodes_expected}"
            )
        local_summary.verification_completed = True
        local_summary.finalize_result()
        return local_summary
    finally:
        if pin_may_be_active:
            local_summary.cleanup_attempted = True
            try:
                local_summary.cleanup_response = clear_visual_pin(base_url)
            except Exception as exc:  # Cleanup errors must not erase the primary run evidence.
                local_summary.add_error(f"visual cleanup failed: {type(exc).__name__}: {exc}")
            else:
                local_summary.cleanup_succeeded = True
            local_summary.finalize_result()


def summary_exit_code(summary: VerificationSummary) -> int:
    if summary.interrupted:
        return 130
    if not summary.verification_completed:
        return 2
    if summary.errors or summary.result == "ERROR":
        return 2
    if summary.cleanup_attempted and not summary.cleanup_succeeded:
        return 2
    if summary.result == "PASS":
        return 0
    if summary.result == "FAIL":
        return 1
    return 2


def print_final_result(summary: VerificationSummary, output_dir: str) -> None:
    for error in summary.errors:
        print(f"ERROR {error}", file=sys.stderr)
    if output_dir:
        root = Path(output_dir).resolve()
        report = (
            root / "runs" / safe_slug(summary.run_id) / "display_visual_summary.json"
            if summary.run_id
            else root / "display_visual_summary.json"
        )
        if report.exists():
            prefix = "REPORT" if summary.artifacts_finalized else "REPORT_INCOMPLETE"
            print(f"{prefix} {report}")
        else:
            print(f"ERROR report artifact was not written: {report}", file=sys.stderr)
    cleanup = "PASS" if summary.cleanup_succeeded else ("FAIL" if summary.cleanup_attempted else "NOT_ATTEMPTED")
    artifacts = (
        "PASS"
        if summary.artifacts_finalized
        else ("FAIL" if summary.artifacts_requested else "NOT_REQUESTED")
    )
    label = "INTERRUPTED" if summary.interrupted else summary.result
    print(
        f"RESULT {label} exit={summary.exit_code} "
        f"complete={'yes' if summary.verification_completed else 'no'} "
        f"steps={summary.steps_checked}/{summary.steps_expected} "
        f"transitions={summary.transitions_checked}/{summary.transitions_expected} "
        f"l2={summary.l2_decodes}/{summary.l2_decodes_expected} "
        f"assertions={summary.assertion_count} failures={len(summary.failures)} "
        f"errors={len(summary.errors)} cleanup={cleanup} artifacts={artifacts} "
        f"scope={summary.scope_label()}"
    )


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    summary = VerificationSummary(
        result="RUNNING",
        run_id=uuid.uuid4().hex,
        started_at_utc=utc_now(),
        verifier_sha256=verifier_sha256(),
        artifacts_requested=bool(args.output_dir),
        options={
            "step": args.step,
            "filter": args.filter,
            "transitions_only": args.transitions_only,
            "strict_masks": args.strict_masks,
            "goldens": args.goldens,
            "update_goldens": args.update_goldens,
            "no_flush_shadow": args.no_flush_shadow,
        },
    )

    try:
        write_reports(summary, args.output_dir, no_report=args.no_report)
    except OSError as exc:
        summary.add_error(f"failed to initialize report artifacts: {exc}")
        summary.completed_at_utc = utc_now()
        summary.exit_code = 2
        try:
            write_summary(summary, args.output_dir)
        except OSError as recovery_exc:
            summary.add_error(f"failed to persist initialization error summary: {recovery_exc}")
        print_final_result(summary, args.output_dir)
        return 2

    try:
        run_device(args, summary=summary)
    except ProtocolError as exc:
        summary.add_error(str(exc))
    except KeyboardInterrupt:
        summary.interrupted = True
        summary.add_error("interrupted by user")
    except Exception as exc:
        summary.add_error(f"internal verifier error: {type(exc).__name__}: {exc}")

    summary.completed_at_utc = utc_now()
    summary.finalize_result()
    summary.exit_code = summary_exit_code(summary)
    summary.artifacts_finalized = bool(args.output_dir)
    try:
        write_reports(summary, args.output_dir, no_report=args.no_report)
    except OSError as exc:
        summary.artifacts_finalized = False
        summary.add_error(f"failed to finalize report artifacts: {exc}")
        summary.exit_code = 2
        try:
            write_summary(summary, args.output_dir)
        except OSError as recovery_exc:
            summary.add_error(f"failed to persist corrected error summary: {recovery_exc}")
    print_final_result(summary, args.output_dir)
    return summary.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
