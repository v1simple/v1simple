#!/usr/bin/env python3
"""Verify that a release firmware binary embeds the selected semantic version."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SEMVER_RE = re.compile(r"^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$")


def check_firmware_version(
    firmware: Path,
    expected: str,
    source: str,
) -> list[str]:
    errors: list[str] = []
    if not SEMVER_RE.fullmatch(expected):
        return [f"invalid expected semantic version: {expected!r}"]
    if not SEMVER_RE.fullmatch(source):
        return [f"invalid source semantic version: {source!r}"]
    try:
        image = firmware.read_bytes()
    except OSError as exc:
        return [f"could not read firmware image {firmware}: {exc}"]

    expected_literal = expected.encode("ascii") + b"\0"
    if expected_literal not in image:
        errors.append(f"firmware does not contain selected version {expected!r}")
    if source != expected and source.encode("ascii") + b"\0" in image:
        errors.append(
            f"firmware still contains source version {source!r} instead of only the selected version"
        )
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--expected", required=True)
    parser.add_argument("--source", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    errors = check_firmware_version(args.firmware, args.expected, args.source)
    if errors:
        print("[release-version] firmware version validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(f"[release-version] firmware embeds {args.expected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
