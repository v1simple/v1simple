#!/usr/bin/env python3
"""Enforce coordinator-only runtime mute/volume writes.

Quiet-system phase 2 contract:
  - production source may not call V1BLEClient setMute()/setVolume() directly
    outside QuietCoordinatorModule.
"""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC_ROOT = ROOT / "src"
ALLOWED_FILES = {
    "src/modules/quiet/quiet_coordinator_module.cpp",
}
CALL_RE = re.compile(r"(?:->|\.)set(?:Mute|Volume)\(")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}


def iter_source_files() -> list[Path]:
    files: list[Path] = []
    for path in SRC_ROOT.rglob("*"):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            files.append(path)
    return sorted(files)


def find_violations() -> list[str]:
    violations: list[str] = []
    for path in iter_source_files():
        relative = path.relative_to(ROOT).as_posix()
        if relative in ALLOWED_FILES:
            continue
        for line_no, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if CALL_RE.search(raw_line):
                violations.append(f"{relative}:{line_no}: {raw_line.strip()}")
    return violations


def main() -> int:
    violations = find_violations()
    if violations:
        print("[contract] quiet coordinator contract violated.")
        print("Direct runtime setMute()/setVolume() calls must stay inside QuietCoordinatorModule.")
        for row in violations:
            print(f"  - {row}")
        return 1

    print("[contract] quiet coordinator contract matches (0 direct mute/volume writes outside coordinator)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
