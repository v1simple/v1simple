#!/usr/bin/env python3
"""Verify that a release firmware binary embeds the selected semantic version."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SEMVER_RE = re.compile(r"^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$")
# setup() prints this complete string from FIRMWARE_VERSION. Check that record
# so an unrelated library version or web asset cannot satisfy the test.
BOOT_VERSION_RE = re.compile(rb"Firmware: ([^\x00]*)\x00")


def check_firmware_version(
    firmware: Path,
    expected: str,
    source: str | None = None,
) -> list[str]:
    if not SEMVER_RE.fullmatch(expected):
        return [f"invalid expected semantic version: {expected!r}"]
    if source is not None and not SEMVER_RE.fullmatch(source):
        return [f"invalid source semantic version: {source!r}"]
    try:
        image = firmware.read_bytes()
    except OSError as exc:
        return [f"could not read firmware image {firmware}: {exc}"]

    errors: list[str] = []
    boot_versions = BOOT_VERSION_RE.findall(image)
    if boot_versions != [expected.encode("ascii")]:
        errors.append(
            f"firmware must contain exactly one boot version record for selected version {expected!r}"
        )
    if source is not None and source != expected and source.encode("ascii") in boot_versions:
        errors.append(f"firmware still reports source version {source!r}")
    return errors


def require_release_firmware_version(firmware: Path, tag: str) -> None:
    if not tag.startswith("v") or not SEMVER_RE.fullmatch(tag[1:]):
        raise ValueError(f"invalid release tag {tag!r}; expected vMAJOR.MINOR.PATCH")
    errors = check_firmware_version(firmware, tag[1:])
    if errors:
        raise ValueError("; ".join(errors))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--expected", required=True)
    parser.add_argument("--source")
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
