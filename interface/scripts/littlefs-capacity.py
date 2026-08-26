#!/usr/bin/env python3
"""Prove that the assembled deployment tree preserves the LittleFS reserve."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import sys
from typing import Sequence

try:
    from littlefs import LittleFS
    from littlefs.errors import LittleFSError
except ImportError as exc:
    print(f"  packed result:     FAIL (LittleFS packer unavailable: {exc})", file=sys.stderr)
    raise SystemExit(1) from exc


RESERVE_BYTES = 64 * 1024
ENVIRONMENT = "env:waveshare-349"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--partition-table", required=True, type=Path)
    parser.add_argument("--platformio-core-dir", required=True, type=Path)
    parser.add_argument("--expected-raw-bytes", required=True, type=int)
    return parser.parse_args(argv)


def read_storage_partition_size(path: Path) -> int:
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row or row[0].strip().startswith("#"):
                continue
            if row[0].strip() == "storage":
                return int(row[4].strip(), 0)
    raise RuntimeError(f"storage partition not found in {path}")


def read_environment_setting(path: Path, setting: str) -> str | None:
    section: str | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
            continue
        if section == ENVIRONMENT and stripped.startswith(f"{setting} ="):
            return stripped.split("=", 1)[1].strip()
    return None


def read_packer_block_size(core_dir: Path) -> int:
    builder = core_dir / "platforms" / "espressif32" / "builder" / "main.py"
    source = builder.read_text(encoding="utf-8")
    match = re.search(r'env\["FS_BLOCK"\]\s*=\s*int\("([^"]+)",\s*16\)', source)
    if not match:
        raise RuntimeError(f"could not determine PlatformIO LittleFS block size from {builder}")
    return int(match.group(1), 16)


def read_runtime_name_max(project_root: Path, core_dir: Path) -> int:
    memory_type = read_environment_setting(
        project_root / "platformio.ini", "board_build.arduino.memory_type"
    )
    if not memory_type:
        raise RuntimeError("could not determine waveshare-349 Arduino memory type")

    sdkconfig = (
        core_dir
        / "packages"
        / "framework-arduinoespressif32-libs"
        / "esp32s3"
        / memory_type
        / "include"
        / "sdkconfig.h"
    )
    match = re.search(
        r"#define\s+CONFIG_LITTLEFS_OBJ_NAME_LEN\s+(\d+)",
        sdkconfig.read_text(encoding="utf-8"),
    )
    if not match:
        raise RuntimeError(f"could not determine runtime LittleFS name limit from {sdkconfig}")
    return int(match.group(1))


def collect_tree(root: Path) -> tuple[list[Path], list[Path], int]:
    if not root.is_dir():
        raise RuntimeError(f"deployed data directory not found: {root}")

    entries = sorted(root.rglob("*"), key=lambda path: path.relative_to(root).as_posix())
    unsupported = [path for path in entries if path.is_symlink() or not (path.is_dir() or path.is_file())]
    if unsupported:
        raise RuntimeError(f"unsupported deployed entry: {unsupported[0]}")

    directories = sorted(
        (path for path in entries if path.is_dir()),
        key=lambda path: (len(path.relative_to(root).parts), path.relative_to(root).as_posix()),
    )
    files = [path for path in entries if path.is_file()]
    raw_bytes = sum(path.stat().st_size for path in files)
    return directories, files, raw_bytes


def pack_tree(
    root: Path,
    directories: list[Path],
    files: list[Path],
    *,
    block_size: int,
    block_count: int,
    name_max: int,
) -> bool:
    try:
        filesystem = LittleFS(
            block_size=block_size,
            block_count=block_count,
            read_size=1,
            prog_size=1,
            cache_size=block_size,
            lookahead_size=32,
            block_cycles=500,
            name_max=name_max,
            disk_version=(2 << 16) | 1,
            mount=False,
        )
        filesystem.format()
        filesystem.mount()

        for directory in directories:
            relative = directory.relative_to(root).as_posix()
            filesystem.makedirs(relative, exist_ok=True)
            filesystem.setattr(relative, "t", b"\0\0\0\0")

        for source in files:
            relative = source.relative_to(root).as_posix()
            with filesystem.open(relative, "wb") as destination, source.open("rb") as handle:
                while chunk := handle.read(64 * 1024):
                    destination.write(chunk)
            filesystem.setattr(relative, "t", b"\0\0\0\0")

        filesystem.unmount()
        return True
    except LittleFSError:
        return False


def minimum_packed_blocks(
    root: Path,
    directories: list[Path],
    files: list[Path],
    *,
    block_size: int,
    maximum_blocks: int,
    name_max: int,
) -> int | None:
    if not pack_tree(
        root,
        directories,
        files,
        block_size=block_size,
        block_count=maximum_blocks,
        name_max=name_max,
    ):
        return None

    low = 2
    high = maximum_blocks
    while low < high:
        candidate = (low + high) // 2
        if pack_tree(
            root,
            directories,
            files,
            block_size=block_size,
            block_count=candidate,
            name_max=name_max,
        ):
            high = candidate
        else:
            low = candidate + 1
    return low


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        project_root = args.partition_table.resolve().parent
        partition_bytes = read_storage_partition_size(args.partition_table)
        block_size = read_packer_block_size(args.platformio_core_dir)
        name_max = read_runtime_name_max(project_root, args.platformio_core_dir)
        directories, files, raw_bytes = collect_tree(args.data_dir)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"  packed result:     FAIL ({exc})", file=sys.stderr)
        return 1

    safety_limit = partition_bytes - RESERVE_BYTES
    print(f"  partition size:    {partition_bytes} bytes")
    print(f"  safety limit:      {safety_limit} bytes (partition minus {RESERVE_BYTES}-byte reserve)")
    print(f"  raw deployed:      {raw_bytes} bytes across {len(files)} files")
    print(f"  block/name limit:  {block_size} bytes/{name_max} bytes")

    if raw_bytes != args.expected_raw_bytes:
        print(
            f"  packed result:     FAIL (deployed tree changed while checking: "
            f"expected {args.expected_raw_bytes} raw bytes, found {raw_bytes})",
            file=sys.stderr,
        )
        return 1
    if partition_bytes <= RESERVE_BYTES or safety_limit % block_size != 0:
        print(
            "  packed result:     FAIL (partition minus reserve is not a positive whole number of blocks)",
            file=sys.stderr,
        )
        return 1

    longest_name = max(
        (len(path.name.encode("utf-8")) for path in [*directories, *files]), default=0
    )
    if longest_name > name_max:
        print(
            f"  packed result:     FAIL (deployed filename requires {longest_name} bytes; "
            f"runtime limit is {name_max})",
            file=sys.stderr,
        )
        return 1

    maximum_blocks = safety_limit // block_size
    try:
        packed_blocks = minimum_packed_blocks(
            args.data_dir,
            directories,
            files,
            block_size=block_size,
            maximum_blocks=maximum_blocks,
            name_max=name_max,
        )
    except Exception as exc:  # Fail closed on packer or deployed-tree I/O errors.
        print(f"  packed result:     FAIL (LittleFS packing error: {exc})", file=sys.stderr)
        return 1
    if packed_blocks is None:
        print(
            f"  packed result:     FAIL (tree cannot be packed into the {safety_limit}-byte safety limit)",
            file=sys.stderr,
        )
        return 1

    packed_bytes = packed_blocks * block_size
    margin = safety_limit - packed_bytes
    print(f"  packed result:     {packed_bytes} bytes ({packed_blocks} blocks)")
    print(
        f"  result:            PASS (packed tree is {margin} bytes below the safety limit; "
        f"{RESERVE_BYTES}-byte reserve preserved)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
