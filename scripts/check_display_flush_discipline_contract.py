#!/usr/bin/env python3
"""Check display flush discipline contract.

Enforced invariants:
1) Display flush call-site snapshot remains stable (DISPLAY_FLUSH / flush calls).
2) Display allocation patterns (new/malloc/String) are only allowed in begin/init scopes.

Use --update to rewrite expected call-site snapshot.
"""

from __future__ import annotations

import argparse
from collections import Counter
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[1]
CONTRACT_FILE = ROOT / "test" / "contracts" / "display_flush_discipline_contract.txt"

SRC_ROOT = ROOT / "src"
INCLUDE_ROOT = ROOT / "include"
DISPLAY_PIPELINE_FILE = SRC_ROOT / "modules" / "display" / "display_pipeline_module.cpp"
DISPLAY_FLUSH_HEADER = INCLUDE_ROOT / "display_flush.h"

MASK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)
FUNC_RE = re.compile(
    r"(?m)^[^#\n{};]*\b([A-Za-z_~][A-Za-z0-9_:~]*)\s*\([^;{}]*\)\s*(?:const\s*)?\{"
)

DISPLAY_FLUSH_CALL_RE = re.compile(r"\bDISPLAY_FLUSH\s*\(")
POINTER_FLUSH_CALL_RE = re.compile(r"->\s*flush\s*\(")
BARE_FLUSH_CALL_RE = re.compile(r"(?<![:.>\w])flush\s*\(")
LINE_TOKEN_RE = re.compile(r"\bline=\d+\b")

ALLOCATION_PATTERNS: Sequence[Tuple[str, re.Pattern[str]]] = (
    ("forbidden_new", re.compile(r"\bnew\b")),
    ("forbidden_malloc", re.compile(r"\bmalloc\s*\(")),
    ("forbidden_calloc", re.compile(r"\bcalloc\s*\(")),
    ("forbidden_realloc", re.compile(r"\brealloc\s*\(")),
    ("forbidden_string", re.compile(r"\bString\b")),
)

CONTROL_KEYWORDS = {"if", "for", "while", "switch", "catch"}


@dataclass(frozen=True)
class FunctionRange:
    name: str
    open_brace: int
    close_brace: int


def read_text(path: Path) -> str:
    if not path.exists():
        raise FileNotFoundError(f"Source file not found: {path}")
    return path.read_text(encoding="utf-8")


