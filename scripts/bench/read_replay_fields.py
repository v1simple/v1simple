#!/usr/bin/env python3
"""Read visible radar fields from replay video and compare with replay stimuli.

Supported view: the registered 1280x720 bench camera with the current
seven-segment frequency/counter and built-in secondary-card font layouts.
This reader handles stable, unmuted K/Ka/X radar states and Photo subtypes on
secondary cards. It reports unsupported or unreadable frames explicitly;
the pixel reader never receives expectations.
Requires ffmpeg, numpy, and Pillow. It does not alter capture artifacts.
"""

from __future__ import annotations

import argparse
import bisect
from collections import Counter
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image


# Order: top, middle, bottom, upper left, upper right, lower left, lower right.
DIGITS = {
    (1, 0, 1, 1, 1, 1, 1): "0",
    (1, 1, 1, 0, 1, 1, 0): "2",
    (1, 1, 1, 0, 1, 0, 1): "3",
    (0, 1, 0, 1, 1, 0, 1): "4",
    (1, 1, 1, 1, 0, 0, 1): "5",
    (1, 1, 1, 1, 0, 1, 1): "6",
    (1, 0, 0, 0, 1, 0, 1): "7",
    (1, 1, 1, 1, 1, 1, 1): "8",
    (1, 1, 1, 1, 1, 0, 1): "9",
}
PTS_TIME = re.compile(r"\bn:\s*0\s+pts:\s*-?\d+\s+pts_time:([\d.]+)")

# The card renderer uses the classic 5x7 GFX font at text size 2. These are
# the needed glyph columns from its glcdfont.h, not captured image samples.
CARD_GLYPHS = {
    "0": (0x3E, 0x51, 0x49, 0x45, 0x3E), "1": (0, 0x42, 0x7F, 0x40, 0),
    "2": (0x72, 0x49, 0x49, 0x49, 0x46), "3": (0x21, 0x41, 0x49, 0x4D, 0x33),
    "4": (0x18, 0x14, 0x12, 0x7F, 0x10), "5": (0x27, 0x45, 0x45, 0x45, 0x39),
    "6": (0x3C, 0x4A, 0x49, 0x49, 0x31), "7": (0x41, 0x21, 0x11, 0x09, 0x07),
    "8": (0x36, 0x49, 0x49, 0x49, 0x36), "9": (0x46, 0x49, 0x49, 0x29, 0x1E),
    ".": (0, 0, 0x60, 0x60, 0),
    "A": (0x7C, 0x12, 0x11, 0x12, 0x7C), "C": (0x3E, 0x41, 0x41, 0x41, 0x22),
    "D": (0x7F, 0x41, 0x41, 0x41, 0x3E), "E": (0x7F, 0x49, 0x49, 0x49, 0x41),
    "H": (0x7F, 0x08, 0x08, 0x08, 0x7F), "I": (0, 0x41, 0x7F, 0x41, 0),
    "K": (0x7F, 0x08, 0x14, 0x22, 0x41), "L": (0x7F, 0x40, 0x40, 0x40, 0x40),
    "M": (0x7F, 0x02, 0x1C, 0x02, 0x7F), "N": (0x7F, 0x04, 0x08, 0x10, 0x7F),
    "O": (0x3E, 0x41, 0x41, 0x41, 0x3E), "P": (0x7F, 0x09, 0x09, 0x09, 0x06),
    "R": (0x7F, 0x09, 0x19, 0x29, 0x46), "T": (0x03, 0x01, 0x7F, 0x01, 0x03),
}
PHOTO_LABELS = {1: "MRCT", 2: "3D", 3: "3DHD", 4: "HALO", 5: "NK7", 6: "EKIN", 7: "RT4",
                255: "PHOTO"}
PHOTO_CANDIDATES = tuple(PHOTO_LABELS.values()) + tuple(f"P{x}" for x in range(8, 16))


