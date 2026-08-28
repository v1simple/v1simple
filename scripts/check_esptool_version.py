#!/usr/bin/env python3
"""Fail early unless esptool matches the release image-builder pin."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys

REQUIRED_VERSION = (5, 3, 0)


def parse_version(raw: str) -> tuple[int, ...] | None:
    match = re.search(
        r"(?<![0-9A-Za-z])(\d+)\.(\d+)\.(\d+)(?![0-9A-Za-z.+-])",
        raw,
    )
    if match is None:
        return None
    return tuple(int(part) for part in match.groups())


def format_version(version: tuple[int, ...]) -> str:
    return ".".join(str(part) for part in version)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--python",
        default=sys.executable,
        help="Python interpreter whose esptool module will build the merged image",
    )
    parser.add_argument(
        "--required-version",
        default=format_version(REQUIRED_VERSION),
        help="Exact required esptool version",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    required_version = parse_version(args.required_version)
    if required_version is None:
        print(f"[toolchain] invalid required esptool version: {args.required_version}", file=sys.stderr)
        return 2

    try:
        proc = subprocess.run(
            [args.python, "-m", "esptool", "version"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except OSError as exc:
        print(f"[toolchain] failed to execute {args.python!r}: {exc}", file=sys.stderr)
        return 1

    output = proc.stdout.strip()
    version = parse_version(output)
    if proc.returncode != 0 or version is None:
        print(f"[toolchain] unable to parse esptool version from: {output}", file=sys.stderr)
        return 1

    if version == required_version:
        print(f"[toolchain] esptool {format_version(version)} matches the release pin")
        return 0

    print(
        f"[toolchain] esptool {format_version(version)} does not match the required "
        f"release pin {format_version(required_version)}.",
        file=sys.stderr,
    )
    print(
        f"[toolchain] Install esptool=={format_version(required_version)} before building release images.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
