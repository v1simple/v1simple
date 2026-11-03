#!/usr/bin/env python3
"""Prove that the assembled deployment tree preserves the LittleFS reserve."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from build_context import BuildContext, read_storage_partition_size  # noqa: E402

try:
    from littlefs import LittleFS
    from littlefs.errors import LittleFSError
except ImportError as exc:
    print(f"  packed result:     FAIL (LittleFS packer unavailable: {exc})", file=sys.stderr)
    raise SystemExit(1) from exc


RESERVE_BYTES = 64 * 1024


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--context-json", required=True)
    parser.add_argument("--expected-raw-bytes", required=True, type=int)
    return parser.parse_args(argv)


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
        context = BuildContext.from_json(args.context_json)
        partition_bytes = read_storage_partition_size(Path(context.partitions))
        block_size = context.filesystem_block_size()
        name_max = context.filesystem_name_max()
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