def to_relative(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def line_for_index(source: str, index: int) -> int:
    return source.count("\n", 0, index) + 1


def mask_comments_and_strings(source: str) -> str:
    def _mask(match: re.Match[str]) -> str:
        return "".join("\n" if ch == "\n" else " " for ch in match.group(0))

    return MASK_RE.sub(_mask, source)


def find_matching_brace(masked_source: str, open_brace: int) -> int:
    depth = 0
    for idx in range(open_brace, len(masked_source)):
        ch = masked_source[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return idx
    raise ValueError(f"Unbalanced braces while parsing function starting at {open_brace}")


def extract_function_ranges(masked_source: str) -> List[FunctionRange]:
    ranges: List[FunctionRange] = []
    for match in FUNC_RE.finditer(masked_source):
        name = match.group(1)
        if name in CONTROL_KEYWORDS:
            continue
        open_brace = match.end() - 1
        try:
            close_brace = find_matching_brace(masked_source, open_brace)
        except ValueError:
            continue
        ranges.append(FunctionRange(name=name, open_brace=open_brace, close_brace=close_brace))
    return ranges


def enclosing_function_name(ranges: List[FunctionRange], index: int) -> str:
    best: FunctionRange | None = None
    for fr in ranges:
        if fr.open_brace <= index <= fr.close_brace:
            if best is None or (fr.close_brace - fr.open_brace) < (best.close_brace - best.open_brace):
                best = fr
    return best.name if best else "<global>"


def display_scan_files() -> List[Path]:
    files = sorted(SRC_ROOT.rglob("*.cpp"))
    files.append(DISPLAY_FLUSH_HEADER)
    deduped: List[Path] = []
    seen = set()
    for path in files:
        if path in seen:
            continue
        seen.add(path)
        deduped.append(path)
    return deduped


def allocation_scan_files() -> List[Path]:
    files = sorted(SRC_ROOT.glob("display*.cpp"))
    if DISPLAY_PIPELINE_FILE.exists():
        files.append(DISPLAY_PIPELINE_FILE)
    deduped: List[Path] = []
    seen = set()
    for path in files:
        if path in seen:
            continue
        seen.add(path)
        deduped.append(path)
    return deduped


def extract_flush_callsites(path: Path) -> List[str]:
    source = read_text(path)
    masked = mask_comments_and_strings(source)
    ranges = extract_function_ranges(masked) if path.suffix == ".cpp" else []
    relative = to_relative(path)
    out: List[str] = []

    if path != DISPLAY_FLUSH_HEADER:
        for match in DISPLAY_FLUSH_CALL_RE.finditer(masked):
            line = line_for_index(source, match.start())
            func = enclosing_function_name(ranges, match.start())
            out.append(f"kind=DISPLAY_FLUSH file={relative} line={line} function={func}")

    for match in POINTER_FLUSH_CALL_RE.finditer(masked):
        line = line_for_index(source, match.start())
        func = enclosing_function_name(ranges, match.start())
        out.append(f"kind=POINTER_FLUSH file={relative} line={line} function={func}")

    if path != DISPLAY_FLUSH_HEADER:
        for match in BARE_FLUSH_CALL_RE.finditer(masked):
            line = line_for_index(source, match.start())
            func = enclosing_function_name(ranges, match.start())
            out.append(f"kind=BARE_FLUSH file={relative} line={line} function={func}")

    return out


def extract_all_flush_callsites() -> List[str]:
    out: List[str] = []
    for path in display_scan_files():
        out.extend(extract_flush_callsites(path))
    return sorted(set(out))


def extract_allocation_violations() -> List[str]:
    violations: List[str] = []
    for path in allocation_scan_files():
        source = read_text(path)
        masked = mask_comments_and_strings(source)
        ranges = extract_function_ranges(masked)
        relative = to_relative(path)

        for rule, pattern in ALLOCATION_PATTERNS:
            for match in pattern.finditer(masked):
                line = line_for_index(source, match.start())
                func = enclosing_function_name(ranges, match.start())
                func_lower = func.lower()
                allowed = func != "<global>" and ("begin" in func_lower or "init" in func_lower)
                if allowed:
                    continue
                violations.append(
                    f"file={relative} line={line} function={func} rule={rule}"
                )
    return sorted(set(violations))


def read_expected_lines(path: Path) -> List[str]:
    if not path.exists():
        return []
    lines: List[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        lines.append(line)
    return lines


def write_lines(path: Path, header: str, lines: List[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = [header, ""]
    payload.extend(lines)
    payload.append("")
    path.write_text("\n".join(payload), encoding="utf-8")


def normalize_callsite_line(row: str) -> str:
    normalized = LINE_TOKEN_RE.sub("line=*", row)
    return re.sub(r"\s+", " ", normalized).strip()


def multiset_rows(counter: Counter[str]) -> List[str]:
    rows: List[str] = []
    for row in sorted(counter):
        rows.extend([row] * counter[row])
    return rows


def print_diff(expected: List[str], actual: List[str]) -> None:
    expected_counter = Counter(expected)
    actual_counter = Counter(actual)
    missing = multiset_rows(expected_counter - actual_counter)
    extra = multiset_rows(actual_counter - expected_counter)

    print("[contract] display-flush-discipline snapshot mismatch")
    if missing:
        print("  missing:")
        for row in missing:
            print(f"    - {row}")
    if extra:
        print("  extra:")
        for row in extra:
            print(f"    + {row}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite expected contract snapshot from current source",
    )
    args = parser.parse_args()

    callsites = extract_all_flush_callsites()
    allocation_violations = extract_allocation_violations()

    if args.update:
        write_lines(
            CONTRACT_FILE,
            "# Display flush discipline contract (flush call-site snapshot)",
            callsites,
        )
        print(f"Updated {CONTRACT_FILE}")

    expected_callsites = read_expected_lines(CONTRACT_FILE)
    expected_normalized = [normalize_callsite_line(row) for row in expected_callsites]
    actual_normalized = [normalize_callsite_line(row) for row in callsites]
    ok = True

    if Counter(expected_normalized) != Counter(actual_normalized):
        print_diff(expected_normalized, actual_normalized)
        ok = False
    elif expected_callsites != callsites:
        print(
            "[contract] display-flush-discipline line-offset drift detected "
            "(structural call-sites unchanged)"
        )

    if allocation_violations:
        print("[contract] display allocation violations detected")
        for row in allocation_violations:
            print(f"  - {row}")
        ok = False

    if not ok:
        print("\nRun with --update only when intentionally changing contract.")
        return 1

    print(
        "[contract] display-flush-discipline contract matches "
        f"({len(callsites)} call-sites, 0 allocation violations)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
