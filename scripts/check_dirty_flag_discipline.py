#!/usr/bin/env python3
"""Check display dirty flag discipline contract.

Enforced invariant:
    Every display_*.cpp file that reads a dirty flag field (dirty.xxx) must
    also clear it (dirty.xxx = false) in the same function body.

    If a flag is consumed without being cleared, two failure modes result:
    - Permanent redraw loop (flag stays true, every frame redraws)
    - Stale cache (flag cleared without local cache invalidation — a
      different bug, but this script catches the most dangerous case)

This script parses each file for dirty flag reads and clears, then verifies
that every flag read in a function also has a corresponding clear.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Set, Tuple

ROOT = Path(__file__).resolve().parents[1]
SRC_ROOT = ROOT / "src"

# Files that participate in the dirty flag protocol.
# display_update.cpp handles resetTracking specially (via element cache invalidation).
DISPLAY_FILES = sorted(SRC_ROOT.glob("display_*.cpp"))

# Match dirty flag field reads: dirty_.bands, dirty_.frequency, etc.
# Excludes assignments (dirty_.x = true/false) — those are sets/clears, not reads.
DIRTY_READ_RE = re.compile(r"\bdirty_\.(\w+)\b(?!\s*=)")

# Match dirty flag clears: dirty_.xxx = false
DIRTY_CLEAR_RE = re.compile(r"\bdirty_\.(\w+)\s*=\s*false\b")

# Match dirty flag sets: dirty_.xxx = true (not a read, not a clear)
DIRTY_SET_RE = re.compile(r"\bdirty_\.(\w+)\s*=\s*true\b")

# Rough function-body detector: find function definitions and their brace-delimited bodies.
# We track brace depth to associate reads/clears with the enclosing function.
FUNC_START_RE = re.compile(
    r"(?m)^[^#\n{};]*\b([A-Za-z_~][A-Za-z0-9_:~]*)\s*\([^;{}]*\)\s*(?:const\s*)?\{"
)

# Fields that are metadata or persistent mode flags, not per-frame dirty flags.
# resetTracking — consumed in display_update.cpp by the cache reset path.
# multiAlert — persistent mode flag (true while in multi-alert layout), not a
#   fire-once dirty signal. It is set/cleared by mode transitions, not by
#   individual render functions.
EXEMPT_FIELDS = {"resetTracking", "multiAlert"}

# Method names on the DisplayDirtyFlags struct that look like field reads
# to the regex but are actually method calls (e.g. dirty.setIndicatorFlags()).
EXEMPT_METHODS = {"setIndicatorFlags"}

# Orchestration files read dirty flags to route control flow but delegate
# the actual clear to the per-element render functions they call.
# For these files, we check at file scope (not function scope): every flag
# read anywhere in the file must be cleared somewhere in the file OR in
# its known delegate files.
ORCHESTRATION_FILES = {"display_update.cpp", "display_screens.cpp"}


@dataclass
class FunctionInfo:
    name: str
    file: Path
    start_line: int
    flags_read: Set[str] = field(default_factory=set)
    flags_cleared: Set[str] = field(default_factory=set)


def extract_functions(filepath: Path) -> List[FunctionInfo]:
    """Parse a C++ file and extract function bodies with their dirty flag usage."""
    text = filepath.read_text()
    lines = text.split("\n")
    functions: List[FunctionInfo] = []

    # Find all function definition start positions.
    func_starts: List[Tuple[int, str]] = []  # (char_offset, name)
    for m in FUNC_START_RE.finditer(text):
        name = m.group(1)
        char_offset = m.end() - 1  # Position of the opening brace
        func_starts.append((char_offset, name))

    for idx, (brace_pos, func_name) in enumerate(func_starts):
        # Walk from opening brace to find matching close.
        depth = 1
        pos = brace_pos + 1
        while pos < len(text) and depth > 0:
            ch = text[pos]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            elif ch == "/" and pos + 1 < len(text):
                # Skip comments.
                if text[pos + 1] == "/":
                    nl = text.find("\n", pos)
                    pos = nl if nl != -1 else len(text)
                    continue
                elif text[pos + 1] == "*":
                    end = text.find("*/", pos + 2)
                    pos = end + 1 if end != -1 else len(text)
                    continue
            elif ch == '"':
                # Skip string literals.
                pos += 1
                while pos < len(text) and text[pos] != '"':
                    if text[pos] == "\\":
                        pos += 1
                    pos += 1
            pos += 1

        body = text[brace_pos:pos]
        start_line = text[:brace_pos].count("\n") + 1

        info = FunctionInfo(
            name=func_name,
            file=filepath,
            start_line=start_line,
        )

        for m in DIRTY_READ_RE.finditer(body):
            flag = m.group(1)
            if flag not in EXEMPT_FIELDS and flag not in EXEMPT_METHODS:
                info.flags_read.add(flag)

        for m in DIRTY_CLEAR_RE.finditer(body):
            flag = m.group(1)
            info.flags_cleared.add(flag)

        # A flag clear like `dirty.x = false` also matches the read pattern,
        # so flags_read includes flags that are only cleared. Remove flags
        # that are *only* cleared (never read in a conditional context).
        # Actually, we want: if you read it, you must clear it. Clears without
        # reads are benign (defensive clears). Reads without clears are violations.

        if info.flags_read or info.flags_cleared:
            functions.append(info)

    return functions


def check_discipline() -> List[str]:
    """Return list of violation descriptions."""
    violations: List[str] = []

    if not DISPLAY_FILES:
        violations.append("ERROR: No display_*.cpp files found under src/")
        return violations

    # Collect all clears across all display files (for orchestration check).
    all_clears: Set[str] = set()
    file_functions: Dict[Path, List[FunctionInfo]] = {}

    for filepath in DISPLAY_FILES:
        functions = extract_functions(filepath)
        file_functions[filepath] = functions
        for func in functions:
            all_clears.update(func.flags_cleared)

    for filepath, functions in file_functions.items():
        is_orchestration = filepath.name in ORCHESTRATION_FILES

        if is_orchestration:
            # For orchestration files, check at file scope: every flag read
            # anywhere in the file must be cleared *somewhere* across all
            # display files (the orchestrator delegates to render functions).
            file_reads: Set[str] = set()
            file_clears: Set[str] = set()
            for func in functions:
                file_reads.update(func.flags_read)
                file_clears.update(func.flags_cleared)
            uncleared = file_reads - all_clears
            for flag in sorted(uncleared):
                violations.append(
                    f"{filepath.relative_to(ROOT)}: reads dirty.{flag} "
                    f"but no display file ever clears it"
                )
        else:
            # For render files, check per-function: every flag read must be
            # cleared in the same function.
            for func in functions:
                uncleared = func.flags_read - func.flags_cleared
                for flag in sorted(uncleared):
                    violations.append(
                        f"{filepath.relative_to(ROOT)}:{func.start_line} "
                        f"function '{func.name}' reads dirty.{flag} but never "
                        f"clears it (dirty.{flag} = false)"
                    )

    return violations


def main() -> int:
    violations = check_discipline()

    if violations:
        print("DIRTY FLAG DISCIPLINE CONTRACT VIOLATIONS:")
        print()
        for v in violations:
            print(f"  ✗ {v}")
        print()
        print(f"{len(violations)} violation(s) found.")
        return 1

    # Summary of what was checked.
    total_files = len(DISPLAY_FILES)
    print(f"Dirty flag discipline: {total_files} display files checked, all clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