def card_templates():
    templates = {}
    for character, columns in CARD_GLYPHS.items():
        pixels = np.array([[(columns[x] >> y) & 1 for x in range(5)] for y in range(7)], dtype=np.uint8)
        doubled = np.repeat(np.repeat(pixels, 2, axis=0), 2, axis=1) * 255
        templates[character] = np.asarray(Image.fromarray(doubled).resize(
            (16, 24), Image.Resampling.BILINEAR)) > 80
    return templates


CARD_TEMPLATES = card_templates()


def ndjson(path):
    with path.open() as source:
        for line in source:
            yield json.loads(line)


def runs(indices):
    result = []
    for value in indices:
        index = int(value)
        if not result or index > result[-1][-1] + 1:
            result.append([index])
        else:
            result[-1].append(index)
    return result


def orange_mask(rgb):
    red, green, blue = (rgb[:, :, channel].astype(np.int16) for channel in range(3))
    return (red > 100) & (red * 4 > green * 5) & (green * 2 > blue * 3) & (green > 30)


def seven_segment(mask):
    """Decode a tightly cropped, lit seven-segment glyph; None means unreadable."""
    height, width = mask.shape
    if height < 40:
        return None
    if width < 25:
        return "1" if height >= 55 else None

    def lit(y0, y1, x0, x1):
        part = mask[int(height * y0):max(int(height * y1), int(height * y0) + 1),
                    int(width * x0):max(int(width * x1), int(width * x0) + 1)]
        return int(part.mean() > .30)

    bits = (lit(0, .13, .15, .85), lit(.43, .57, .15, .85),
            lit(.87, 1, .15, .85), lit(.14, .43, 0, .30),
            lit(.14, .43, .70, 1), lit(.57, .86, 0, .30),
            lit(.57, .86, .70, 1))
    return DIGITS.get(bits)


def read_frequency(mask):
    # Coordinates describe field placement, never the expected characters.
    area = mask[250:365, 450:850]
    components = runs(np.flatnonzero(area.sum(axis=0) > 4))
    symbols = []
    for component in components:
        left, right = component[0], component[-1] + 1
        column = area[:, left:right]
        if column.sum() < 80:  # isolated sensor/color noise
            continue
        rows = np.flatnonzero(column.sum(axis=1))
        glyph = column[rows[0]:rows[-1] + 1]
        if glyph.shape[0] < 40:
            symbols.append(".")
        else:
            symbols.append(seven_segment(glyph) or "?")
    value = "".join(symbols)
    return value if re.fullmatch(r"\d\d\.\d\d\d", value) else None


