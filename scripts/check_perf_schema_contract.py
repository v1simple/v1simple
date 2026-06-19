#!/usr/bin/env python3
"""Check perf schema cross-file referential integrity.

The perf/observability layer wires several files together:

  - ``src/perf_metrics.h``           → ground-truth C++ struct fields
  - ``tools/metric_schema.py``       → canonical metric names + CSV aliases
  - ``test/contracts/perf_csv_column_contract.txt``
                                      → frozen CSV header column order

The C++ compiler catches ``PERF_INC(typo)`` at build time (it expands to
``perfCounters.typo++``), but it does NOT catch drift in the Python schema
that drives offline scoring, soak KV export, and CSV import. This script
closes that gap.

Enforced invariants:

  A. Every CSV alias in ``CSV_DELTA_COLUMNS``, ``CSV_PEAK_DIAGNOSTIC_COLUMNS``,
     ``CSV_PEAK_ONLY_COLUMNS``, and ``CSV_CONNECT_BURST_PEAK_COLUMNS`` appears
     in ``perf_csv_column_contract.txt``.
  B. Every source counter name in ``DISPLAY_COUNTER_DELTA_MAPPINGS`` exists
     as a field in ``PerfCounters`` or ``PerfExtendedMetrics``.
  C. Every C++ field name in ``DISPLAY_SAMPLE_FIELD_MAPPINGS`` exists as a
     field in ``PerfCounters`` or ``PerfExtendedMetrics``.
  D. Every CSV-dict key (``CSV_DELTA_COLUMNS``, ``CSV_PEAK_DIAGNOSTIC_COLUMNS``,
     ``CSV_PEAK_ONLY_COLUMNS``) appears in ``CANONICAL_METRIC_UNITS``.
     ``metric_unit()`` will raise ``KeyError`` at runtime if this drifts.
     (``DISPLAY_COUNTER_DELTA_MAPPINGS`` / ``DISPLAY_SAMPLE_FIELD_MAPPINGS``
     emit soak output-record keys that are deliberately not canonical.)
  E. Every key in ``SOAK_TREND_METRIC_KV_ALIASES`` exists in
     ``CANONICAL_METRIC_UNITS`` (KV aliases only rename real metrics).
  F. Every ``SOAK_TREND_METRIC_NAMES`` entry appears in
     ``CANONICAL_METRIC_UNITS``.

This is a cheap, deterministic check — no build required. Wire it into
``scripts/ci-test.sh`` alongside the other ``check_perf_*_contract.py``
gates.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Iterable, List, Set, Tuple


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import metric_schema  # type: ignore  # noqa: E402


PERF_METRICS_HEADER = ROOT / "src" / "perf_metrics.h"
CSV_COLUMN_CONTRACT = ROOT / "test" / "contracts" / "perf_csv_column_contract.txt"


STRUCT_RE = re.compile(r"struct\s+(\w+)\s*\{")
# Catches std::atomic<T> name{...};
ATOMIC_FIELD_RE = re.compile(
    r"^\s*std::atomic<[^>]+>\s+(\w+)\s*[\{=;]", re.MULTILINE
)
# Catches plain POD fields: "uint32_t name = 0;" or "uint8_t name{0};"
POD_FIELD_RE = re.compile(
    r"^\s*(?:uint\d+_t|int\d+_t|size_t|bool)\s+(\w+)\s*[\{=;]", re.MULTILINE
)


def read_text(path: Path) -> str:
    if not path.exists():
        raise FileNotFoundError(f"Required file not found: {path}")
    return path.read_text(encoding="utf-8")


def find_struct_body(source: str, struct_name: str) -> str:
    match = re.search(rf"struct\s+{re.escape(struct_name)}\s*\{{", source)
    if not match:
        raise RuntimeError(f"struct {struct_name!r} not found in {PERF_METRICS_HEADER}")
    start = match.end() - 1
    depth = 0
    for idx in range(start, len(source)):
        ch = source[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : idx]
    raise RuntimeError(f"Unbalanced braces in struct {struct_name!r}")


def extract_struct_fields(source: str, struct_name: str) -> Set[str]:
    body = find_struct_body(source, struct_name)
    # Strip out any nested struct bodies so we don't accidentally pull in their
    # fields (PerfExtendedMetrics contains histogram sub-structs).
    stripped = []
    depth = 0
    for ch in body:
        if ch == "{":
            depth += 1
            continue
        if ch == "}":
            depth -= 1
            continue
        if depth == 0:
            stripped.append(ch)
    body = "".join(stripped)

    fields: Set[str] = set()
    for match in ATOMIC_FIELD_RE.finditer(body):
        fields.add(match.group(1))
    for match in POD_FIELD_RE.finditer(body):
        fields.add(match.group(1))
    return fields


def load_csv_header_columns(path: Path) -> Set[str]:
    columns: Set[str] = set()
    for raw in read_text(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        columns.add(line)
    return columns


def report(issues: Iterable[Tuple[str, List[str]]]) -> bool:
    ok = True
    for label, items in issues:
        if not items:
            continue
        ok = False
        print(f"[contract] perf-schema: {label}")
        for item in sorted(items):
            print(f"    - {item}")
    return ok


def main() -> int:
    header_source = read_text(PERF_METRICS_HEADER)
    counter_fields = extract_struct_fields(header_source, "PerfCounters")
    extended_fields = extract_struct_fields(header_source, "PerfExtendedMetrics")
    all_cpp_fields = counter_fields | extended_fields

    csv_header_columns = load_csv_header_columns(CSV_COLUMN_CONTRACT)

    canonical_names: Set[str] = set(metric_schema.CANONICAL_METRIC_UNITS.keys())

    issues: List[Tuple[str, List[str]]] = []

    # A. CSV aliases must appear in the frozen CSV header contract.
    csv_alias_mappings: List[Tuple[str, dict]] = [
        ("CSV_DELTA_COLUMNS", metric_schema.CSV_DELTA_COLUMNS),
        ("CSV_PEAK_DIAGNOSTIC_COLUMNS", metric_schema.CSV_PEAK_DIAGNOSTIC_COLUMNS),
        ("CSV_PEAK_ONLY_COLUMNS", metric_schema.CSV_PEAK_ONLY_COLUMNS),
        ("CSV_CONNECT_BURST_PEAK_COLUMNS", metric_schema.CSV_CONNECT_BURST_PEAK_COLUMNS),
    ]
    for label, mapping in csv_alias_mappings:
        missing_aliases = [
            f"{label}[{metric!r}] -> CSV column {alias!r} not in perf_csv_column_contract.txt"
            for metric, alias in mapping.items()
            if alias not in csv_header_columns
        ]
        issues.append(("CSV alias missing from header contract", missing_aliases))

    # B. Display counter source names must exist in the C++ structs.
    missing_counter_sources = [
        f"DISPLAY_COUNTER_DELTA_MAPPINGS: counter {source!r} (for {canonical!r}) "
        f"not found in PerfCounters/PerfExtendedMetrics"
        for source, canonical in metric_schema.DISPLAY_COUNTER_DELTA_MAPPINGS
        if source not in all_cpp_fields
    ]
    issues.append(("display-counter source not in C++ structs", missing_counter_sources))

    # C. Display sample field names must exist in the C++ structs.
    missing_sample_fields = [
        f"DISPLAY_SAMPLE_FIELD_MAPPINGS: field {cpp_field!r} (for {canonical!r}) "
        f"not found in PerfCounters/PerfExtendedMetrics"
        for canonical, cpp_field in metric_schema.DISPLAY_SAMPLE_FIELD_MAPPINGS
        if cpp_field not in all_cpp_fields
    ]
    issues.append(("display-sample field not in C++ structs", missing_sample_fields))

    # D. Every CSV-dict key must be a declared canonical metric name
    # (import_perf_csv.py calls metric_unit() on these keys).
    referenced_canonical: Set[str] = set()
    for mapping in (
        metric_schema.CSV_DELTA_COLUMNS,
        metric_schema.CSV_PEAK_DIAGNOSTIC_COLUMNS,
        metric_schema.CSV_PEAK_ONLY_COLUMNS,
        metric_schema.CSV_CONNECT_BURST_PEAK_COLUMNS,
    ):
        referenced_canonical.update(mapping.keys())

    missing_canonical = [
        f"canonical metric {name!r} referenced by CSV mapping but missing from CANONICAL_METRIC_UNITS"
        for name in sorted(referenced_canonical - canonical_names)
    ]
    issues.append(("canonical metric undeclared", missing_canonical))

    # E. KV alias keys must be real canonical metric names.
    missing_kv_alias_bases = [
        f"SOAK_TREND_METRIC_KV_ALIASES[{name!r}] -> canonical metric not in CANONICAL_METRIC_UNITS"
        for name in metric_schema.SOAK_TREND_METRIC_KV_ALIASES
        if name not in canonical_names
    ]
    issues.append(
        ("soak KV alias references unknown canonical metric", missing_kv_alias_bases)
    )

    # F. SOAK_TREND_METRIC_NAMES must be subset of CANONICAL_METRIC_UNITS.
    missing_soak_trend = [
        f"SOAK_TREND_METRIC_NAMES entry {name!r} missing from CANONICAL_METRIC_UNITS"
        for name in metric_schema.SOAK_TREND_METRIC_NAMES
        if name not in canonical_names
    ]
    issues.append(("soak trend metric undeclared", missing_soak_trend))

    ok = report(issues)

    if not ok:
        print(
            "\n[contract] perf-schema: fix the referential drift above. "
            "Either update tools/metric_schema.py to match renamed counters "
            "or rename the offending struct field in src/perf_metrics.h."
        )
        return 1

    print(
        "[contract] perf-schema contract intact "
        f"(counters={len(counter_fields)}, extended={len(extended_fields)}, "
        f"canonical={len(canonical_names)}, csv_cols={len(csv_header_columns)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
