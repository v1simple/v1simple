#!/usr/bin/env python3
"""Scan firmware source for known bug patterns.

Every class of bug found by manual review gets a detector here.
Run as part of CI to catch regressions and track improvement.

Exit 0  → no violations (or --report mode)
Exit 1  → violations found

Usage:
    python3 scripts/check_bug_patterns.py           # CI gate (fails on violations)
    python3 scripts/check_bug_patterns.py --report   # print scorecard only
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

# ---------------------------------------------------------------------------
# Pattern definitions
# ---------------------------------------------------------------------------
# Each pattern has:
#   id        — short stable identifier
#   title     — human-readable name
#   why       — what bug this catches
#   regex     — compiled pattern to match (applied per-line)
#   exclude   — optional compiled pattern; if it matches the same line, skip
#   files     — glob for which files to scan (relative to SRC)

@dataclass
class BugPattern:
    id: str
    title: str
    why: str
    regex: re.Pattern
    exclude: re.Pattern | None = None
    files: str = "**/*.cpp"
    advisory: bool = False  # If True, reported but doesn't fail CI

@dataclass
class Violation:
    pattern_id: str
    file: Path
    line: int
    text: str


PATTERNS: list[BugPattern] = [
    # ── millis() wraparound ──────────────────────────────────────
    # Dangerous: `now < deadlineMs` or `now >= deadlineMs` using
    # unsigned millis().  After 49-day wraparound, `now` drops to 0
    # and the comparison breaks permanently.
    # Safe alternative: static_cast<int32_t>(now - deadline) < 0
    BugPattern(
        id="MILLIS_DIRECT_COMPARE",
        title="Unsafe millis() direct comparison",
        why="now < deadline / now >= deadline breaks after 49-day wraparound. "
            "Use static_cast<int32_t>(now - deadline) instead.",
        regex=re.compile(
            r"""
            (?:                         # Match variable names
              \bnow\b                   # 'now'
            | \bnowMs\b                 # 'nowMs'
            | \bmillis\s*\(\s*\)        # 'millis()'
            )
            \s*                         # optional space
            (?:<(?!=)|>=)               # < (not <=) or >=
            \s*                         # optional space
            (?!                         # NOT followed by an elapsed-time subtraction
              \w+\s*-                   # (if RHS is a subtraction, it's elapsed-time pattern)
            )
            \w+                         # the deadline variable
            (?:Ms|_ms|AllowedMs|UntilMs|StartMs|AtMs|DeadlineMs)  # must end with a timestamp suffix
            """,
            re.VERBOSE,
        ),
        # Skip lines that already use the safe cast pattern
        exclude=re.compile(r"static_cast<int32_t>"),
    ),

    # ── int16_t accumulator overflow ─────────────────────────────
    # Accumulating into int16_t without a bounds guard overflows
    # after ~327 additions of values in [-100, 100].
    # Uses context-aware scanning (see scan_file).
    BugPattern(
        id="INT16_ACCUMULATOR",
        title="Unbounded int16_t accumulation",
        why="int16_t += cast<int16_t>(...) overflows after ~327 iterations. "
            "Guard with a sample/iteration cap.",
        regex=re.compile(
            r"\bint16_t\b.*\+=|"            # int16_t var += ...
            r"\+=\s*static_cast<int16_t>"   # var += static_cast<int16_t>(...)
        ),
        # Allow if a guard is visible nearby (context window checked in scan_file)
        exclude=re.compile(r"<\s*255|count\s*<|samplecount|bounded", re.IGNORECASE),
    ),

    # ── cos(lat) missing in geo distance ─────────────────────────
    # Longitude degrees are physically narrower near the poles.
    # Any distance/radius calculation on lat/lon that doesn't
    # multiply dLon by cos(lat) will over-estimate E-W distance.
    BugPattern(
        id="GEO_DISTANCE_NO_COSLAT",
        title="Geo distance without cos(lat) correction",
        why="Longitude-delta distance calculations must scale by cos(latitude). "
            "Without this, E-W distance is wrong by up to 2x at 60° latitude.",
        regex=re.compile(
            r"(?:dLon|deltaLon|lonDiff|dlonE5|dlon)\s*\*\s*(?:dLon|deltaLon|lonDiff|dlonE5|dlon)"
        ),
        # Exclude if cosLat appears nearby (checked via context window in scan_file)
        exclude=re.compile(r"\b(?:cos|cosf)\s*\(|\bcosLat(?:Scale|Clamped)?\b"),
    ),

    # ── Blocking semaphore take in BLE callbacks ─────────────────
    # BLE callbacks run on the NimBLE task. portMAX_DELAY blocks
    # the entire BLE stack and can cause watchdog resets.
    BugPattern(
        id="BLE_CALLBACK_BLOCKING",
        title="Blocking semaphore in BLE callback context",
        why="xSemaphoreTake with portMAX_DELAY in a BLE callback blocks the "
            "NimBLE task, risking watchdog reset and dropped connections.",
        regex=re.compile(r"xSemaphoreTake\s*\([^)]*portMAX_DELAY"),
        files="**/ble_*.cpp",
    ),

    # ── Raw new/delete in firmware ───────────────────────────────
    # ESP32 heap is fragile.  Prefer stack, static, or PSRAM alloc.
    # NOTE: NimBLE callbacks and std::unique_ptr require new.
    # This pattern has a high false-positive rate and is tracked
    # in --report mode only (not gated in CI).
    BugPattern(
        id="RAW_HEAP_ALLOC",
        title="Raw new/malloc in firmware",
        why="Heap fragmentation on ESP32 causes OOM crashes. "
            "Use stack allocation, static buffers, or ps_malloc.",
        regex=re.compile(r"(?<!\w)new\s+[A-Z]|\bmalloc\s*\(|\bcalloc\s*\("),
        exclude=re.compile(
            r"ps_malloc|ps_calloc|heap_caps_malloc|PSRAM|"
            r"placement.new|override|//|nothrow|std::nothrow|"
            r"#\s*include|#\s*define|UNIT_TEST|test|"
            r"LOG|log|print|reset\(|unique_ptr|make_unique|"
            r'\".*new|Initializing'
        ),
        files="**/*.cpp",
        advisory=True,  # Too many NimBLE/std false positives to gate
    ),
]


# ---------------------------------------------------------------------------
# Scanner
# ---------------------------------------------------------------------------

def strip_comments_from_line(line: str, in_block_comment: bool) -> tuple[str, bool]:
    """Remove C/C++ comments from one line while preserving code outside them."""
    out: list[str] = []
    index = 0

    while index < len(line):
        if in_block_comment:
            end = line.find("*/", index)
            if end == -1:
                return "".join(out), True
            index = end + 2
            in_block_comment = False
            continue

        line_comment = line.find("//", index)
        block_comment = line.find("/*", index)

        if line_comment != -1 and (block_comment == -1 or line_comment < block_comment):
            out.append(line[index:line_comment])
            return "".join(out), False

        if block_comment != -1:
            out.append(line[index:block_comment])
            index = block_comment + 2
            in_block_comment = True
            continue

        out.append(line[index:])
        return "".join(out), False

    return "".join(out), in_block_comment


def strip_comments(lines: list[str]) -> list[str]:
    """Strip comments from a list of source lines."""
    in_block_comment = False
    code_lines: list[str] = []
    for line in lines:
        code_line, in_block_comment = strip_comments_from_line(line, in_block_comment)
        code_lines.append(code_line)
    return code_lines


def find_matching_files(base: Path, files_glob: str) -> list[Path]:
    """Resolve the file glob for a bug pattern."""
    if files_glob.startswith("**/"):
        matches = base.rglob(files_glob[3:])
    else:
        matches = base.glob(files_glob)
    return sorted(path for path in matches if path.is_file())


def scan_file(path: Path, pattern: BugPattern) -> list[Violation]:
    """Scan a single file for a single pattern."""
    violations = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return violations

    lines = text.splitlines()
    code_lines = strip_comments(lines)
    # Patterns that need multi-line context to check guards
    CONTEXT_PATTERNS = {"INT16_ACCUMULATOR", "GEO_DISTANCE_NO_COSLAT"}
    context_window = 5  # lines above/below to check for exclude pattern

    for i, line in enumerate(code_lines):
        lineno = i + 1
        stripped = line.strip()
        if not stripped:
            continue
        if pattern.regex.search(line):
            if pattern.exclude:
                # For context-aware patterns, check surrounding lines too
                if pattern.id in CONTEXT_PATTERNS:
                    start = max(0, i - context_window)
                    end = min(len(code_lines), i + context_window + 1)
                    context = "\n".join(code_lines[start:end])
                    if pattern.exclude.search(context):
                        continue
                elif pattern.exclude.search(line):
                    continue
            violations.append(Violation(
                pattern_id=pattern.id,
                file=path,
                line=lineno,
                text=lines[i].strip(),
            ))
    return violations


def scan_all(patterns: list[BugPattern]) -> list[Violation]:
    """Run all patterns against matching source files."""
    all_violations: list[Violation] = []

    for pattern in patterns:
        for path in find_matching_files(SRC, pattern.files):
            all_violations.extend(scan_file(path, pattern))

    return all_violations


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def print_report(violations: list[Violation], patterns: list[BugPattern]) -> None:
    """Print a human-readable scorecard."""
    # Group by pattern
    by_pattern: dict[str, list[Violation]] = {p.id: [] for p in patterns}
    for v in violations:
        by_pattern[v.pattern_id].append(v)

    total = len(violations)
    gated_patterns = [p for p in patterns if not p.advisory]
    advisory_patterns = [p for p in patterns if p.advisory]
    gated_violations = sum(len(by_pattern[p.id]) for p in gated_patterns)
    clean = sum(1 for p in patterns if len(by_pattern[p.id]) == 0)

    print()
    print("=" * 64)
    print("  BUG PATTERN SCAN — SCORECARD")
    print("=" * 64)

    if gated_patterns:
        print()
        print("  GATED (CI fails on violations):")
        for pattern in gated_patterns:
            hits = by_pattern[pattern.id]
            status = "CLEAN" if not hits else f"{len(hits)} violation(s)"
            icon = "\u2705" if not hits else "\u274c"
            print(f"    {icon}  {pattern.title}: {status}")
            if hits:
                print(f"        Why: {pattern.why}")
                for v in hits[:5]:
                    rel = v.file.relative_to(ROOT)
                    print(f"        {rel}:{v.line}: {v.text[:90]}")
                if len(hits) > 5:
                    print(f"        ... and {len(hits) - 5} more")

    if advisory_patterns:
        print()
        print("  ADVISORY (tracked, not gated):")
        for pattern in advisory_patterns:
            hits = by_pattern[pattern.id]
            status = "CLEAN" if not hits else f"{len(hits)} hit(s)"
            icon = "\u2705" if not hits else "\u26a0\ufe0f"
            print(f"    {icon}  {pattern.title}: {status}")
            if hits:
                for v in hits[:3]:
                    rel = v.file.relative_to(ROOT)
                    print(f"        {rel}:{v.line}: {v.text[:90]}")
                if len(hits) > 3:
                    print(f"        ... and {len(hits) - 3} more")

    print()
    print("-" * 64)
    print(f"  {clean}/{len(patterns)} patterns clean  |"
          f"  {gated_violations} gated  |  {total} total violations")
    print("-" * 64)
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Scan for known bug patterns")
    parser.add_argument("--report", action="store_true",
                        help="Print scorecard without failing")
    args = parser.parse_args()

    violations = scan_all(PATTERNS)
    print_report(violations, PATTERNS)

    if args.report:
        return 0

    # Only gated violations cause CI failure
    gated = [v for v in violations
             if not next(p for p in PATTERNS if p.id == v.pattern_id).advisory]
    if gated:
        print(f"FAIL: {len(gated)} gated bug-pattern violation(s) found.")
        print("Fix the violations above, or use --report for scorecard-only mode.")
        return 1
    print("PASS: All gated bug-pattern checks clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
