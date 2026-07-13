#!/usr/bin/env python3
"""Doc-as-spec gate: protocol tables in docs/V1_PROTOCOL_REFERENCES.md are the
single source of truth for decode tables. This script parses the machine-
readable spec blocks, regenerates test/fixtures/protocol_spec_tables.h, and
fails when the committed header drifts from the document.

Usage:
  python3 scripts/check_protocol_spec_tables.py          # verify (CI gate)
  python3 scripts/check_protocol_spec_tables.py --write  # regenerate header

The generated header is consumed by test_protocol_spec_conformance, which
drives the real parse path with these documented values. Stdlib only.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "V1_PROTOCOL_REFERENCES.md"
HEADER = ROOT / "test" / "fixtures" / "protocol_spec_tables.h"

EXPECTED_TABLES = (
    "led-bitmap-bars",
    "alert-band-values",
    "alert-direction-bits",
    "strength-thresholds-ka",
    "strength-thresholds-x",
    "strength-thresholds-k",
    "vr-to-card-bars",
    "alert-row-layout",
    "alert-aux0-bits",
    "user-bytes-bit-map",
)

BAND_NAMES = ("LASER", "KA", "K", "X", "KU")
DIRECTION_NAMES = ("FRONT", "SIDE", "REAR")

BLOCK_RE = re.compile(
    r"<!--\s*spec-table:\s*(?P<name>[a-z0-9-]+)\s*-->\s*\n```text\n(?P<body>.*?)```",
    re.DOTALL,
)
LINE_RE = re.compile(r"^\s*(?P<key>0x[0-9A-Fa-f]{2}|\d+|overflow)\s*->\s*(?P<value>\w+)\s*$")
USER_BYTE_LINE_RE = re.compile(
    r"^\s*byte(?P<byte>[0-5])\s+(?P<mask>0x[0-9A-Fa-f]{2})\s+"
    r"(?P<kind>direct|inverted|field)\s*->\s*(?P<name>\w+)\s*$"
)


def parse_tables(text: str) -> dict[str, list[tuple[str, str]]]:
    tables: dict[str, list[tuple[str, str]]] = {}
    for match in BLOCK_RE.finditer(text):
        name = match.group("name")
        rows: list[tuple[str, str]] = []
        for raw_line in match.group("body").splitlines():
            line = raw_line.strip()
            if not line:
                continue
            if name == "user-bytes-bit-map":
                ub = USER_BYTE_LINE_RE.match(line)
                if not ub:
                    raise ValueError(f"spec-table {name}: unparsable line {raw_line!r}")
                rows.append((f"{ub.group('byte')}:{ub.group('mask')}:{ub.group('kind')}",
                             ub.group("name")))
                continue
            line_match = LINE_RE.match(line)
            if not line_match:
                raise ValueError(f"spec-table {name}: unparsable line {raw_line!r}")
            rows.append((line_match.group("key"), line_match.group("value")))
        if name in tables:
            raise ValueError(f"duplicate spec-table {name}")
        tables[name] = rows
    return tables


def key_int(key: str) -> int:
    return int(key, 16) if key.startswith("0x") else int(key)


def validate(tables: dict[str, list[tuple[str, str]]]) -> list[str]:
    errors: list[str] = []
    for name in EXPECTED_TABLES:
        if name not in tables:
            errors.append(f"missing spec-table: {name}")
    if errors:
        return errors

    bitmap = tables["led-bitmap-bars"]
    overflow_rows = [row for row in bitmap if row[0] == "overflow"]
    if len(overflow_rows) != 1:
        errors.append("led-bitmap-bars must define exactly one overflow row")
    normal = [(key_int(k), int(v)) for k, v in bitmap if k != "overflow"]
    expected_bitmap_keys = [0x00, 0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0xFF]
    if [bits for bits, _ in normal] != expected_bitmap_keys:
        errors.append("led-bitmap-bars: must list contiguous source bitmaps "
                      "0x00,0x01,0x03,0x07,0x0F,0x1F,0x3F,0x7F,0xFF in order")
    previous_bars = -1
    for bits, bars in normal:
        if bars < 0 or bars > 8:
            errors.append(f"led-bitmap-bars: 0x{bits:02X} maps outside local 0..8 display bars")
        if bars < previous_bars:
            errors.append(f"led-bitmap-bars: 0x{bits:02X} lowers local display strength")
        previous_bars = bars
    if dict(normal).get(0x00) != 0:
        errors.append("led-bitmap-bars: 0x00 must render 0 local bars")
    if dict(normal).get(0x3F) != 8:
        errors.append("led-bitmap-bars: V1G2 six-source-bar full scale (0x3F) "
                      "must render all 8 local bars")
    if overflow_rows and int(overflow_rows[0][1]) != 8:
        errors.append("led-bitmap-bars: overflow must render all 8 local bars")

    for name, allowed in (("alert-band-values", BAND_NAMES),
                          ("alert-direction-bits", DIRECTION_NAMES)):
        for key, value in tables[name]:
            if value not in allowed:
                errors.append(f"{name}: unknown symbol {value}")
            if key == "overflow":
                errors.append(f"{name}: overflow row not allowed")

    for suffix in ("ka", "x", "k"):
        rows = [(key_int(k), int(v)) for k, v in tables[f"strength-thresholds-{suffix}"]]
        if [bars for _, bars in rows] != list(range(8, 0, -1)):
            errors.append(f"strength-thresholds-{suffix}: must list bars 8..1 in order")
        raws = [raw for raw, _ in rows]
        if raws != sorted(raws, reverse=True):
            errors.append(f"strength-thresholds-{suffix}: raw thresholds must be strictly descending")

    layout = [(key_int(k), v) for k, v in tables["alert-row-layout"]]
    if [off for off, _ in layout] != list(range(0, 7)):
        errors.append("alert-row-layout: must cover offsets 0..6 in order")
    expected_layout = ["INDEX_COUNT", "FREQ_MSB", "FREQ_LSB", "FRONT_RSSI",
                       "REAR_RSSI", "BAND_ARROW", "AUX0"]
    if [name for _, name in layout] != expected_layout:
        errors.append("alert-row-layout: field names/order changed — update the "
                      "conformance test and this validator together")

    aux_masks = [(key_int(k), v) for k, v in tables["alert-aux0-bits"]]
    combined = 0
    for mask, _ in aux_masks:
        if combined & mask:
            errors.append("alert-aux0-bits: overlapping masks")
        combined |= mask

    seen_bits: dict[int, int] = {}
    for key, name in tables["user-bytes-bit-map"]:
        byte_s, mask_s, kind = key.split(":")
        byte_i, mask = int(byte_s), int(mask_s, 16)
        if mask == 0:
            errors.append(f"user-bytes-bit-map: zero mask for {name}")
        if kind in ("direct", "inverted") and mask & (mask - 1):
            errors.append(f"user-bytes-bit-map: {name} is {kind} but mask "
                          f"0x{mask:02X} is not a single bit")
        if seen_bits.get(byte_i, 0) & mask:
            errors.append(f"user-bytes-bit-map: byte{byte_i} mask overlap at {name}")
        seen_bits[byte_i] = seen_bits.get(byte_i, 0) | mask

    compression = [(key_int(k), int(v)) for k, v in tables["vr-to-card-bars"]]
    if [vr for vr, _ in compression] != list(range(0, 9)):
        errors.append("vr-to-card-bars: must cover vrBars 0..8 in order")
    for vr, card in compression:
        if card != (vr * 6 + 4) // 8:
            errors.append(f"vr-to-card-bars: {vr} -> {card} breaks (vr*6+4)/8 rounding")

    return errors


def render_header(tables: dict[str, list[tuple[str, str]]]) -> str:
    bitmap_rows = [(key_int(k), int(v)) for k, v in tables["led-bitmap-bars"] if k != "overflow"]
    overflow_bars = int(dict(tables["led-bitmap-bars"])["overflow"])
    band_rows = [(key_int(k), v) for k, v in tables["alert-band-values"]]
    dir_rows = [(key_int(k), v) for k, v in tables["alert-direction-bits"]]
    compression = [int(v) for _, v in tables["vr-to-card-bars"]]

    def strength_rows(suffix: str) -> list[tuple[int, int]]:
        return [(key_int(k), int(v)) for k, v in tables[f"strength-thresholds-{suffix}"]]

    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("// GENERATED FILE — do not edit by hand.")
    lines.append("// Source of truth: docs/V1_PROTOCOL_REFERENCES.md (machine-readable spec tables).")
    lines.append("// Regenerate: python3 scripts/check_protocol_spec_tables.py --write")
    lines.append("// CI gate:    python3 scripts/check_protocol_spec_tables.py")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("namespace protocol_spec {")
    lines.append("")
    lines.append("enum class SpecBand : uint8_t { LASER, KA, K, X, KU };")
    lines.append("enum class SpecDirection : uint8_t { FRONT, SIDE, REAR };")
    lines.append("")
    lines.append("struct BitmapBarsCase { uint8_t bitmap; uint8_t bars; };")
    lines.append("inline constexpr BitmapBarsCase kLedBitmapBars[] = {")
    for bits, bars in bitmap_rows:
        lines.append(f"    {{0x{bits:02X}, {bars}}},")
    lines.append("};")
    lines.append(f"inline constexpr uint8_t kLedBitmapOverflowBars = {overflow_bars};")
    lines.append("")
    lines.append("struct BandCase { uint8_t byte; SpecBand band; };")
    lines.append("inline constexpr BandCase kAlertBandValues[] = {")
    for byte, name in band_rows:
        lines.append(f"    {{0x{byte:02X}, SpecBand::{name}}},")
    lines.append("};")
    lines.append("")
    lines.append("struct DirectionCase { uint8_t bit; SpecDirection direction; };")
    lines.append("inline constexpr DirectionCase kAlertDirectionBits[] = {")
    for bit, name in dir_rows:
        lines.append(f"    {{0x{bit:02X}, SpecDirection::{name}}},")
    lines.append("};")
    lines.append("")
    lines.append("// Minimum raw RSSI for each VR bargraph bar count (8..1), per band.")
    lines.append("struct StrengthThreshold { uint8_t minRaw; uint8_t vrBars; };")
    for suffix, ident in (("ka", "Ka"), ("x", "X"), ("k", "K")):
        lines.append(f"inline constexpr StrengthThreshold kStrengthThresholds{ident}[] = {{")
        for raw, bars in strength_rows(suffix):
            lines.append(f"    {{0x{raw:02X}, {bars}}},")
        lines.append("};")
    lines.append("inline constexpr uint8_t kLaserVrBars = 8;")
    lines.append("")
    lines.append("// vrBars (index 0..8) -> six-bar secondary-card scale.")
    lines.append("inline constexpr uint8_t kVrToCardBars[9] = {")
    lines.append("    " + ", ".join(str(v) for v in compression))
    lines.append("};")
    lines.append("")
    lines.append("// Alert-row byte offsets (see docs: alert-row-layout).")
    layout_rows = [(key_int(k), v) for k, v in tables["alert-row-layout"]]
    for off, name in layout_rows:
        lines.append(f"inline constexpr uint8_t kAlertRowOffset{name.title().replace('_','')} = {off};")
    lines.append("")
    lines.append("// Alert-row aux0 bit masks (see docs: alert-aux0-bits).")
    for key, name in tables["alert-aux0-bits"]:
        lines.append(f"inline constexpr uint8_t kAlertAux0{name.title().replace('_','')}Mask = 0x{key_int(key):02X};")
    lines.append("")
    lines.append("// V1 user-settings bytes bit map (see docs: user-bytes-bit-map).")
    lines.append("// kind: 0=direct (bit set == ON), 1=inverted (bit clear == ON), 2=multi-bit field.")
    lines.append("struct UserByteField { uint8_t byteIndex; uint8_t mask; uint8_t kind; const char* name; };")
    lines.append("inline constexpr UserByteField kUserBytesBitMap[] = {")
    kind_code = {"direct": 0, "inverted": 1, "field": 2}
    for key, name in tables["user-bytes-bit-map"]:
        byte_s, mask_s, kind = key.split(":")
        lines.append(f"    {{{int(byte_s)}, 0x{int(mask_s,16):02X}, {kind_code[kind]}, \"{name}\"}},")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace protocol_spec")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    write = "--write" in sys.argv[1:]

    if not DOC.is_file():
        print(f"[spec-tables] missing {DOC}", file=sys.stderr)
        return 1

    try:
        tables = parse_tables(DOC.read_text(encoding="utf-8"))
        errors = validate(tables)
    except ValueError as exc:
        print(f"[spec-tables] {exc}", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"[spec-tables] {error}", file=sys.stderr)
        return 1

    rendered = render_header(tables)

    if write:
        HEADER.parent.mkdir(parents=True, exist_ok=True)
        HEADER.write_text(rendered, encoding="utf-8")
        print(f"[spec-tables] wrote {HEADER.relative_to(ROOT)}")
        return 0

    if not HEADER.is_file():
        print(f"[spec-tables] missing generated header {HEADER.relative_to(ROOT)}; "
              "run with --write", file=sys.stderr)
        return 1

    if HEADER.read_text(encoding="utf-8") != rendered:
        print("[spec-tables] generated header is stale relative to the spec tables in "
              "docs/V1_PROTOCOL_REFERENCES.md; run "
              "python3 scripts/check_protocol_spec_tables.py --write", file=sys.stderr)
        return 1

    print(f"[spec-tables] {len(tables)} tables verified; fixtures header in sync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
