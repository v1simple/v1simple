#!/usr/bin/env python3
"""Verify modified embedded fonts do not carry Reserved Font Names."""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FONT_HEADER = ROOT / "include" / "Segment7Font.h"
RESERVED_NAME = "Segment7"
RENAMED_SYMBOL = "V1SevenXFont"


def font_bytes_from_header(path: Path) -> bytes:
    text = path.read_text(encoding="utf-8")
    values = [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", text)]
    if not values:
        raise ValueError(f"{path.relative_to(ROOT)} does not contain hex font data")
    return bytes(values)


def table_directory(data: bytes) -> dict[str, tuple[int, int]]:
    if len(data) < 12:
        raise ValueError("font data too short for sfnt header")
    count = struct.unpack_from(">H", data, 4)[0]
    tables: dict[str, tuple[int, int]] = {}
    offset = 12
    for _ in range(count):
        if offset + 16 > len(data):
            raise ValueError("font table directory truncated")
        tag = data[offset : offset + 4].decode("latin1")
        _, table_offset, length = struct.unpack_from(">III", data, offset + 4)
        if table_offset + length > len(data):
            raise ValueError(f"font table {tag!r} extends past end of data")
        tables[tag] = (table_offset, length)
        offset += 16
    return tables


def decode_name_record(platform_id: int, payload: bytes) -> str:
    if platform_id in (0, 3):
        return payload.decode("utf-16-be", errors="replace")
    return payload.decode("macroman", errors="replace")


def font_name_records(data: bytes) -> list[str]:
    tables = table_directory(data)
    if "name" not in tables:
        raise ValueError("font is missing name table")
    name_offset, name_length = tables["name"]
    name_data = data[name_offset : name_offset + name_length]
    if len(name_data) < 6:
        raise ValueError("name table too short")
    _, count, string_offset = struct.unpack_from(">HHH", name_data, 0)
    records: list[str] = []
    for index in range(count):
        record_offset = 6 + index * 12
        if record_offset + 12 > len(name_data):
            raise ValueError("name record table truncated")
        platform_id, _, _, _, length, offset = struct.unpack_from(">HHHHHH", name_data, record_offset)
        start = string_offset + offset
        end = start + length
        if end > len(name_data):
            raise ValueError("name string extends past name table")
        records.append(decode_name_record(platform_id, name_data[start:end]))
    return records


def main() -> int:
    text = FONT_HEADER.read_text(encoding="utf-8")
    if re.search(rf"\b{re.escape(RESERVED_NAME)}Font(?:_size)?\b", text):
        print(f"[font] reserved symbol name still present in {FONT_HEADER.relative_to(ROOT)}")
        return 1
    if RENAMED_SYMBOL not in text:
        print(f"[font] expected renamed symbol {RENAMED_SYMBOL} in {FONT_HEADER.relative_to(ROOT)}")
        return 1

    data = font_bytes_from_header(FONT_HEADER)
    reserved_ascii = RESERVED_NAME.encode("ascii")
    reserved_utf16 = RESERVED_NAME.encode("utf-16-be")
    if reserved_ascii in data or reserved_utf16 in data:
        print(f"[font] reserved font name {RESERVED_NAME!r} still appears in embedded bytes")
        return 1

    offenders = [record for record in font_name_records(data) if RESERVED_NAME in record]
    if offenders:
        print(f"[font] reserved font name {RESERVED_NAME!r} still appears in name table")
        for record in offenders:
            print(f"  - {record}")
        return 1

    print(f"[font] modified font names comply with reserved-name policy ({RENAMED_SYMBOL})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
