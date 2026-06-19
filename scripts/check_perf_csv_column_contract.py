#!/usr/bin/env python3
"""Check perf CSV column contract for src/perf_sd_logger.cpp.

Enforced invariants:
1) PERF_CSV_HEADER column order matches expected snapshot.
2) PERF_CSV_HEADER column count equals the serialized field count in appendSnapshotLine().
3) expectedPerfCsvColumns() derives from PERF_CSV_HEADER.

Use --update to rewrite expected snapshot.
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path
from typing import List

ROOT = Path(__file__).resolve().parents[1]
SRC_FILE = ROOT / "src" / "perf_sd_logger.cpp"
CONTRACT_FILE = ROOT / "test" / "contracts" / "perf_csv_column_contract.txt"

MASK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)
PERF_HEADER_RE = re.compile(
    r"static\s+constexpr\s+const\s+char\s*\*\s*PERF_CSV_HEADER\s*=\s*((?:\"(?:\\.|[^\"\\])*\"\s*)+);",
    re.DOTALL,
)
SNPRINTF_FORMAT_RE = re.compile(
    r"int\s+n\s*=\s*snprintf\s*\(\s*line\s*,\s*sizeof\s*\(\s*line\s*\)\s*,\s*((?:\"(?:\\.|[^\"\\])*\"\s*)+)\s*,",
    re.DOTALL,
)
APPEND_CALL_RE = re.compile(r"\bappendCsv(?:UInt32|UInt32Last|UInt16|UInt8|UInt8Last|Int32|Int16|UtcField)\s*\(")


def read_text(path: Path) -> str:
    if not path.exists():
        raise FileNotFoundError(f"Source file not found: {path}")
    return path.read_text(encoding="utf-8")


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


def mask_comments_and_strings(source: str) -> str:
    def _mask(match: re.Match[str]) -> str:
        return "".join("\n" if ch == "\n" else " " for ch in match.group(0))

    return MASK_RE.sub(_mask, source)


def decode_c_string_blob(blob: str) -> str:
    pieces: List[str] = []
    for match in re.finditer(r"\"(?:\\.|[^\"\\])*\"", blob, re.DOTALL):
        pieces.append(ast.literal_eval(match.group(0)))
    if not pieces:
        raise ValueError("No C string literals found in blob")
    return "".join(pieces)


def find_matching_brace(masked_source: str, open_brace_index: int) -> int:
    depth = 0
    for idx in range(open_brace_index, len(masked_source)):
        ch = masked_source[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return idx
    raise ValueError("Unbalanced braces while parsing function body")


def extract_function_body(source: str, masked_source: str, func_name: str) -> str:
    sig_re = re.compile(rf"\b{re.escape(func_name)}\s*\([^)]*\)\s*\{{")
    match = sig_re.search(masked_source)
    if not match:
        return ""
    open_brace = match.end() - 1
    close_brace = find_matching_brace(masked_source, open_brace)
    return source[open_brace + 1 : close_brace]


def count_format_fields(fmt: str) -> int:
    count = 0
    idx = 0
    while idx < len(fmt):
        if fmt[idx] != "%":
            idx += 1
            continue
        if idx + 1 < len(fmt) and fmt[idx + 1] == "%":
            idx += 2
            continue
        count += 1
        idx += 1
    return count


def print_diff(expected: List[str], actual: List[str]) -> None:
    expected_set = set(expected)
    actual_set = set(actual)
    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)

    print("[contract] perf-csv-column snapshot mismatch")
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

    source = read_text(SRC_FILE)
    masked_source = mask_comments_and_strings(source)

    header_match = PERF_HEADER_RE.search(source)
    if not header_match:
        print("[contract] perf-csv-column: PERF_CSV_HEADER literal not found")
        return 1
    header_text = decode_c_string_blob(header_match.group(1))
    header_line = header_text.splitlines()[0] if header_text else ""
    columns = [col.strip() for col in header_line.split(",") if col.strip()]

    header_count = len(columns)
    formatter_count = 0

    append_body = extract_function_body(source, masked_source, "PerfSdLogger::appendSnapshotLine")
    if append_body:
        formatter_count = len(APPEND_CALL_RE.findall(append_body))

    if formatter_count == 0:
        format_match = SNPRINTF_FORMAT_RE.search(source)
        if not format_match:
            print("[contract] perf-csv-column: serialized field format not found")
            return 1
        format_text = decode_c_string_blob(format_match.group(1))
        formatter_count = count_format_fields(format_text)

    expected_fn_body = extract_function_body(
        source,
        masked_source,
        "expectedPerfCsvColumns",
    )
    derives_from_header = bool(
        re.search(
            r"countCsvColumns\s*\(\s*PERF_CSV_HEADER\s*,\s*strlen\s*\(\s*PERF_CSV_HEADER\s*\)\s*\)",
            expected_fn_body,
        )
    )

    if args.update:
        write_lines(
            CONTRACT_FILE,
            "# Perf CSV column contract (PERF_CSV_HEADER order)",
            columns,
        )
        print(f"Updated {CONTRACT_FILE}")

    expected_columns = read_expected_lines(CONTRACT_FILE)
    ok = True

    if expected_columns != columns:
        print_diff(expected_columns, columns)
        ok = False

    if header_count != formatter_count:
        print(
            "[contract] perf-csv-column count mismatch: "
            f"header_columns={header_count} serialized_fields={formatter_count}"
        )
        ok = False

    if not derives_from_header:
        print(
            "[contract] perf-csv-column mismatch: "
            "expectedPerfCsvColumns() is not derived from PERF_CSV_HEADER"
        )
        ok = False

    if not ok:
        print("\nRun with --update only when intentionally changing contract.")
        return 1

    print(
        "[contract] perf-csv-column contract matches "
        f"({header_count} columns, {formatter_count} serialized fields)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
