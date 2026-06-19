#!/usr/bin/env python3
"""Validate `build.sh --dist` always builds and verifies packaged artifacts."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_SH = ROOT / "build.sh"


def extract_dist_block(text: str) -> str:
    start = text.find('if [ "$DIST_BUILD" = true ]; then')
    if start < 0:
        raise ValueError("missing DIST_BUILD staging block")

    marker = '\n# Tests (requires gcc/g++ on host)'
    end = text.find(marker, start)
    if end < 0:
        raise ValueError("missing tests block after DIST_BUILD staging block")

    return text[start:end]


def main() -> int:
    text = BUILD_SH.read_text(encoding="utf-8")
    errors: list[str] = []

    try:
        dist_block = extract_dist_block(text)
    except ValueError as exc:
        errors.append(str(exc))
        dist_block = ""

    if "-t buildfs" not in dist_block:
        errors.append("--dist must run a LittleFS buildfs step before staging")

    if "stage_dist_artifact" not in dist_block:
        errors.append("--dist must use fail-on-missing artifact staging")

    expected_sources = (
        'FW_SRC=".pio/build/${PIO_ENV}/firmware.bin"',
        'FS_SRC=".pio/build/${PIO_ENV}/littlefs.bin"',
    )
    for source in expected_sources:
        if source not in dist_block:
            errors.append(f"--dist missing expected source declaration: {source}")

    required_helper_checks = (
        r'stage_dist_artifact\(\).*?\[\s*!\s*-f\s+"\$src"\s*\].*?exit 1',
        r'stage_dist_artifact\(\).*?file_size_bytes "\$dest"',
        r'stage_dist_artifact\(\).*?file_sha256 "\$dest"',
    )
    for pattern in required_helper_checks:
        if not re.search(pattern, text, re.DOTALL):
            errors.append(f"stage_dist_artifact helper missing required behavior: {pattern}")

    upload_help_misstatement = re.compile(
        r"--upload(?:-fs)?\s+.*runs tests first", re.IGNORECASE
    )
    if upload_help_misstatement.search(text):
        errors.append("build.sh help must not claim uploads run tests unless --test is set")

    if errors:
        print("[contract] build.sh --dist contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] build.sh --dist builds LittleFS and verifies staged artifacts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
