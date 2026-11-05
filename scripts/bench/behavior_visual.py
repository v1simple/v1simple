"""Narrow optical verdicts for the hands-off behavior bench.

The decoder sees pixels and a registered pose, never requested settings or
stimuli. It supports the current seven-segment layout and six saturated test
colors, not arbitrary RGB565 colorimetry. Callers own capture integrity, clock
alignment and firmware/input identity. All times below use one caller-supplied
clock; the tolerance is an observation budget, not a product latency promise.
"""
from __future__ import annotations

import math
from collections import Counter
from functools import lru_cache

import numpy as np

if __package__:
    from .read_replay_fields import CARD_GLYPHS, read_band, read_direction, read_frequency, registered_pixels
else:
    from read_replay_fields import CARD_GLYPHS, read_band, read_direction, read_frequency, registered_pixels


COLOR_CLASSES = ("red", "yellow", "green", "cyan", "blue", "magenta")
# Calibrated image coordinates after registered_pixels, as in read_replay_fields.
# Meter interiors exclude outlines and neighboring content. Main order is the
# user's bar1..bar6 (bottom to top); card order is left to right.
MAIN_BAR_RECTS = tuple((905, y, 963, y + 12) for y in (407, 369, 331, 293, 255, 217))
CARD_BAR_RECTS = tuple(
    tuple((left + 33 * index, 433, left + 33 * index + 24, 446) for index in range(6))
    for left in (425, 673)
)
REGION_ALLOWANCE = {"frequency_vertical": 220, "main_bars": 160,
                    "card0": 180, "card1": 180, "arrows": 180}


def _image(rgb):
    if not isinstance(rgb, np.ndarray) or rgb.shape != (720, 1280, 3) or rgb.dtype != np.uint8:
        raise ValueError("expected a 1280x720 RGB uint8 camera frame")
    return rgb


def _rect(rgb, rect):
    x0, y0, x1, y1 = rect
    return rgb[y0:y1, x0:x1]


def _color(rgb, *, minimum_pixels=80, minimum_fraction=.08):
    """Classify visible saturated content, independent of expected color."""
    values = rgb.astype(np.float32)
    high, low = values.max(axis=2), values.min(axis=2)
    threshold = max(45., float(np.percentile(high, 98)) * .55)
    chroma = high - low
    # The admitted camera captures bright magenta with saturation near .30,
    # while its blue phosphor appears near hue 206 rather than textbook 240.
    # These global optical bounds apply to every region. Weak chroma between
    # the neutral and saturated bands remains unreadable, not an invented hue.
    mask = (high >= threshold) & (chroma >= np.maximum(25., high * .20))
    count = int(mask.sum())
    visible_count = int((high >= threshold).sum())
    result = {"color": None, "pixels": count, "fraction": round(count / mask.size, 4),
              "visible_pixels": visible_count, "agreement": 0.0}
    if count < minimum_pixels or count / mask.size < minimum_fraction:
        neutral = (high >= threshold) & (chroma < np.maximum(18., high * .12))
        neutral_count = int(neutral.sum())
        if (neutral_count >= minimum_pixels and neutral_count / mask.size >= minimum_fraction and
                neutral_count / max(visible_count, 1) >= .85):
            result.update(color="neutral", pixels=neutral_count,
                          fraction=round(neutral_count / mask.size, 4),
                          agreement=round(neutral_count / visible_count, 4))
        return result
    selected, spread, maximum = values[mask], chroma[mask], high[mask]
    red, green, blue = selected.T
    hue = np.empty(count, dtype=np.float32)
    is_red = red == maximum
    is_green = (~is_red) & (green == maximum)
    is_blue = ~(is_red | is_green)
    hue[is_red] = ((green[is_red] - blue[is_red]) / spread[is_red]) % 6
    hue[is_green] = (blue[is_green] - red[is_green]) / spread[is_green] + 2
    hue[is_blue] = (red[is_blue] - green[is_blue]) / spread[is_blue] + 4
    hue *= 60
    # Deliberate sentinels remain separated under this fixed camera profile.
    # Explicit hue gaps reject boundary shades. Every visible pixel, including
    # neutral/weak-chroma content, counts against agreement unless it supports
    # the selected hue. A colored minority cannot authorize a washed-out field.
    indices = np.select([((hue < 42) | (hue >= 335)), (hue >= 48) & (hue < 80),
                         (hue >= 85) & (hue < 150), (hue >= 160) & (hue < 192),
                         (hue >= 198) & (hue < 265), (hue >= 285) & (hue < 325)],
                        [0, 1, 2, 3, 4, 5], default=-1)
    votes = np.bincount(indices[indices >= 0], minlength=6)
    winner = int(votes.argmax())
    agreement = float(votes[winner] / max(visible_count, 1))
    result["agreement"] = round(agreement, 4)
    if agreement >= .75:
        result["color"] = COLOR_CLASSES[winner]
    return result


