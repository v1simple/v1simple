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
# Whitespace-tolerant so clang-format cannot hide a call by wrapping the member
# access (e.g. `audio\n    ->setMute(`) or the argument list.
CALL_RE = re.compile(r"(?:->|\.)\s*set(?:Mute|Volume)\s*\(")
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
        text = path.read_text(encoding="utf-8")
        # Scan the whole file rather than line-by-line: a line-scoped search cannot
        # see a call that clang-format has wrapped across a newline.
        lines = text.splitlines()
        for match in CALL_RE.finditer(text):
            line_no = text.count("\n", 0, match.start()) + 1
            raw_line = lines[line_no - 1] if line_no - 1 < len(lines) else ""
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