def read_counter(mask):
    area = mask[188:278, 240:310]
    ys, xs = np.where(area)
    if not len(xs):
        return None
    glyph = area[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    return seven_segment(glyph)


def read_band(rgb):
    bright = np.max(rgb, axis=2) > 120
    votes = {
        "ka": int(bright[250:322, 320:420].sum()),
        "k": int(bright[319:389, 320:395].sum()),
        "x": int(bright[389:459, 320:395].sum()),
    }
    winner = max(votes, key=votes.get)
    other = max(value for key, value in votes.items() if key != winner)
    return winner if votes[winner] > 600 and votes[winner] > 3 * other else None


def read_direction(mask):
    votes = {
        "FRONT": int(mask[182:302, 995:1188].sum()),
        "SIDE": int(mask[302:368, 995:1188].sum()),
        "REAR": int(mask[368:419, 995:1188].sum()),
    }
    winner = max(votes, key=votes.get)
    other = max(value for key, value in votes.items() if key != winner)
    return winner if votes[winner] > 2500 and votes[winner] > 2 * other else None


def card_match_score(observed, template):
    both = int((observed & template).sum())
    extra = int((observed & ~template).sum())
    missing = int((~observed & template).sum())
    return 2 * both / (2 * both + extra + missing + 1e-9)


def read_card_frequency(rgb, slot):
    """Return (visible, frequency); glyph fitting never sees an expected value."""
    if slot not in (0, 1):
        raise ValueError("unsupported secondary-card slot")
    start, stop = ((482, 513) if slot == 0 else (731, 758))
    text = rgb[382:413, start:stop + 120]
    white = np.all(text > 110, axis=2)
    if int(white.sum()) < 150:
        return False, None
    # Translation and character pitch are camera registration parameters. Fit
    # them from observed glyphs, then decode each position from font geometry.
    bright = np.all(rgb > 110, axis=2)
    best = (-1., None, 0.)
    for y in range(383, 390):
        for x0 in range(start, stop):
            for pitch in (19., 19.25, 19.5):
                score = 0.
                smallest = 1.
                characters = []
                for index in range(6):
                    x = round(x0 + index * pitch)
                    cell = bright[y:y + 24, x:x + 16]
                    candidates = "." if index == 2 else "0123456789"
                    ranked = sorted(((card_match_score(cell, CARD_TEMPLATES[character]), character)
                                     for character in candidates), reverse=True)
                    score += ranked[0][0]
                    smallest = min(smallest, ranked[0][0])
                    characters.append(ranked[0][1])
                average = score / 6
                if average > best[0]:
                    best = (average, "".join(characters), smallest)
    return True, best[1] if best[0] >= .75 and best[2] >= .60 else None


def read_card_photo_label(rgb, slot):
    start, stop, right = ((458, 466, 629) if slot == 0 else (705, 713, 876))
    bright = np.max(rgb, axis=2) > 110
    if int(bright[382:413, start:right].sum()) < 80:
        return None
    best = (-1., None, 0.)
    for y in (383, 384, 385, 386):
        for x0 in range(start, stop):
            for pitch in (19., 19.25, 19.5):
                for label in PHOTO_CANDIDATES:
                    tail = round(x0 + len(label) * pitch)
                    if int(bright[y:y + 24, tail:right].sum()) > 50:
                        continue
                    scores = [card_match_score(bright[y:y + 24,
                                                      round(x0 + index * pitch):round(x0 + index * pitch) + 16],
                                               CARD_TEMPLATES[character])
                              for index, character in enumerate(label)]
                    average = sum(scores) / len(scores)
                    if average > best[0]:
                        best = (average, label, min(scores))
    return best[1] if best[0] >= .75 and best[2] >= .60 else None


def read_card_direction(rgb, slot):
    left = 418 if slot == 0 else 665
    white = np.all(rgb[381:414, left:left + 32] > 110, axis=2)
    ys, xs = np.where(white)
    if len(xs) < 100:
        return None
    rows = white[ys.min():ys.max() + 1, xs.min():xs.max() + 1].sum(axis=1)
    if len(rows) <= 10:
        return "SIDE"
    if len(rows) < 17:
        return None
    return "REAR" if rows[:4].mean() > 2 * rows[-4:].mean() else "FRONT"


def read_pixels(rgb):
    """Observed fields only. No stimulus or expected state enters here."""
    if rgb.shape != (720, 1280, 3):
        raise ValueError("unsupported camera geometry")
    orange = orange_mask(rgb)
    cards = []
    for slot in (0, 1):
        visible, frequency = read_card_frequency(rgb, slot)
        direction = read_card_direction(rgb, slot)
        label = None if frequency is not None else read_card_photo_label(rgb, slot)
        cards.append((visible or direction is not None or label is not None,
                      frequency or label, direction))
    return {"frequency": read_frequency(orange), "band": read_band(rgb),
            "direction": read_direction(orange), "counter": read_counter(orange), "cards": cards}


def frame_at(video, when):
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "info",
               "-ss", f"{when:.6f}", "-i", str(video), "-frames:v", "1",
               "-vf", "showinfo", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    result = subprocess.run(command, check=True, capture_output=True)
    match = PTS_TIME.search(result.stderr.decode(errors="replace"))
    if not match or len(result.stdout) != 1280 * 720 * 3:
        raise ValueError("camera frame could not be decoded at stimulus time")
    actual = when + float(match.group(1))
    if abs(actual - when) > .020:
        raise ValueError("decoded camera frame is more than 20 ms from selection")
    return actual, np.frombuffer(result.stdout, dtype=np.uint8).reshape((720, 1280, 3))


def expectation(stimulus):
    state = stimulus["expected"]
    active = [alert for alert in state["alerts"] if alert["priority"]]
    if (len(active) != 1 or state["muted"] or active[0].get("photoType") or
            state["bogeyCounterChar"] not in "123P" or active[0]["band"] not in ("k", "ka", "x")):
        return None
    alert = active[0]
    return {"frequency": f"{alert['frequencyMHz'] / 1000:.3f}", "band": alert["band"],
            "direction": alert["direction"],
            "counter": state["bogeyCounterChar"] if state["bogeyCounterChar"] in "123" else None}


def secondary_identities(stimulus):
    return sorted(((PHOTO_LABELS.get(alert.get("photoType"), f"P{alert['photoType']}")
                    if alert.get("photoType") else f"{alert['frequencyMHz'] / 1000:.3f}"),
                   alert["direction"])
                  for alert in stimulus["expected"]["alerts"]
                  if not alert["priority"] and alert["band"] in ("k", "ka", "x"))


def compare_run(run, limit=None, progress=None):
    window = json.loads((run / "window_result.json").read_text())
    if (window.get("result") != "COMPLETE" or
            window.get("runtime_qualification", {}).get("status") != "qualified" or
            window.get("camera", {}).get("result") != "CAPTURED" or
            window["camera"].get("video_timing_verification_result", {}).get("status") != "verified"):
        raise ValueError("replay capture is not complete and qualified")
    stimuli = list(ndjson(run / "replay_stimulus.ndjson"))
    if len(stimuli) < 3:
        raise ValueError("too few replay stimuli")
    frames = [row for row in ndjson(run / "camera/frame_timing.ndjson") if row["status"] == "written"]
    host = [row["host_capture_ns"] for row in frames]
    if not frames or any(b <= a for a, b in zip(host, host[1:])):
        raise ValueError("missing or nonmonotonic camera timing")
    videos = list((run / "camera").glob("evidence_*.mov"))
    if len(videos) != 1:
        raise ValueError("expected exactly one recorded camera video")
    preflight = json.loads((run / "camera/camera_preflight.json").read_text())
    if preflight.get("registration", {}).get("result") != "PASS":
        raise ValueError("camera registration did not pass")
    video = videos[0]

    eligible = []
    for index in range(1, len(stimuli) - 1):
        current = expectation(stimuli[index])
        if current and current == expectation(stimuli[index - 1]) == expectation(stimuli[index + 1]):
            eligible.append(stimuli[index])
    if limit is not None:
        eligible = eligible[:limit]
    if not eligible:
        raise ValueError("no supported stable radar stimuli")
    by_sequence = {row["stimulusSequence"]: row for row in stimuli}

    totals = {field: {"matched": 0, "mismatched": 0, "unreadable": 0,
                      "blinkOff": 0, "notApplicable": 0} for field in
              ("frequency", "band", "direction", "counter")}
    cards = {"matched": 0, "mismatched": 0, "unreadable": 0, "notApplicable": 0}
    exceptions = []
    for checked, stimulus in enumerate(eligible, 1):
        requested = stimulus["requestedHostMonotonicNs"] + 180_000_000
        index = bisect.bisect_left(host, requested)
        index = min(max(index, 1), len(frames) - 1)
        row = frames[index]
        if abs(row["host_capture_ns"] - requested) > 20_000_000:
            raise ValueError("camera timing has no nearby frame")
        when = (row["video_pts_value"] / row["video_pts_timescale"] +
                (requested - row["host_capture_ns"]) / 1e9)
        video_time, rgb = frame_at(video, when)
        observed = read_pixels(rgb)
        expected = expectation(stimulus)
        for field in totals:
            if expected[field] is None:
                totals[field]["notApplicable"] += 1
                continue
            blink_off = ((field == "band" and stimulus["expected"]["bandBlink"]) or
                         (field == "direction" and stimulus["expected"]["arrowBlink"]))
            category = ("blinkOff" if observed[field] is None and blink_off else
                        "unreadable" if observed[field] is None else
                        "matched" if observed[field] == expected[field] else "mismatched")
            totals[field][category] += 1
            if category in ("mismatched", "unreadable") and len(exceptions) < 30:
                exceptions.append({"sequence": stimulus["stimulusSequence"], "videoSeconds": round(video_time, 3),
                                   "field": field, "expected": expected[field], "observed": observed[field],
                                   "category": category})
        sequence = stimulus["stimulusSequence"]
        expected_cards = secondary_identities(stimulus)
        neighbors = (by_sequence.get(sequence - 1), by_sequence.get(sequence + 1))
        if not expected_cards or None in neighbors or any(
            secondary_identities(neighbor) != expected_cards for neighbor in neighbors
        ):
            cards["notApplicable"] += 1
        else:
            visible = [(frequency, direction) for present, frequency, direction in observed["cards"] if present]
            needed = Counter(expected_cards)
            seen = Counter(identity for identity in visible if None not in identity)
            if all(seen[value] >= count for value, count in needed.items()):
                category = "matched"
            elif any(None in identity for identity in visible):
                category = "unreadable"
            else:
                category = "mismatched"
            cards[category] += 1
            if category != "matched" and len(exceptions) < 30:
                exceptions.append({"sequence": sequence, "videoSeconds": round(video_time, 3),
                                   "field": "secondaryCardTextDirection", "expected": expected_cards,
                                   "observed": visible, "category": category})
        if progress is not None and (checked % 100 == 0 or checked == len(eligible)):
            progress(checked, len(eligible))
    return {"result": ("FAIL" if any(v["mismatched"] for v in totals.values()) else
                       "FAIL" if cards["mismatched"] else
                       "INCONCLUSIVE" if cards["unreadable"] or any(v["unreadable"] for v in totals.values()) or
                       any(not v["matched"] for v in totals.values()) else "PASS"),
            "scope": "registered 1280x720 view; unmuted, stable K/Ka/X radar states; one camera frame per replay step; blink-off arrows and bands are permitted",
            "source": {"gitSha": window["git_sha"],
                       "imageId": window["runtime_identity"]["image_id"],
                       "cameraCaptureId": window["camera"]["capture_id"],
                       "displayContractSha256": window["artifacts"]["replay_display_contract"]["sha256"]},
            "framesChecked": len(eligible), "fields": totals, "secondaryCardTextDirection": cards,
            "exceptions": exceptions}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path, help="replay artifact directory")
    parser.add_argument("--limit", type=int, help="analyze the first N eligible stimuli while developing")
    parser.add_argument("--output", type=Path, help="write a separate post-capture JSON report")
    parser.add_argument("--progress", action="store_true", help="report post-capture decoding progress")
    args = parser.parse_args()
    try:
        if not shutil.which("ffmpeg"):
            raise ValueError("ffmpeg is required")
        progress = (lambda checked, total: print(
            f"[bench] visual fields {checked}/{total} sampled frames", file=sys.stderr, flush=True
        )) if args.progress else None
        report = compare_run(args.run, args.limit, progress)
    except (ValueError, KeyError, FileNotFoundError, subprocess.CalledProcessError) as error:
        report = {"result": "INCONCLUSIVE", "reason": str(error)}
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output is None:
        print(rendered, end="")
    else:
        temporary = args.output.with_name(args.output.name + ".tmp")
        temporary.write_text(rendered)
        temporary.replace(args.output)
    raise SystemExit({"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 2}[report["result"]])


if __name__ == "__main__":
    main()
