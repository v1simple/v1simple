#!/usr/bin/env python3
"""Check that module headers don't use extern declarations (violates DI architecture)."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULES_DIR = ROOT / "src" / "modules"

# Match lines with extern keyword, excluding:
# - extern "C" linkage specifications (legitimate)
# - comments
# - string literals
# - #ifdef UNIT_TEST blocks

EXTERN_RE = re.compile(r'^\s*extern\s+(?!"C")')


def mask_comments_and_strings(source: str) -> str:
    """Replace comments and string literals with spaces to avoid false matches."""
    result = []
    i = 0
    while i < len(source):
        # Handle block comments
        if i < len(source) - 1 and source[i : i + 2] == "/*":
            end = source.find("*/", i + 2)
            if end != -1:
                result.append(" " * (end + 2 - i))
                i = end + 2
                continue
        # Handle line comments
        if i < len(source) - 1 and source[i : i + 2] == "//":
            newline = source.find("\n", i)
            if newline != -1:
                result.append(" " * (newline - i) + "\n")
                i = newline + 1
            else:
                result.append(" " * (len(source) - i))
                i = len(source)
            continue
        # Handle string literals (double quotes)
        if source[i] == '"':
            result.append(" ")
            i += 1
            while i < len(source):
                if source[i] == "\\":
                    result.append("  ")
                    i += 2
                elif source[i] == '"':
                    result.append(" ")
                    i += 1
                    break
                else:
                    result.append(" ")
                    i += 1
            continue
        # Handle character literals (single quotes)
        if source[i] == "'":
            result.append(" ")
            i += 1
            while i < len(source):
                if source[i] == "\\":
                    result.append("  ")
                    i += 2
                elif source[i] == "'":
                    result.append(" ")
                    i += 1
                    break
                else:
                    result.append(" ")
                    i += 1
            continue
        result.append(source[i])
        i += 1
    return "".join(result)


def is_inside_ifdef_unit_test(source: str, line_no: int) -> bool:
    """Check if a line is inside #ifdef UNIT_TEST ... #endif block."""
    lines = source.split("\n")
    ifdef_depth = 0
    in_unit_test = False

    for i in range(min(line_no - 1, len(lines))):
        line = lines[i].strip()
        if line.startswith("#ifdef"):
            if "UNIT_TEST" in line:
                in_unit_test = True
                ifdef_depth += 1
            else:
                ifdef_depth += 1
        elif line.startswith("#else"):
            if ifdef_depth > 0 and in_unit_test:
                in_unit_test = False
        elif line.startswith("#endif"):
            if ifdef_depth > 0:
                ifdef_depth -= 1
                if ifdef_depth == 0:
                    in_unit_test = False

    return in_unit_test


def scan_module_headers() -> list[str]:
    """Scan all .h files under src/modules/ for extern declarations."""
    violations: list[str] = []

    if not MODULES_DIR.exists():
        return violations

    for header_path in sorted(MODULES_DIR.rglob("*.h")):
        relative = header_path.relative_to(ROOT).as_posix()
        source = header_path.read_text(encoding="utf-8", errors="replace")
        masked = mask_comments_and_strings(source)

        lines = source.split("\n")
        masked_lines = masked.split("\n")

        for line_no, (raw_line, masked_line) in enumerate(zip(lines, masked_lines), start=1):
            # Check masked line to avoid matches in comments/strings
            if EXTERN_RE.search(masked_line):
                # Check if it's extern "C" (which is allowed)
                if 'extern "C"' in raw_line or "extern \"C\"" in raw_line:
                    continue

                # Check if inside UNIT_TEST block
                if is_inside_ifdef_unit_test(source, line_no):
                    continue

                # This is an extern declaration violation
                violations.append(f"file={relative} line={line_no}")

    return violations


def main() -> int:
    violations = scan_module_headers()

    if violations:
        print("[guard] extern-escape violations detected")
        for row in violations:
            print(f"  - {row}")
        return 1

    print("[guard] extern-escape guard matches (0 extern declarations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
