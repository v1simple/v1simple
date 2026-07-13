#!/usr/bin/env python3
"""Enforce frontend HTTP resilience invariants.

Contracts:
1) All HTTP requests must use fetchWithTimeout (no raw fetch calls in app code).
2) Polling loops must use createPoll (no ad-hoc setInterval network polls).

Allowed exceptions:
- interface/src/lib/utils/poll.js: defines fetchWithTimeout/createPoll internals.
- interface/src/routes/settings/+page.svelte: one local UI clock tick interval.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FRONTEND_SRC = ROOT / "interface" / "src"

ALLOWED_FETCH_FILE = "interface/src/lib/utils/poll.js"
ALLOWED_SETINTERVAL_FILE = "interface/src/lib/utils/poll.js"
ALLOWED_SETTINGS_TICK_FILE = "interface/src/routes/settings/+page.svelte"
ALLOWED_SETTINGS_TICK_SNIPPET = "timeTickInterval = setInterval("

SOURCE_SUFFIXES = {".js", ".ts", ".svelte"}
FETCH_RE = re.compile(r"\bfetch\s*\(")
SET_INTERVAL_RE = re.compile(r"\bsetInterval\s*\(")


def iter_source_files() -> list[Path]:
    files: list[Path] = []
    for path in FRONTEND_SRC.rglob("*"):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            files.append(path)
    return sorted(files)


def main() -> int:
    fetch_violations: list[str] = []
    interval_violations: list[str] = []

    for path in iter_source_files():
        rel = path.relative_to(ROOT).as_posix()
        lines = path.read_text(encoding="utf-8").splitlines()

        for line_no, line in enumerate(lines, start=1):
            if FETCH_RE.search(line):
                if rel != ALLOWED_FETCH_FILE:
                    fetch_violations.append(f"{rel}:{line_no}: {line.strip()}")

            if SET_INTERVAL_RE.search(line):
                allowed_tick = (
                    rel == ALLOWED_SETTINGS_TICK_FILE
                    and ALLOWED_SETTINGS_TICK_SNIPPET in line
                )
                if rel != ALLOWED_SETINTERVAL_FILE and not allowed_tick:
                    interval_violations.append(f"{rel}:{line_no}: {line.strip()}")

    if fetch_violations:
        print("[contract] Raw fetch() calls detected outside poll utility:")
        for row in fetch_violations:
            print(f"  - {row}")

    if interval_violations:
        print("[contract] setInterval() polling detected outside createPoll utility:")
        for row in interval_violations:
            print(f"  - {row}")

    if fetch_violations or interval_violations:
        print("\nFrontend HTTP resilience contract FAILED.")
        return 1

    print("Frontend HTTP resilience contract OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
