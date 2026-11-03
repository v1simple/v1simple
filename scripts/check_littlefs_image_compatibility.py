#!/usr/bin/env python3
"""Gate the LittleFS image against the firmware runtime configuration.

The approved image path is PlatformIO/pioarduino `buildfs`, which currently
uses littlefs-python. This script verifies the produced image size and checks
that the image can be listed using the firmware runtime's LittleFS name limit.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Iterable, Sequence

from build_context import read_storage_partition_size, resolve


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BLOCK_SIZE = 4096
DEFAULT_PAGE_SIZE = 256


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--data-dir", default=str(ROOT / "data"))
    p.add_argument("--partition-table")
    p.add_argument("--pio", help="Selected PlatformIO executable; never fall back after failure")
    p.add_argument("--env", default=os.environ.get("PIO_ENV", "waveshare-349"))
    p.add_argument("--candidate", help="Existing LittleFS image to verify")
    p.add_argument("--block-size", type=int)
    p.add_argument("--page-size", type=int, default=DEFAULT_PAGE_SIZE)
    p.add_argument(
        "--storage-bytes",
        type=lambda v: int(v, 0),
        help="Override storage partition size; defaults to partitions_v1.csv storage size",
    )
    p.add_argument(
        "--runtime-name-max",
        type=int,
        help="Override firmware LittleFS runtime name limit",
    )
    p.add_argument(
        "--littlefs-python",
        help="Explicit littlefs-python CLI; otherwise use the selected interpreter or PATH in standalone mode",
    )
    return p.parse_args(argv)


def run(cmd: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=check, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def all_files(root: Path) -> list[Path]:
    if not root.exists():
        raise FileNotFoundError(root)
    return sorted(p for p in root.rglob("*") if p.is_file())


def data_name_stats(root: Path) -> tuple[int, int, Path | None, Path | None]:
    files = all_files(root)
    if not files:
        return (0, 0, None, None)
    max_name_path = max(files, key=lambda p: len(p.name))
    max_path_path = max(files, key=lambda p: len("/" + p.relative_to(root).as_posix()))
    return (
        len(max_name_path.name),
        len("/" + max_path_path.relative_to(root).as_posix()),
        max_name_path,
        max_path_path,
    )


def find_littlefs_python(explicit: str | None) -> str:
    candidate = shutil.which(explicit or "littlefs-python")
    if not candidate:
        raise FileNotFoundError(f"littlefs-python CLI not found: {explicit or 'PATH'}")
    return candidate


def list_with_littlefs_python(
    littlefs_python: str,
    image: Path,
    block_size: int,
    name_max: int,
) -> subprocess.CompletedProcess[str]:
    return run(
        [
            littlefs_python,
            "list",
            "--block-size",
            str(block_size),
            "--name-max",
            str(name_max),
            str(image),
        ],
        check=False,
    )


def print_command_output(prefix: str, lines: Iterable[str], limit: int = 12) -> None:
    emitted = 0
    for line in lines:
        if emitted >= limit:
            print(f"{prefix}...")
            return
        print(f"{prefix}{line}")
        emitted += 1


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    data_dir = Path(args.data_dir)
    try:
        context = None
        if args.pio or not args.runtime_name_max:
            context = resolve(ROOT, args.env, args.pio or os.environ.get("PIO_CMD", "pio"))
        partition_table = Path(args.partition_table or (context.partitions if context else ROOT / "partitions_v1.csv"))
        storage_bytes = args.storage_bytes or read_storage_partition_size(partition_table)
        runtime_name_max = args.runtime_name_max or context.filesystem_name_max()
        block_size = args.block_size or (context.filesystem_block_size() if context else DEFAULT_BLOCK_SIZE)
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    try:
        max_name, max_path, max_name_path, max_path_path = data_name_stats(data_dir)
    except FileNotFoundError as exc:
        print(f"ERROR: data directory not found: {exc}", file=sys.stderr)
        return 1
    print("LittleFS compatibility check")
    print(f"  data dir:          {data_dir}")
    print(f"  storage bytes:     {storage_bytes}")
    print(f"  block/page size:   {block_size}/{args.page_size}")
    print(f"  runtime name max:  {runtime_name_max}")
    print(f"  max data basename: {max_name} ({max_name_path})")
    print(f"  max data path:     {max_path} ({max_path_path})")

    failed = False
    if max_name > runtime_name_max:
        print(f"ERROR: data contains basename longer than runtime name max ({max_name} > {runtime_name_max})")
        failed = True

    candidate = Path(args.candidate) if args.candidate else None
    if candidate:
        if not candidate.exists():
            print(f"ERROR: candidate image not found: {candidate}", file=sys.stderr)
            return 1
        size = candidate.stat().st_size
        print(f"  candidate image:   {candidate}")
        print(f"  candidate size:    {size}")
        if size != storage_bytes:
            print(f"ERROR: candidate size {size} != storage partition size {storage_bytes}")
            failed = True
        try:
            selected_cli = args.littlefs_python or (str(Path(context.tools.python).parent / "littlefs-python") if context else None)
            littlefs_python = find_littlefs_python(selected_cli)
        except FileNotFoundError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
        result = list_with_littlefs_python(littlefs_python, candidate, block_size, runtime_name_max)
        if result.returncode == 0:
            print("  runtime-style mount/list: PASS")
            print_command_output("    ", result.stdout.splitlines())
        else:
            print("ERROR: runtime-style mount/list failed")
            print_command_output("    stdout: ", result.stdout.splitlines())
            print_command_output("    stderr: ", result.stderr.splitlines())
            failed = True

    if failed:
        print("LittleFS compatibility check: FAIL")
        return 1

    print("LittleFS compatibility check: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