def observe_frame(rgb, registration):
    """Return JSON-compatible observations from one original camera frame.

    A successful SCAN registration must be retained for the same unmoved camera
    session. It is applied here to every field, including absence and guards.
    This function does not classify an observed frame as a test pass or failure.
    """
    pixels = registered_pixels(_image(rgb), registration)
    brightness = pixels.max(axis=2)
    # Agreeing independent thresholds tolerate optical halos without choosing
    # whichever reading matches the stimulus. Any readable conflict rejects
    # the identity; one isolated reading is insufficient.
    frequency_peak = float(np.percentile(brightness[250:370, 450:850], 98))
    thresholds = {max(25., frequency_peak * ratio) for ratio in (.5, .6, .7)}
    readings = [read_frequency(brightness >= threshold) for threshold in sorted(thresholds)]
    readable = [reading for reading in readings if reading is not None]
    frequency = readable[0] if len(readable) >= 2 and len(set(readable)) == 1 else None
    lit = brightness >= 35
    area = lit[250:370, 450:850]
    grid = area.reshape(6, 20, 20, 20).mean(axis=(1, 3)).ravel()
    activity = {
        "frequency_vertical": int(lit[255:304, 450:850].sum() + lit[324:358, 450:850].sum()),
        "main_bars": int(lit[210:425, 897:975].sum()),
        "card0": int(lit[378:455, 410:635].sum()),
        "card1": int(lit[378:455, 655:880].sum()),
        "arrows": int(lit[185:414, 995:1170].sum()),
    }

    guards = {"counter": int(lit[188:278, 240:312].sum()),
              "status": int(lit[420:453, 1030:1205].sum())}
    return {
        "schema_version": 1,
        "primary_frequency": frequency,
        "frequency_color": _color(pixels[250:370, 450:850], minimum_pixels=300),
        # A fully lit cell must fill most of its fixed interior. A minority
        # patch over dark/occluded content is not six-cell palette evidence.
        "main_bars": [_color(_rect(pixels, rect), minimum_pixels=90, minimum_fraction=.75)
                      for rect in MAIN_BAR_RECTS],
        "card_bars": [[_color(_rect(pixels, rect), minimum_pixels=45, minimum_fraction=.75)
                       for rect in slot] for slot in CARD_BAR_RECTS],
        "activity": activity,
        "guards": guards,
        "frequency_grid": [round(float(value), 4) for value in grid],
    }

@lru_cache(maxsize=1)
def _warning_geometry():
    # Bounded registered-pose fit. Unlike the padded registration crop, these
    # are text coordinates, with four scale-6 5x7 glyphs and one space.
    positions = [(x, y, scale) for scale in np.arange(9., 11.01, .25)
                 for y in range(230, 276, 2) for x in range(460, 522, 2)]
    columns, rows = np.meshgrid(np.arange(30) + .5, np.arange(8) + .5)
    xs = np.array([np.rint(x + columns * scale).astype(int) for x, y, scale in positions])
    ys = np.array([np.rint(y + rows * scale).astype(int) for x, y, scale in positions])
    glyphs = {**CARD_GLYPHS, "V": (0x1F, 0x20, 0x40, 0x20, 0x1F), " ": (0, 0, 0, 0, 0)}
    names = tuple(glyphs)
    masks = np.array([[[bool((glyphs[name][column] if column < 5 else 0) & (1 << row))
                        for column in range(6)] for row in range(8)] for name in names])
    wanted = np.concatenate([masks[names.index(letter)] for letter in "VOL 0"], axis=1)
    return positions, xs, ys, names, masks, wanted


