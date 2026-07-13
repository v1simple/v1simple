#!/usr/bin/env python3
"""Validate the firmware env enforces member initializer reorder warnings."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENV_NAME = "waveshare-349"
REQUIRED_FLAG = "-Werror=reorder"


def run_envdump() -> str:
    result = subprocess.run(
        ["pio", "run", "-e", ENV_NAME, "-t", "envdump"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    return result.stdout


def extract_flags(text: str, key: str) -> list[str]:
    match = re.search(rf"'{re.escape(key)}': \[(.*?)\]\s*,", text, re.DOTALL)
    if not match:
        raise RuntimeError(f"Missing {key} in PlatformIO envdump output")
    return re.findall(r"'([^']*)'", match.group(1))


def main() -> int:
    try:
        output = run_envdump()
        cxxflags = extract_flags(output, "CXXFLAGS")
        ccflags = extract_flags(output, "CCFLAGS")
    except Exception as exc:
        print(f"[contract] reorder-warning: {exc}")
        return 1

    errors: list[str] = []
    if REQUIRED_FLAG not in cxxflags:
        errors.append(f"{ENV_NAME} CXXFLAGS missing {REQUIRED_FLAG}")
    if REQUIRED_FLAG in ccflags:
        errors.append(f"{ENV_NAME} CCFLAGS unexpectedly include {REQUIRED_FLAG}")

    if errors:
        print("[contract] reorder-warning mismatch:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(
        "[contract] reorder-warning enforced in active firmware flags "
        f"for {ENV_NAME}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
