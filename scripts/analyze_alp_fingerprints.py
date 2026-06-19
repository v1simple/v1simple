#!/usr/bin/env python3
"""Summarize ALP raw-frame fingerprints from alp_*.csv logs.

This reader intentionally avoids strict schema-width checks. It uses the
required header names only and ignores any extra trailing columns so new ALP
log revisions can still be mined without first updating the script.

Usage:
    python3 scripts/analyze_alp_fingerprints.py /path/to/logs
    python3 scripts/analyze_alp_fingerprints.py /path/to/logs --json

The report focuses on two questions:

1. Which C8..CE gun-family fingerprints are present versus the firmware's
    current detect/deploy lookup tables?
2. Which non-C valid raw-frame byte0 values would fall through the runtime's
    dispatch map to the UNKNOWN_FRAME trace path, if any are present in the log?

Note: the current firmware does not persist UNKNOWN_FRAME fallback traces into
the ALP CSV. This script can only report unknown-dispatch rows that are present
as full raw-frame log entries in the CSV data itself.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


REQUIRED_COLUMNS = ("millis", "event", "byte0", "byte1", "byte2")

ALERT_BYTE0 = 0x98
HEARTBEAT_BYTE0S = {0xB0, 0xB8, 0xE0, 0xA8, 0xF0}
DISCOVERY_BYTE0 = 0x91

KNOWN_DETECT = {
    (0xC8, 0x0D): "PL3_PROLITE",
    (0xC8, 0x11): "DRAGONEYE_COMPACT",
    (0xC9, 0x0E): "LTI_TRUSPEED_LR",
    (0xCB, 0x10): "LASER_ATLANTA_PL2",
    (0xCD, 0x0C): "MARKSMAN_ULTRALYTE",
    (0xCD, 0x0D): "STALKER_LZ1",
    (0xCD, 0x10): "LASER_ALLY",
    (0xCE, 0x0C): "ATLANTA_STEALTH",
}

KNOWN_DEPLOY = {
    (0xC8, 0xD5): "PL3_PROLITE",
    (0xC8, 0xD6): "DRAGONEYE_COMPACT",
    (0xC9, 0xF5): "LTI_TRUSPEED_LR",
    (0xCB, 0xEB): "LASER_ATLANTA_PL2",
    (0xCD, 0xD6): "MARKSMAN_ULTRALYTE",
    (0xCD, 0xEB): "STALKER_LZ1",
    (0xCD, 0xD7): "LASER_ALLY",
    (0xCE, 0xEB): "ATLANTA_STEALTH",
}


@dataclass(frozen=True)
class WarningRecord:
    file: str
    line: int
    message: str


@dataclass(frozen=True)
class ExampleRecord:
    file: str
    line: int
    millis: int
    event: str | None
    byte0: int
    byte1: int
    byte2: int
    checksum: int | None
    direction: str | None
    gun: str | None
    extra: str | None


def parse_hex_field(value: str | None) -> int | None:
    if value is None:
        return None
    stripped = value.strip()
    if not stripped:
        return None
    return int(stripped, 16)


def alp_checksum(byte0: int, byte1: int, byte2: int) -> int:
    return (byte0 + byte1 + byte2) & 0x7F


def classify_runtime_dispatch(byte0: int) -> str:
    if byte0 == ALERT_BYTE0:
        return "alert_or_status"
    if byte0 in HEARTBEAT_BYTE0S:
        return "heartbeat_or_setup"
    if 0xC8 <= byte0 <= 0xCE:
        return "gun_family"
    if 0xD0 <= byte0 <= 0xD3:
        return "register_write"
    if byte0 == DISCOVERY_BYTE0:
        return "discovery"
    return "unknown_dispatch"


def iter_alp_rows(directory: Path) -> tuple[list[dict[str, object]], list[WarningRecord]]:
    rows: list[dict[str, object]] = []
    warnings: list[WarningRecord] = []

    for path in sorted(directory.glob("alp_*.csv")):
        with open(path, newline="") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                warnings.append(WarningRecord(path.name, 1, "empty file"))
                continue

            missing = [column for column in REQUIRED_COLUMNS if column not in reader.fieldnames]
            if missing:
                warnings.append(
                    WarningRecord(path.name, 1, f"missing required columns: {', '.join(missing)}")
                )
                continue

            for line_no, row in enumerate(reader, start=2):
                try:
                    millis_raw = (row.get("millis") or "").strip()
                    if not millis_raw:
                        continue
                    parsed = {
                        "file": path.name,
                        "line": line_no,
                        "millis": int(millis_raw),
                        "event": (row.get("event") or "").strip() or None,
                        "byte0": parse_hex_field(row.get("byte0")),
                        "byte1": parse_hex_field(row.get("byte1")),
                        "byte2": parse_hex_field(row.get("byte2")),
                        "checksum": parse_hex_field(row.get("checksum")),
                        "direction": (row.get("direction") or "").strip() or None,
                        "gun": (row.get("gun") or "").strip() or None,
                        "extra": (row.get("extra") or "").strip() or None,
                    }
                except ValueError as exc:
                    warnings.append(WarningRecord(path.name, line_no, f"parse error: {exc}"))
                    continue

                rows.append(parsed)

    return rows, warnings


def format_pair(pair: tuple[int, int]) -> str:
    return f"{pair[0]:02X} {pair[1]:02X}"


def format_triple(triple: tuple[int, int, int]) -> str:
    return f"{triple[0]:02X} {triple[1]:02X} {triple[2]:02X}"


def summarize(directory: Path, example_limit: int) -> dict[str, object]:
    rows, warnings = iter_alp_rows(directory)

    detect_known_seen: Counter[tuple[int, int]] = Counter()
    deploy_known_seen: Counter[tuple[int, int]] = Counter()
    detect_unknown_seen: Counter[tuple[int, int]] = Counter()
    deploy_unknown_seen: Counter[tuple[int, int]] = Counter()
    other_shapes_seen: Counter[tuple[int, int, int]] = Counter()
    file_unknown_counts: Counter[str] = Counter()
    raw_dispatch_counts: Counter[str] = Counter()
    unknown_dispatch_shapes: Counter[tuple[int, int, int]] = Counter()
    invalid_checksum_shapes: Counter[tuple[int, int, int, int]] = Counter()
    file_unknown_dispatch_counts: Counter[str] = Counter()
    example_map: defaultdict[object, list[ExampleRecord]] = defaultdict(list)
    shape_counts: Counter[str] = Counter()
    c_frame_rows = 0
    raw_frame_rows = 0
    valid_checksum_rows = 0
    invalid_checksum_rows = 0

    for row in rows:
        byte0 = row["byte0"]
        byte1 = row["byte1"]
        byte2 = row["byte2"]
        checksum = row["checksum"]
        example = ExampleRecord(
            file=row["file"],
            line=row["line"],
            millis=row["millis"],
            event=row["event"],
            byte0=byte0,
            byte1=byte1,
            byte2=byte2,
            checksum=checksum,
            direction=row["direction"],
            gun=row["gun"],
            extra=row["extra"],
        )

        if byte0 is not None and byte1 is not None and byte2 is not None and checksum is not None:
            raw_frame_rows += 1
            if checksum == alp_checksum(byte0, byte1, byte2):
                valid_checksum_rows += 1
                dispatch = classify_runtime_dispatch(byte0)
                raw_dispatch_counts[dispatch] += 1
                if dispatch == "unknown_dispatch":
                    triple = (byte0, byte1, byte2)
                    unknown_dispatch_shapes[triple] += 1
                    file_unknown_dispatch_counts[row["file"]] += 1
                    example_key = ("unknown-dispatch", triple)
                    if len(example_map[example_key]) < example_limit:
                        example_map[example_key].append(example)
            else:
                invalid_checksum_rows += 1
                quad = (byte0, byte1, byte2, checksum)
                invalid_checksum_shapes[quad] += 1
                example_key = ("invalid-checksum", quad)
                if len(example_map[example_key]) < example_limit:
                    example_map[example_key].append(example)

        if byte0 is None or byte1 is None or byte2 is None:
            continue
        if not (0xC8 <= byte0 <= 0xCE):
            continue

        c_frame_rows += 1

        if byte1 == 0x00 and byte2 != 0x00:
            shape_counts["deploy"] += 1
            key = (byte0, byte2)
            if key in KNOWN_DEPLOY:
                deploy_known_seen[key] += 1
                example_key: object = ("deploy-known", key)
            else:
                deploy_unknown_seen[key] += 1
                file_unknown_counts[row["file"]] += 1
                example_key = ("deploy-unknown", key)
            if len(example_map[example_key]) < example_limit:
                example_map[example_key].append(example)
            continue

        if byte2 == 0x00 and byte1 != 0x00:
            shape_counts["detect"] += 1
            key = (byte0, byte1)
            if key in KNOWN_DETECT:
                detect_known_seen[key] += 1
                example_key = ("detect-known", key)
            else:
                detect_unknown_seen[key] += 1
                file_unknown_counts[row["file"]] += 1
                example_key = ("detect-unknown", key)
            if len(example_map[example_key]) < example_limit:
                example_map[example_key].append(example)
            continue

        shape_counts["other"] += 1
        triple = (byte0, byte1, byte2)
        other_shapes_seen[triple] += 1
        file_unknown_counts[row["file"]] += 1
        example_key = ("other", triple)
        if len(example_map[example_key]) < example_limit:
            example_map[example_key].append(example)

    def pack_pairs(counter: Counter[tuple[int, int]], known: dict[tuple[int, int], str], category: str) -> list[dict[str, object]]:
        items = []
        for key, count in counter.most_common():
            label = known.get(key)
            items.append({
                "fingerprint": format_pair(key),
                "count": count,
                "gun": label,
                "examples": [asdict(item) for item in example_map[(category, key)]],
            })
        return items

    def pack_triples(counter: Counter[tuple[int, int, int]]) -> list[dict[str, object]]:
        items = []
        for key, count in counter.most_common():
            items.append({
                "frame": format_triple(key),
                "count": count,
                "examples": [asdict(item) for item in example_map[("other", key)]],
            })
        return items

    def pack_unknown_dispatch(counter: Counter[tuple[int, int, int]]) -> list[dict[str, object]]:
        items = []
        for key, count in counter.most_common():
            items.append({
                "frame": format_triple(key),
                "count": count,
                "examples": [asdict(item) for item in example_map[("unknown-dispatch", key)]],
            })
        return items

    def pack_invalid_checksums(counter: Counter[tuple[int, int, int, int]]) -> list[dict[str, object]]:
        items = []
        for key, count in counter.most_common():
            items.append({
                "frame": f"{key[0]:02X} {key[1]:02X} {key[2]:02X} {key[3]:02X}",
                "count": count,
                "expected_checksum": f"{alp_checksum(key[0], key[1], key[2]):02X}",
                "examples": [asdict(item) for item in example_map[("invalid-checksum", key)]],
            })
        return items

    return {
        "directory": str(directory),
        "alp_files": len(list(directory.glob("alp_*.csv"))),
        "parsed_rows": len(rows),
        "warnings": [asdict(warning) for warning in warnings],
        "raw_frame_rows": raw_frame_rows,
        "valid_checksum_rows": valid_checksum_rows,
        "invalid_checksum_rows": invalid_checksum_rows,
        "runtime_dispatch_counts": dict(raw_dispatch_counts),
        "unknown_dispatch_shapes": pack_unknown_dispatch(unknown_dispatch_shapes),
        "invalid_checksum_shapes": pack_invalid_checksums(invalid_checksum_shapes),
        "files_with_unknown_dispatch_rows": [
            {"file": name, "count": count}
            for name, count in file_unknown_dispatch_counts.most_common()
        ],
        "c_frame_rows": c_frame_rows,
        "shape_counts": dict(shape_counts),
        "detect_known": pack_pairs(detect_known_seen, KNOWN_DETECT, "detect-known"),
        "detect_unknown": pack_pairs(detect_unknown_seen, {}, "detect-unknown"),
        "deploy_known": pack_pairs(deploy_known_seen, KNOWN_DEPLOY, "deploy-known"),
        "deploy_unknown": pack_pairs(deploy_unknown_seen, {}, "deploy-unknown"),
        "other_shapes": pack_triples(other_shapes_seen),
        "files_with_unknown_candidates": [
            {"file": name, "count": count}
            for name, count in file_unknown_counts.most_common()
        ],
    }


def print_text(summary: dict[str, object]) -> None:
    print(f"Directory: {summary['directory']}")
    print(f"ALP files: {summary['alp_files']}")
    print(f"Parsed rows: {summary['parsed_rows']}")
    print(f"Warnings: {len(summary['warnings'])}")
    print(f"Raw-frame rows: {summary['raw_frame_rows']}")
    print(f"Valid-checksum rows: {summary['valid_checksum_rows']}")
    print(f"Invalid-checksum rows: {summary['invalid_checksum_rows']}")
    print(f"Runtime dispatch counts: {summary['runtime_dispatch_counts']}")
    print("UNKNOWN_FRAME caveat: current firmware does not persist fallback UNKNOWN_FRAME traces to CSV;")
    print("this section only reports unknown-dispatch rows that are present as full raw-frame log entries.")
    print()

    print("Non-C valid frames that would hit UNKNOWN_FRAME:")
    if not summary["unknown_dispatch_shapes"]:
        print("  none")
    for item in summary["unknown_dispatch_shapes"]:
        print(f"  {item['frame']} count={item['count']}")
        for example in item["examples"]:
            print(
                "    "
                f"{example['file']}:{example['line']} ms={example['millis']} "
                f"event={example['event']}"
            )
    print()

    print("Invalid-checksum raw frames:")
    if not summary["invalid_checksum_shapes"]:
        print("  none")
    for item in summary["invalid_checksum_shapes"]:
        print(
            f"  {item['frame']} count={item['count']} "
            f"expected={item['expected_checksum']}"
        )
    print()

    print(f"C-frame rows: {summary['c_frame_rows']}")
    print(f"Shape counts: {summary['shape_counts']}")
    print()

    print("Known detect fingerprints:")
    if not summary["detect_known"]:
        print("  none")
    for item in summary["detect_known"]:
        print(f"  {item['fingerprint']} -> {item['gun']} count={item['count']}")
    print()

    print("Unknown detect fingerprints:")
    if not summary["detect_unknown"]:
        print("  none")
    for item in summary["detect_unknown"]:
        print(f"  {item['fingerprint']} count={item['count']}")
        for example in item["examples"]:
            print(
                "    "
                f"{example['file']}:{example['line']} ms={example['millis']} "
                f"event={example['event']} gun={example['gun']} extra={example['extra']}"
            )
    print()

    print("Known deploy fingerprints:")
    if not summary["deploy_known"]:
        print("  none")
    for item in summary["deploy_known"]:
        print(f"  {item['fingerprint']} -> {item['gun']} count={item['count']}")
    print()

    print("Unknown deploy fingerprints:")
    if not summary["deploy_unknown"]:
        print("  none")
    for item in summary["deploy_unknown"]:
        print(f"  {item['fingerprint']} count={item['count']}")
        for example in item["examples"]:
            print(
                "    "
                f"{example['file']}:{example['line']} ms={example['millis']} "
                f"event={example['event']} gun={example['gun']} extra={example['extra']}"
            )
    print()

    print("Other C-frame shapes:")
    if not summary["other_shapes"]:
        print("  none")
    for item in summary["other_shapes"]:
        print(f"  {item['frame']} count={item['count']}")
        for example in item["examples"]:
            print(
                "    "
                f"{example['file']}:{example['line']} ms={example['millis']} "
                f"event={example['event']} gun={example['gun']} extra={example['extra']}"
            )


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs_dir", type=Path, help="Directory containing alp_*.csv logs")
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of text")
    parser.add_argument(
        "--example-limit",
        type=int,
        default=3,
        help="Maximum example rows stored per fingerprint candidate",
    )
    return parser.parse_args(list(argv))


def main(argv: Iterable[str]) -> int:
    args = parse_args(argv)
    summary = summarize(args.logs_dir, args.example_limit)
    if args.json:
        json.dump(summary, sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        print_text(summary)

    return 0 if not summary["warnings"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))