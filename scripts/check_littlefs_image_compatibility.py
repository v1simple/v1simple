#!/usr/bin/env python3
"""Gate the LittleFS image against the firmware runtime configuration.

The approved image path is PlatformIO/pioarduino `buildfs`, which currently
uses littlefs-python. This script verifies the produced image size and checks
that the image can be listed using the firmware runtime's LittleFS name limit.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Iterable, Sequence


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BLOCK_SIZE = 4096
DEFAULT_PAGE_SIZE = 256


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--data-dir", default=str(ROOT / "data"))
    p.add_argument("--partition-table", default=str(ROOT / "partitions_v1.csv"))
    p.add_argument("--candidate", help="Existing LittleFS image to verify")
    p.add_argument("--block-size", type=int, default=DEFAULT_BLOCK_SIZE)
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
        help="littlefs-python CLI path; defaults to PATH or ~/.platformio/penv/bin/littlefs-python",
    )
    return p.parse_args(argv)


def run(cmd: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=check, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def read_storage_partition_size(path: Path) -> int:
    if not path.exists():
        raise FileNotFoundError(f"partition table not found: {path}")
    with path.open(newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row or row[0].strip().startswith("#"):
                continue
            if row[0].strip() == "storage":
                return int(row[4].strip(), 0)
    raise RuntimeError(f"storage partition not found in {path}")


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


def configured_memory_type() -> str | None:
    ini = ROOT / "platformio.ini"
    if not ini.exists():
        return None
    env = None
    for line in ini.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            env = stripped[1:-1]
            continue
        if env == "env:waveshare-349" and stripped.startswith("board_build.arduino.memory_type"):
            _, value = stripped.split("=", 1)
            return value.strip()
    return None


def framework_runtime_name_max() -> int | None:
    memory_type = configured_memory_type()
    candidates: list[Path] = []
    home = Path.home()
    if memory_type:
        candidates.append(
            home
            / ".platformio/packages/framework-arduinoespressif32-libs/esp32s3"
            / memory_type
            / "include/sdkconfig.h"
        )
    candidates.append(home / ".platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig")

    patterns = [
        re.compile(r"#define\s+CONFIG_LITTLEFS_OBJ_NAME_LEN\s+(\d+)"),
        re.compile(r"CONFIG_LITTLEFS_OBJ_NAME_LEN=(\d+)"),
    ]
    for path in candidates:
        if not path.exists():
            continue
        text = path.read_text(errors="ignore")
        for pattern in patterns:
            m = pattern.search(text)
            if m:
                return int(m.group(1))
    return None


def find_littlefs_python(explicit: str | None) -> str:
    candidates: list[str] = []
    if explicit:
        candidates.append(explicit)
    found = shutil.which("littlefs-python")
    if found:
        candidates.append(found)
    candidates.append(str(Path.home() / ".platformio" / "penv" / "bin" / "littlefs-python"))
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    raise FileNotFoundError("littlefs-python CLI not found")


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
    partition_table = Path(args.partition_table)
    try:
        storage_bytes = args.storage_bytes or read_storage_partition_size(partition_table)
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    runtime_name_max = args.runtime_name_max or framework_runtime_name_max()
    if runtime_name_max is None:
        print("ERROR: could not determine firmware CONFIG_LITTLEFS_OBJ_NAME_LEN", file=sys.stderr)
        return 1

    try:
        max_name, max_path, max_name_path, max_path_path = data_name_stats(data_dir)
    except FileNotFoundError as exc:
        print(f"ERROR: data directory not found: {exc}", file=sys.stderr)
        return 1
    print("LittleFS compatibility check")
    print(f"  data dir:          {data_dir}")
    print(f"  storage bytes:     {storage_bytes}")
    print(f"  block/page size:   {args.block_size}/{args.page_size}")
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
            littlefs_python = find_littlefs_python(args.littlefs_python)
        except FileNotFoundError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
        result = list_with_littlefs_python(littlefs_python, candidate, args.block_size, runtime_name_max)
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
