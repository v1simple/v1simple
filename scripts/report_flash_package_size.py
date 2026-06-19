#!/usr/bin/env python3
"""Report flash package size truth for firmware artifacts.

Prints firmware, filesystem, bootloader, partition, total package bytes,
and remaining gap against a reduction target from a known baseline.

Exit status is non-zero only when an explicit budget/expectation is passed
and violated.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import sys


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", default=".pio/build/waveshare-349")
    p.add_argument("--partition-table", default="partitions_v1.csv")
    p.add_argument("--baseline-total-bytes", type=int, default=6_361_920)
    p.add_argument("--target-reduction-pct", type=float, default=25.0)
    p.add_argument("--max-total-package-bytes", type=int)
    p.add_argument("--max-firmware-bytes", type=int)
    p.add_argument("--expect-littlefs-bytes", type=int)
    return p.parse_args()


def human(n: int) -> str:
    mib = n / (1024 * 1024)
    return f"{n} bytes ({mib:.2f} MiB)"


def read_storage_partition_size(path: Path) -> int:
    with path.open(newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row or row[0].strip().startswith("#"):
                continue
            if row[0].strip() == "storage":
                return int(row[4].strip(), 0)
    raise RuntimeError("storage partition not found in partition table")


def stat_size(path: Path) -> int:
    if not path.exists():
        raise FileNotFoundError(path)
    return path.stat().st_size


def main() -> int:
    args = parse_args()
    build_dir = Path(args.build_dir)
    partition_table = Path(args.partition_table)

    firmware = stat_size(build_dir / "firmware.bin")
    littlefs = stat_size(build_dir / "littlefs.bin")
    bootloader = stat_size(build_dir / "bootloader.bin")
    partitions = stat_size(build_dir / "partitions.bin")
    storage_partition = read_storage_partition_size(partition_table)

    total = firmware + littlefs + bootloader + partitions
    target_total = int(round(args.baseline_total_bytes * (1.0 - args.target_reduction_pct / 100.0)))
    saved_vs_baseline = args.baseline_total_bytes - total
    remaining_gap = max(0, total - target_total)

    print("Flash package report")
    print(f"  firmware.bin:   {human(firmware)}")
    print(f"  littlefs.bin:   {human(littlefs)}")
    print(f"  bootloader.bin: {human(bootloader)}")
    print(f"  partitions.bin: {human(partitions)}")
    print(f"  storage part:   {human(storage_partition)}")
    print(f"  total package:  {human(total)}")
    print(f"  baseline total: {human(args.baseline_total_bytes)}")
    print(f"  target total:   {human(target_total)} ({args.target_reduction_pct:.1f}% reduction)")
    print(f"  saved so far:   {human(saved_vs_baseline)}")
    print(f"  remaining gap:  {human(remaining_gap)}")

    failed = False

    if littlefs > storage_partition:
        print("ERROR: littlefs.bin exceeds storage partition size", file=sys.stderr)
        failed = True

    if args.expect_littlefs_bytes is not None and littlefs != args.expect_littlefs_bytes:
        print(
            f"ERROR: littlefs.bin size {littlefs} != expected {args.expect_littlefs_bytes}",
            file=sys.stderr,
        )
        failed = True

    if args.max_firmware_bytes is not None and firmware > args.max_firmware_bytes:
        print(
            f"ERROR: firmware.bin size {firmware} exceeds budget {args.max_firmware_bytes}",
            file=sys.stderr,
        )
        failed = True

    if args.max_total_package_bytes is not None and total > args.max_total_package_bytes:
        print(
            f"ERROR: total package size {total} exceeds budget {args.max_total_package_bytes}",
            file=sys.stderr,
        )
        failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