def observe_warning_frame(rgb, registration):
    """Read full warning glyphs, never treating an arbitrary red patch as text."""
    result = observe_frame(rgb, registration)
    pixels = registered_pixels(_image(rgb), registration)
    center = pixels[230:375, 450:880]
    lit = center.max(axis=2) >= 35
    result["warning"] = {"text": None, "blank": int(lit.sum()) < 40, "lit_pixels": int(lit.sum())}
    bright = pixels.max(axis=2) >= 35
    result["warning_guard_grid"] = np.concatenate((
        bright[188:278, 240:312].reshape(9, 10, 9, 8).mean(axis=(1, 3)).ravel(),
        bright[420:453, 1030:1205].reshape(11, 3, 25, 7).mean(axis=(1, 3)).ravel())).tolist()
    result["warning_live"] = {"band": read_band(pixels),
                               "direction": read_direction(pixels.max(axis=2) >= 120)}
    values = pixels.astype(np.float32)
    red = ((values[:, :, 0] >= 45) & (values[:, :, 0] >= values[:, :, 1] * 1.3)
           & (values[:, :, 0] >= values[:, :, 2] * 1.5))
    if int(red[230:375, 450:880].sum()) < 500:
        return result
    positions, xs, ys, names, masks, wanted = _warning_geometry()
    integral = np.pad(red.astype(np.int32), ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    observed = ((integral[ys + 2, xs + 2] - integral[ys - 1, xs + 2]
                 - integral[ys + 2, xs - 1] + integral[ys - 1, xs - 1]) >= 5)
    errors = (observed != wanted).sum(axis=(1, 2))
    candidates = np.flatnonzero(errors <= 2)
    for index in candidates[np.argsort(errors[candidates])]:
        letters = []
        for position in range(5):
            cell = observed[index, :, position * 6:(position + 1) * 6]
            distance = (masks != cell).sum(axis=(1, 2))
            order = np.argsort(distance)
            if distance[order[0]] > 1 or distance[order[1]] - distance[order[0]] < 2:
                break
            letters.append(names[order[0]])
        if "".join(letters) == "VOL 0":
            x, y, scale = positions[index]
            # Require the rest of the center to stay empty as well. Glyph-cell
            # centers alone cannot exclude an extra word or colored remnant.
            allowed = np.zeros(lit.shape, dtype=bool)
            nominal = np.zeros_like(allowed)
            for row, column in zip(*np.where(wanted)):
                x0, x1 = round(x + column*scale) - 450, round(x + (column+1)*scale) - 450
                y0, y1 = round(y + row*scale) - 230, round(y + (row+1)*scale) - 230
                nominal[max(0, y0-2):min(len(nominal), y1+2), max(0, x0-2):min(nominal.shape[1], x1+2)] = True
                # Saturated optical edges spread beyond the fitted font cells.
                # Bound that fringe below half a cell; full foreground and
                # background cell identity above must still agree.
                allowed[max(0, y0-4):min(len(allowed), y1+4), max(0, x0-4):min(allowed.shape[1], x1+4)] = True
            extra = int((lit & ~allowed).sum())
            # Optical spread must not increase the unrelated-content budget.
            if extra > max(50, int(nominal.sum() * .025)):
                continue
            result["warning"].update(text="VOL 0", cell_errors=int(errors[index]),
                                     fitted_geometry=list(positions[index]), extra_lit_pixels=extra)
            break
    return result


def evaluate_warning_transition(frames, registration, idle_reference, target_s):
    """Validate one short optical fade without relabeling intermediate text.

    Complete opposite endpoints, original native-frame coverage and visible
    pixel monotonicity bound the transition; subvisibility strokes are not
    independently certified by an intermediate exposure.
    """

    def reject(reason):
        return {"result": "INCOMPLETE", "reason": reason, "target_s": target_s}

    if len(frames) < 3 or not np.isfinite(target_s):
        return reject("missing native frames or invalid target time")
    if any(not np.isfinite(row["time_s"]) for row in frames):
        return reject("invalid native frame time")
    if any(
        b["time_s"] <= a["time_s"]
        or b["time_s"] - a["time_s"] > 0.010000001
        or b["frame_seq"] != a["frame_seq"] + 1
        for a, b in zip(frames, frames[1:])
    ):
        return reject("native frame coverage is unordered or has a gap")
    target = min(range(len(frames)), key=lambda i: abs(frames[i]["time_s"] - target_s))
    if abs(frames[target]["time_s"] - target_s) > 1e-6:
        return reject("original unknown frame is missing")
    observations = [observe_warning_frame(row["rgb"], registration) for row in frames]
    states = [
        "on"
        if row["warning"]["text"] == "VOL 0"
        else "off"
        if row["warning"]["blank"]
        else "unknown"
        for row in observations
    ]
    if states[target] != "unknown":
        return reject("target is already a stable observation")
    left = next((i for i in range(target - 1, -1, -1) if states[i] != "unknown"), None)
    right = next(
        (i for i in range(target + 1, len(frames)) if states[i] != "unknown"), None
    )
    if left is None or right is None or states[left] == states[right]:
        return reject("opposite readable endpoints are missing")
    span = frames[right]["time_s"] - frames[left]["time_s"]
    if span > 0.050000001:
        return reject("transition bracket exceeds 50 ms")
    # Inspect every collected neighbor: one unresolved transition, with stable
    # full text/blank on its respective sides. Never cherry-pick clean edges.
    if any(state != states[left] for state in states[: left + 1]) or any(
        state != states[right] for state in states[right:]
    ):
        return reject("collected neighbors contain another unresolved or wrong state")
    reference, error = _reference(idle_reference)
    if error:
        return reject(error)
    guard_reference = np.median(
        np.array([row.get("warning_guard_grid", []) for row in idle_reference]), axis=0
    )
    if guard_reference.shape != (356,) or guard_reference.sum() < 1:
        return reject("original control masks are missing")
    for row in observations:
        guard = np.array(row["warning_guard_grid"])
        if (
            not _visible(row)
            or guard.shape != guard_reference.shape
            or np.abs(guard - guard_reference).sum()
            / max(float(guard_reference.sum()), 1.0)
            > 0.25
        ):
            return reject("control masks are not preserved")
        if row["primary_frequency"] is not None or any(
            row["activity"][field]
            > reference["activity"][field] + REGION_ALLOWANCE[field]
            for field in ("main_bars", "card0", "card1", "arrows")
        ):
            return reject("alert content is present")
    on_index = left if states[left] == "on" else right
    off_index = right if states[right] == "off" else left
    selected = frames[left : right + 1]
    pixels = [
        registered_pixels(row["rgb"], registration)[230:375, 450:880].astype(np.float64)
        for row in selected
    ]
    on, off = pixels[on_index - left], pixels[off_index - left]
    x, y, scale = observations[on_index]["warning"]["fitted_geometry"]
    wanted = _warning_geometry()[-1]
    nominal = np.zeros(on.shape[:2], bool)
    allowed = np.zeros_like(nominal)
    for r, c in zip(*np.where(wanted)):
        x0, x1 = round(x + c * scale) - 450, round(x + (c + 1) * scale) - 450
        y0, y1 = round(y + r * scale) - 230, round(y + (r + 1) * scale) - 230
        for fringe, mask in ((2, nominal), (4, allowed)):
            mask[
                max(0, y0 - fringe) : min(len(mask), y1 + fringe),
                max(0, x0 - fringe) : min(mask.shape[1], x1 + fringe),
            ] = True
    budget = max(50, int(nominal.sum() * 0.025))
    low, high = (
        np.minimum(on[:, :, 0], off[:, :, 0]),
        np.maximum(on[:, :, 0], off[:, :, 0]),
    )
    direction = 1 if states[left] == "off" else -1
    columns, rows = np.meshgrid(np.arange(30) + 0.5, np.arange(8) + 0.5)
    xs = np.rint(x + columns * scale).astype(int) - 450
    ys = np.rint(y + rows * scale).astype(int) - 230

    def cell_majorities(mask):
        integral = np.pad(mask.astype(np.int32), ((1, 0), (1, 0))).cumsum(0).cumsum(1)
        return (
            integral[ys + 2, xs + 2]
            - integral[ys - 1, xs + 2]
            - integral[ys + 2, xs - 1]
            + integral[ys - 1, xs - 1]
        ) >= 5

    # Full-glyph neighbors can prove that a background cell already contains
    # a visible optical edge. Only its absolute visibility crossing is then
    # ambiguous; all existing delta/content/reversal gates remain mandatory.
    witnessed_background = np.zeros_like(wanted)
    for frame, state in zip(frames, states):
        if state != "on":
            continue
        baseline = registered_pixels(frame["rgb"], registration)[
            230:375, 450:880
        ].astype(np.float64)
        baseline_red = baseline[:, :, 0]
        visible_red = (
            (baseline_red >= 35)
            & (baseline_red >= baseline[:, :, 1] * 1.3)
            & (baseline_red >= baseline[:, :, 2] * 1.5)
        )
        witnessed_background |= cell_majorities(visible_red) & ~wanted

    detail = []
    previous = None
    for source, pixel in zip(selected, pixels):
        red_channel = pixel[:, :, 0]
        lit = pixel.max(axis=2) >= 35
        red = (red_channel >= pixel[:, :, 1] * 1.3) & (
            red_channel >= pixel[:, :, 2] * 1.5
        )
        extra = int((lit & ~allowed).sum())
        wrong_hue = int((lit & ~red).sum())
        wrong_hue_cells = int(cell_majorities(lit & ~red).sum())
        outside = int(((red_channel <= low - 35) | (red_channel >= high + 35)).sum())
        reverse = (
            np.zeros_like(allowed)
            if previous is None
            else direction * (red_channel - previous) <= -35
        )
        reversed_pixels = int(reverse.sum())
        # A coherent new visible stroke or reappearance is not scattered
        # optical noise, even with a nonzero camera background. Reuse the
        # decoder's absolute35 visibility floor and 3x3 cell majority.
        new_cells = int(
            (
                cell_majorities((red_channel >= 35) & (high < 35))
                & ~wanted
                & ~witnessed_background
            ).sum()
        )
        visibility_reverse = (
            np.zeros_like(allowed)
            if previous is None
            else (
                ((red_channel < 35) & (previous >= 35))
                if direction == 1
                else ((red_channel >= 35) & (previous < 35))
            )
        )
        reverse_cells = int(
            (
                (cell_majorities(reverse | visibility_reverse) & ~witnessed_background)
                | cell_majorities(reverse)
            ).sum()
        )
        detail.append(
            {
                "time_s": source["time_s"],
                "frame_seq": source["frame_seq"],
                "outside_endpoint_pixels": outside,
                "reverse_pixels": reversed_pixels,
                "new_foreground_cells": new_cells,
                "reverse_cells": reverse_cells,
                "extra_lit_pixels": extra,
                "wrong_hue_pixels": wrong_hue,
                "wrong_hue_cells": wrong_hue_cells,
            }
        )
        if (
            wrong_hue_cells
            or new_cells
            or reverse_cells
            or max(outside, reversed_pixels, extra, wrong_hue) > budget
        ):
            return {
                **reject(
                    "visible pixels do not follow the bounded endpoint transition"
                ),
                "details": detail,
                "budget": budget,
            }
        previous = (
            red_channel
            if previous is None
            else (
                np.maximum(previous, red_channel)
                if direction == 1
                else np.minimum(previous, red_channel)
            )
        )
    return {
        "result": "PASS",
        "kind": "bounded_transition",
        "target_s": target_s,
        "from_state": states[left],
        "to_state": states[right],
        "start_s": selected[0]["time_s"],
        "end_s": selected[-1]["time_s"],
        "duration_s": span,
        "budget": budget,
        "inspected_native_frames": len(frames),
        "details": detail,
        "interpretation": "one short optical transition within the existing noise bounds; intermediate subsets are not stable text proof",
    }


def _warning_blink(rows, transitions, native_presence, active_window):
    """Use every captured frame for cadence and sampled full text for identity."""

    def reject(reason, result="INCOMPLETE", **detail):
        return {"result": result, "reason": reason, **detail}

    if not rows:
        return reject("missing active samples")
    if any(
        b["time_s"] <= a["time_s"] or b["time_s"] - a["time_s"] > 0.140000001
        for a, b in zip(rows, rows[1:])
    ):
        return reject("active sample coverage has a gap")
    admitted = {}
    for row in rows:
        state = row["warning_state"]
        if state == "idle":
            return reject("warning expired during active interval", "FAIL")
        if state in ("on", "off"):
            continue
        proof = transitions.get(row["time_s"])
        if (
            not proof
            or proof.get("result") != "PASS"
            or proof.get("kind") != "bounded_transition"
            or proof.get("target_s") != row["time_s"]
            or proof.get("from_state") not in ("on", "off")
            or proof.get("to_state") not in ("on", "off")
            or proof["from_state"] == proof["to_state"]
            or not all(_finite(proof.get(key)) for key in ("start_s", "end_s"))
            or not proof["start_s"] < row["time_s"] < proof["end_s"]
            or not 0 < proof["end_s"] - proof["start_s"] <= 0.050000001
        ):
            return reject("unreadable sample has no validated bounded transition")
        admitted[row["time_s"]] = proof

    native = list(native_presence or [])
    if not 3 <= len(native) <= 3000:
        return reject("complete native warning presence is missing")
    for row in native:
        source = row.get("source_frame", {})
        if (
            type(row.get("host_monotonic_ns")) is not int
            or type(row.get("frame_seq")) is not int
            or type(row.get("center_lit_pixels")) is not int
            or not 0 <= row["center_lit_pixels"] <= 145 * 430
            or source.get("host_capture_ns") != row["host_monotonic_ns"]
            or source.get("frame_seq") != row["frame_seq"]
            or type(source.get("video_pts_value")) is not int
            or type(source.get("video_pts_timescale")) is not int
            or source["video_pts_timescale"] <= 0
            or not _finite(row.get("video_pts_seconds"))
            or abs(
                row["video_pts_seconds"]
                - source["video_pts_value"] / source["video_pts_timescale"]
            )
            > 1e-9
        ):
            return reject("native presence source identity is invalid")
    if any(
        b["frame_seq"] != a["frame_seq"] + 1
        or not 0 < b["host_monotonic_ns"] - a["host_monotonic_ns"] <= 10_000_000
        or b["video_pts_seconds"] <= a["video_pts_seconds"]
        for a, b in zip(native, native[1:])
    ):
        return reject("native presence source coverage is missing or unordered")
    first_s, last_s = (
        native[0]["host_monotonic_ns"] / 1e9,
        native[-1]["host_monotonic_ns"] / 1e9,
    )
    if (
        not 0 <= first_s - active_window[0] <= 0.010000001
        or not 0 <= active_window[1] - last_s <= 0.010000001
    ):
        return reject("native presence does not cover both active-window endpoints")
    by_time = {row["host_monotonic_ns"]: row for row in native}
    groups = []
    edges = []
    for row in native:
        state = "visible" if row["center_lit_pixels"] >= 40 else "blank"
        now = row["host_monotonic_ns"] / 1e9
        if not groups or groups[-1]["state"] != state:
            if groups:
                edges.append(
                    {
                        "from_state": groups[-1]["state"],
                        "to_state": state,
                        "low_s": groups[-1]["end"],
                        "high_s": now,
                    }
                )
            groups.append(
                {
                    "state": state,
                    "start": now,
                    "end": now,
                    "first_frame_seq": row["frame_seq"],
                    "last_frame_seq": row["frame_seq"],
                    "frames": 1,
                    "full_glyph_witnesses": [],
                }
            )
        else:
            groups[-1]["end"] = now
            groups[-1]["last_frame_seq"] = row["frame_seq"]
            groups[-1]["frames"] += 1
    # Presence is not text. Every stable sampled observation must agree with
    # its exact native frame, and each visible phase needs a full-glyph witness.
    for row in rows:
        source = by_time.get(round(row["time_s"] * 1e9))
        if source is None:
            return reject("sampled content is missing its exact native frame")
        visible = source["center_lit_pixels"] >= 40
        if row["warning_state"] == "on":
            if not visible:
                return reject("full glyph conflicts with native blank presence")
            group = next(
                item
                for item in groups
                if item["first_frame_seq"]
                <= source["frame_seq"]
                <= item["last_frame_seq"]
            )
            group["full_glyph_witnesses"].append(row["time_s"])
        elif row["warning_state"] == "off" and visible:
            return reject("sampled blank conflicts with native visible presence")
    for proof in admitted.values():
        low, high = proof["start_s"], proof["end_s"]
        start = by_time.get(round(low * 1e9))
        end = by_time.get(round(high * 1e9))
        if start is None or end is None:
            return reject("bounded transition endpoints lack exact native coverage")
        direction = (
            ("visible", "blank")
            if proof["from_state"] == "on"
            else ("blank", "visible")
        )
        actual = (
            "visible" if start["center_lit_pixels"] >= 40 else "blank",
            "visible" if end["center_lit_pixels"] >= 40 else "blank",
        )
        matching = [
            edge
            for edge in edges
            if round(low * 1e9) <= round(edge["low_s"] * 1e9)
            and round(edge["high_s"] * 1e9) <= round(high * 1e9)
        ]
        if (
            actual != direction
            or len(matching) != 1
            or (matching[0]["from_state"], matching[0]["to_state"]) != direction
        ):
            return reject("bounded transition conflicts with native phase edges")
    if not 30 <= len(groups) <= 44 or {item["state"] for item in groups} != {
        "visible",
        "blank",
    }:
        return reject(
            "warning did not repeatedly alternate visible and blank phases",
            "FAIL",
            groups=groups,
        )
    if any(group["end"] - group["start"] > 0.300000001 for group in groups):
        return reject(
            "a native warning phase exceeded 300 ms", "FAIL", groups=groups, edges=edges
        )
    durations = []
    for entry, exit_edge in zip(edges, edges[1:]):
        low = exit_edge["low_s"] - entry["high_s"]
        high = exit_edge["high_s"] - entry["low_s"]
        durations.append({"low_s": low, "high_s": high})
        # The visible threshold can cross at different points in LCD rise and
        # fall. Bound each phase by the declared 50 ms optical transition, then
        # measure cadence using like-direction edges so that asymmetry cancels.
        if high < 0.20 or low > 0.30:
            return reject(
                "native phase excludes the 250 ms plus/minus 50 ms optical bound",
                "FAIL",
                groups=groups,
                edges=edges,
                durations=durations,
            )
    means = []
    for direction in ("visible", "blank"):
        same = [edge for edge in edges if edge["to_state"] == direction]
        intervals = 2 * (len(same) - 1)
        low = (same[-1]["low_s"] - same[0]["high_s"]) / intervals
        high = (same[-1]["high_s"] - same[0]["low_s"]) / intervals
        means.append({"to_state": direction, "low_s": low, "high_s": high})
        if not low - 0.01 <= 0.25 <= high + 0.01:
            return reject(
                "native aggregate cadence interval excludes 250 ms",
                "FAIL",
                groups=groups,
                edges=edges,
                mean_intervals=means,
            )
    if any(
        group["state"] == "visible" and not group["full_glyph_witnesses"]
        for group in groups
    ):
        return reject(
            "a native visible phase lacks a complete VOL 0 witness",
            groups=groups,
            edges=edges,
        )
    return {
        "result": "PASS",
        "groups": groups,
        "edges": edges,
        "mean_interval_s": [
            min(item["low_s"] for item in means),
            max(item["high_s"] for item in means),
        ],
        "mean_intervals": means,
        "native_frame_count": len(native),
        "transitions": list(admitted.values()),
    }

def evaluate_warning(samples, idle_reference, *, onset_s, tolerance_s=.35,
                     clear_idle_s=None, live_control=None, transitions=None, native_presence=None):
    """Require warning onset, repeated visible on/off phases, then original idle."""
    if not _finite(onset_s) or not .1 <= tolerance_s <= .5:
        raise ValueError("Invalid warning observation timing")
    rows = _timed(samples)
    reference, error = _reference(idle_reference)
    names = ("warning_idle_controls", "warning_onset", "warning_blink", "warning_expiry")
    if not rows:
        return [{"check": name, **_report(incomplete=["warning camera observations are missing"])} for name in names]
    if error:
        return [{"check": name, **_report(incomplete=[error])} for name in names]
    failures = {name: [] for name in names}
    incomplete = {name: [] for name in names}
    start = onset_s - .6 if clear_idle_s is None else max(clear_idle_s + .05, onset_s - .6)
    selected = _window(rows, (start, onset_s + 12.), .14,
                       "warning timeline", incomplete[names[0]])
    live = _timed(live_control or [])
    if clear_idle_s is None or not _finite(clear_idle_s) or clear_idle_s > onset_s + .25:
        incomplete[names[0]].append("warning lacks its accepted idle-clear anchor")
    else:
        live = _window(live, (clear_idle_s - 1.1, clear_idle_s - .2), .14,
                       "live alert before warning", incomplete[names[0]])
    for row in live:
        if (not _visible(row) or row.get("primary_frequency") != "24.150"
                or row.get("warning_live") != {"band": "k", "direction": "FRONT"}
                or any(cell.get("color") is None for cell in row["main_bars"])):
            incomplete[names[0]].append("K 24.150, front arrow, and six bars were not visible immediately before clear")
    guard_reference = np.median(np.array([row.get("warning_guard_grid", []) for row in idle_reference]), axis=0)
    if guard_reference.shape != (356,) or guard_reference.sum() < 1:
        incomplete[names[0]].append("warning has no complete original control masks")
    for row in selected:
        warning = row.get("warning", {})
        no_alert = all(row["activity"][field] <= reference["activity"][field] + REGION_ALLOWANCE[field]
                       for field in ("main_bars", "card0", "card1", "arrows"))
        if not _visible(row):
            incomplete[names[0]].append("counter/status visibility guard missing")
        # Keep the complete transition timeline, while allowing the panel the
        # same declared latency used by the onset check to clear the live alert.
        if clear_idle_s is None or row["time_s"] >= clear_idle_s + tolerance_s:
            guard = np.array(row.get("warning_guard_grid", []))
            if (guard.shape != guard_reference.shape or guard.size != 356
                    or np.abs(guard - guard_reference).sum() / max(float(guard_reference.sum()), 1.) > .25):
                incomplete[names[0]].append("counter/status masks differ from the original idle controls")
            if row.get("primary_frequency") is not None or not no_alert:
                failures[names[0]].append("an alert field remained during connected silence")
        state = ("on" if warning.get("text") == "VOL 0" else "off" if warning.get("blank") is True
                 else "idle" if _absent(row, reference)[0] == "absent" else "unreadable")
        row["warning_state"] = state
    changed = [row for row in selected if row["warning_state"] in ("on", "off")]
    if not changed:
        (incomplete if any(row["warning_state"] == "unreadable" for row in selected) else failures)[names[1]].append(
            "warning never visibly replaced the idle placeholder")
    elif not onset_s - tolerance_s <= changed[0]["time_s"] <= onset_s + tolerance_s + .14:
        failures[names[1]].append("warning onset is outside the declared observation allowance")
    before = [row for row in selected if row["time_s"] < onset_s - tolerance_s]
    if any(row["warning_state"] in ("on", "off") for row in before):
        failures[names[1]].append("idle placeholder changed before the warning deadline")
    if any(row["warning_state"] == "unreadable" for row in before):
        incomplete[names[1]].append("pre-warning placeholder is unreadable")
    active = _window(selected, (onset_s + tolerance_s, onset_s + 10. - tolerance_s), .14,
                     "warning blink", incomplete[names[2]])
    blink = _warning_blink(active, transitions or {}, native_presence,
                           (onset_s + tolerance_s, onset_s + 10. - tolerance_s))
    if blink["result"] != "PASS":
        (failures if blink["result"] == "FAIL" else incomplete)[names[2]].append(blink["reason"])
    groups = blink.get("groups", [])
    expired = _window(selected, (onset_s + 10. + tolerance_s + .3, onset_s + 12.), .14,
                      "warning expiry", incomplete[names[3]])
    if any(row["warning_state"] in ("on", "off") for row in expired):
        failures[names[3]].append("original idle placeholder did not return after ten seconds")
    if any(row["warning_state"] == "unreadable" for row in expired):
        incomplete[names[3]].append("post-warning placeholder is unreadable")
    return [{"check": name, **_report(failures=list(dict.fromkeys(failures[name])),
                                      incomplete=list(dict.fromkeys(incomplete[name])),
                                      **({"observed_half_cycles": groups, "edge_intervals": blink.get("edges", []),
                                          "mean_interval_s": blink.get("mean_interval_s"),
                                          "mean_intervals": blink.get("mean_intervals", []),
                                          "native_frame_count": blink.get("native_frame_count") } if name == names[2] else {}))}
            for name in names]



def _report(failures=(), incomplete=(), **detail):
    return {"result": "FAIL" if failures else "INCOMPLETE" if incomplete else "PASS",
            "failures": list(failures), "incomplete": list(incomplete), **detail}


def evaluate_palette(samples, *, expected_frequency_colors, expected_bars,
                     require_card_bars=True, min_samples=3):
    """Check sentinel palette observations from a bounded, unmuted preview.

    Samples carry strictly increasing actual time_s values and may span
    partial-strength frames. Six-cell comparisons admit only
    frames with all six independently readable cells. Missing full-strength
    coverage is INCOMPLETE, never a pass. The runner must bound the preview
    before its muted/laser phases or provide only independently identified
    unmuted frames; this decoder does not infer mute state from expectations.
    """
    if (not expected_frequency_colors or len(expected_bars) != 6 or min_samples < 3 or
            any(value not in COLOR_CLASSES for value in (*expected_frequency_colors.values(), *expected_bars))):
        raise ValueError("palette contract requires supported sentinel colors and at least three observations")
    failures, incomplete = [], []
    coverage = Counter()
    witnesses = []
    for index, sample in enumerate(_timed(samples)):
        frequency = sample.get("primary_frequency")
        if frequency not in expected_frequency_colors or not _visible(sample):
            continue
        observed = sample["frequency_color"]["color"]
        if observed is not None:
            coverage[frequency] += 1
            if observed != expected_frequency_colors[frequency]:
                failures.append({"sample": index, "field": "frequency_color", "frequency": frequency,
                                 "expected": expected_frequency_colors[frequency], "observed": observed})
        for name, cells in (("main_bars", sample["main_bars"]),
                            *((f"card{slot}_bars", cells) for slot, cells in enumerate(sample["card_bars"]))):
            colors = [cell["color"] for cell in cells]
            if len(colors) != 6 or any(color is None for color in colors):
                continue
            coverage[name] += 1
            if colors != list(expected_bars):
                failures.append({"sample": index, "field": name,
                                 "expected": list(expected_bars), "observed": colors})
            witnesses.append({"sample": index, "frequency": frequency, "field": name, "colors": colors})
    for frequency in expected_frequency_colors:
        if coverage[frequency] < min_samples:
            incomplete.append(f"insufficient readable frequency/color samples for {frequency}")
    if coverage["main_bars"] < min_samples:
        incomplete.append("insufficient fully visible six-cell main-meter samples")
    if require_card_bars and coverage["card0_bars"] + coverage["card1_bars"] < min_samples:
        incomplete.append("insufficient fully visible six-cell secondary-meter samples")
    return _report(failures, incomplete, coverage=dict(coverage), witnesses=witnesses)


def _finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def _timed(samples):
    rows = list(samples)
    if any(not _finite(row.get("time_s")) for row in rows):
        raise ValueError("every observation needs a finite time_s on the common evidence clock")
    if any(b["time_s"] <= a["time_s"] for a, b in zip(rows, rows[1:])):
        raise ValueError("observation times must strictly increase")
    return rows


def _window(rows, interval, max_gap, label, incomplete):
    start, end = interval
    if not _finite(start) or not _finite(end) or end <= start:
        raise ValueError("observation windows must have increasing finite endpoints")
    selected = [row for row in rows if start <= row["time_s"] <= end]
    if (len(selected) < 3 or selected[0]["time_s"] - start > max_gap or
            end - selected[-1]["time_s"] > max_gap or
            any(b["time_s"] - a["time_s"] > max_gap for a, b in zip(selected, selected[1:]))):
        incomplete.append(f"{label}: missing samples or a gap exceeds {max_gap:g} seconds")
    return selected


def _visible(sample):
    guards = sample.get("guards", {})
    return guards.get("counter", 0) >= 120 and guards.get("status", 0) >= 120


def _reference(samples, *, cards_only=False, expected_frequency=None):
    rows = _timed(samples)
    if len(rows) < 3 or any(not _visible(row) for row in rows):
        return None, "idle reference needs three independently visible counter/status observations"
    fields = ("card0", "card1") if cards_only else tuple(REGION_ALLOWANCE)
    # These are broad empty-region bounds, independent of an expected alert.
    bounds = {"frequency_vertical": 650, "main_bars": 120, "card0": 150, "card1": 150, "arrows": 180}
    for field in fields:
        if any(row["activity"][field] > bounds[field] for row in rows):
            return None, f"reference is not visibly empty in {field}"
    if not cards_only and any(row.get("primary_frequency") is not None for row in rows):
        return None, "reference contains a numeric primary alert"
    if cards_only and any(row.get("primary_frequency") != expected_frequency for row in rows):
        return None, "empty-card reference needs the same readable primary as the expiry control"
    activity = {field: float(np.median([row["activity"][field] for row in rows])) for field in fields}
    grid = np.median(np.array([row["frequency_grid"] for row in rows]), axis=0)
    if not cards_only and grid.sum() < 1:
        return None, "idle frequency placeholder is not visible in the reference"
    return {"activity": activity, "grid": grid}, None


def _absent(sample, reference, *, cards_only=False):
    if not _visible(sample):
        return "unreadable", ["counter/status visibility guard missing"]
    residual = [field for field, baseline in reference["activity"].items()
                if sample["activity"][field] > baseline + REGION_ALLOWANCE[field]]
    if not cards_only and sample.get("primary_frequency") is not None:
        residual.append("numeric_primary")
    if residual:
        return "present", residual
    if not cards_only:
        reference_grid = reference["grid"]
        change = float(np.abs(np.array(sample["frequency_grid"]) - reference_grid).sum() /
                       max(float(reference_grid.sum()), 1.))
        if change > .55:
            return "unreadable", ["idle placeholder does not match the independently observed reference"]
    return "absent", []


def _evaluate_expiry(samples, idle_reference, *, active_window, clear_time_s, persistence_s,
                     expected_frequency, tolerance_s, observation_s, max_sample_gap_s, cards_only):
    for value in (clear_time_s, persistence_s, tolerance_s, observation_s, max_sample_gap_s):
        if not _finite(value):
            raise ValueError("expiry contract contains a non-finite number")
    if (persistence_s < 0 or tolerance_s <= 0 or observation_s < .5 or
            not 0 < max_sample_gap_s <= .25 or active_window[1] >= clear_time_s or
            (persistence_s > 0 and persistence_s <= 2 * tolerance_s)):
        raise ValueError("expiry contract cannot establish distinct positive/hold/absent windows")
    rows = _timed(samples)
    failures, incomplete = [], []
    reference, error = _reference(idle_reference, cards_only=cards_only, expected_frequency=expected_frequency)
    if error:
        return _report(incomplete=[error])
    active = _window(rows, active_window, max_sample_gap_s, "active control", incomplete)
    for row in active:
        actual = row.get("primary_frequency")
        if not _visible(row) or actual is None:
            incomplete.append("active control: required primary and visibility witnesses were not readable")
            continue
        if actual != expected_frequency:
            failures.append({"time_s": row["time_s"], "field": "active_primary",
                             "expected": expected_frequency, "observed": actual})
            continue
        if cards_only and not any(row["activity"][field] > reference["activity"][field] + REGION_ALLOWANCE[field]
                                  for field in ("card0", "card1")):
            incomplete.append("active control: no secondary card was visibly present")
            break
    hold = []
    if persistence_s > 0:
        hold = _window(rows, (clear_time_s + tolerance_s, clear_time_s + persistence_s - tolerance_s),
                       max_sample_gap_s, "configured hold", incomplete)
        for row in hold:
            actual = row.get("primary_frequency")
            if not _visible(row):
                incomplete.append("configured hold: counter/status visibility guard missing")
                continue
            if actual is not None and actual != expected_frequency:
                failures.append({"time_s": row["time_s"], "field": "held_primary",
                                 "expected": expected_frequency, "observed": actual})
                continue
            if cards_only:
                if row.get("primary_frequency") != expected_frequency or not _visible(row):
                    incomplete.append("configured hold: unchanged primary/control is unreadable")
                    continue
                held = any(row["activity"][field] > reference["activity"][field] + REGION_ALLOWANCE[field]
                           for field in ("card0", "card1"))
            else:
                held = row.get("primary_frequency") == expected_frequency
            if not held:
                state, detail = _absent(row, reference, cards_only=cards_only)
                if state == "absent":
                    failures.append({"time_s": row["time_s"], "field": "hold", "reason": "cleared before configured expiry"})
                else:
                    incomplete.append("configured hold: alert identity or retained card is unreadable")
    deadline = clear_time_s + persistence_s
    expired_window = (deadline + tolerance_s, deadline + tolerance_s + observation_s)
    expired = _window(rows, expired_window, max_sample_gap_s, "expired observation", incomplete)
    for row in expired:
        if cards_only and row.get("primary_frequency") != expected_frequency:
            actual = row.get("primary_frequency")
            if _visible(row) and actual is not None:
                failures.append({"time_s": row["time_s"], "field": "remaining_primary",
                                 "expected": expected_frequency, "observed": actual})
            else:
                incomplete.append("expired observation: live primary no longer provides the unchanged control")
            continue
        state, detail = _absent(row, reference, cards_only=cards_only)
        if state == "present":
            failures.append({"time_s": row["time_s"], "field": "expiry", "residual": detail})
        elif state == "unreadable":
            incomplete.append("expired observation: " + "; ".join(detail))
    # Record an observed bracket separately; requested/host-accepted clear time
    # is not a firmware-receipt timestamp and does not prove end-to-end latency.
    first_absent, last_present = None, None
    for row in rows:
        if row["time_s"] < clear_time_s:
            continue
        state, _ = _absent(row, reference, cards_only=cards_only)
        if state == "present" and first_absent is None:
            last_present = row["time_s"]
        if state == "absent" and first_absent is None:
            first_absent = row["time_s"]
    return _report(failures, sorted(set(incomplete)),
                   counts={"active": len(active), "hold": len(hold), "expired": len(expired)},
                   deadline_s=deadline, tolerance_s=tolerance_s, expired_window_s=list(expired_window),
                   observed_transition_s={"last_present": last_present, "first_absent": first_absent},
                   claim="sampled optical expiry relative to supplied clear anchor; not device-receipt latency")


def evaluate_clearance(samples, idle_reference, *, active_window, clear_time_s, persistence_s,
                       expected_frequency="24.150", tolerance_s=.35, observation_s=1., max_sample_gap_s=.25):
    """Require a visible primary, its configured hold, then complete idle.

    Timed samples are dictionaries containing time_s plus observe_frame fields.
    idle_reference contains independently captured connected-idle observations
    under the same layout, camera, brightness and settings. Three reference
    frames, active control, hold (when nonzero), and an uninterrupted one-second
    sampled absence window are required. Whole-frame blackout cannot pass.
    """
    return _evaluate_expiry(samples, idle_reference, active_window=active_window, clear_time_s=clear_time_s,
                            persistence_s=persistence_s, expected_frequency=expected_frequency,
                            tolerance_s=tolerance_s, observation_s=observation_s,
                            max_sample_gap_s=max_sample_gap_s, cards_only=False)


def evaluate_card_clearance(samples, empty_card_reference, *, active_window, clear_time_s, persistence_s,
                            expected_frequency="34.700", tolerance_s=.35, observation_s=1., max_sample_gap_s=.25):
    """Require a secondary card to expire while the same primary remains visible."""
    return _evaluate_expiry(samples, empty_card_reference, active_window=active_window, clear_time_s=clear_time_s,
                            persistence_s=persistence_s, expected_frequency=expected_frequency,
                            tolerance_s=tolerance_s, observation_s=observation_s,
                            max_sample_gap_s=max_sample_gap_s, cards_only=True)


def evaluate_takeover(samples, *, old_frequency="10.525", new_frequency="34.700",
                      new_alert_time_s, release_time_s, persistence_s,
                      tolerance_s=.35, observation_s=.75, max_sample_gap_s=.25):
    """Require a new live primary to replace a still-unexpired retained primary.

    The old primary must be readable after its release, before the new alert.
    The new primary must then remain readable within the supplied observation
    budget, strictly before the old alert could expire naturally. Secondary
    retention of the old alert is allowed. Times refer to host acceptance, not
    firmware receipt; caller must verify the accepted stimulus identities.
    """
    for value in (new_alert_time_s, release_time_s, persistence_s, tolerance_s,
                  observation_s, max_sample_gap_s):
        if not _finite(value):
            raise ValueError("takeover contract contains a non-finite number")
    old_window = (release_time_s + tolerance_s, new_alert_time_s - .1)
    new_window = (new_alert_time_s + tolerance_s, new_alert_time_s + tolerance_s + observation_s)
    if (not old_frequency or not new_frequency or old_frequency == new_frequency or
            persistence_s <= 0 or tolerance_s <= 0 or observation_s < .5 or
            not 0 < max_sample_gap_s <= .25 or old_window[1] - old_window[0] < .4 or
            new_window[1] > release_time_s + persistence_s - tolerance_s):
        raise ValueError("takeover contract cannot separate retained, live, and natural-expiry windows")
    rows = _timed(samples)
    failures, incomplete = [], []
    counts = {}
    for label, window, expected in (("retained control", old_window, old_frequency),
                                    ("live takeover", new_window, new_frequency)):
        selected = _window(rows, window, max_sample_gap_s, label, incomplete)
        counts[label] = len(selected)
        for row in selected:
            actual = row.get("primary_frequency")
            if not _visible(row) or actual is None:
                incomplete.append(label + ": primary or counter/status witness is unreadable")
            elif actual != expected:
                failures.append({"time_s": row["time_s"], "field": label,
                                 "expected": expected, "observed": actual})
    return _report(failures, sorted(set(incomplete)), counts=counts,
                   retained_window_s=list(old_window), takeover_window_s=list(new_window),
                   natural_expiry_s=release_time_s + persistence_s,
                   tolerance_s=tolerance_s,
                   claim="sampled primary takeover before retained expiry; not device-receipt latency")
