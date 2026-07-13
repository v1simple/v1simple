#!/usr/bin/env python3
"""Regression tests for scripts/check_bug_patterns.py."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_bug_patterns as scanner  # type: ignore  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_file(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def pattern_by_id(pattern_id: str) -> scanner.BugPattern:
    return next(pattern for pattern in scanner.PATTERNS if pattern.id == pattern_id)


def test_find_matching_files_handles_recursive_and_non_recursive_globs(tmpdir: Path) -> None:
    root_file = tmpdir / "top.cpp"
    nested_file = tmpdir / "nested" / "child.cpp"
    header_file = tmpdir / "nested" / "child.h"
    write_file(root_file, "int main() { return 0; }\n")
    write_file(nested_file, "int nested() { return 0; }\n")
    write_file(header_file, "#pragma once\n")

    top_only = scanner.find_matching_files(tmpdir, "*.cpp")
    recursive = scanner.find_matching_files(tmpdir, "**/*.cpp")

    assert_true(top_only == [root_file], "non-recursive glob should only match the top-level file")
    assert_true(
        recursive == [nested_file, root_file],
        "recursive glob should match both top-level and nested files",
    )


def test_millis_comment_words_do_not_suppress_violation(tmpdir: Path) -> None:
    source = tmpdir / "millis.cpp"
    write_file(
        source,
        "void f(uint32_t now, uint32_t deadlineMs) {\n"
        "    if (now < deadlineMs) { // wraparound bug still unsafe\n"
        "    }\n"
        "}\n",
    )

    hits = scanner.scan_file(source, pattern_by_id("MILLIS_DIRECT_COMPARE"))
    assert_true(len(hits) == 1, "unsafe millis compare must not be silenced by comment text")


def test_geo_comment_or_cost_name_do_not_suppress_violation(tmpdir: Path) -> None:
    source = tmpdir / "geo.cpp"
    write_file(
        source,
        "void f(float dLon) {\n"
        "    float cost = 1.0f;\n"
        "    // cosLatScale would be needed here, but this comment is not enough\n"
        "    float radius = dLon * dLon;\n"
        "    (void)cost;\n"
        "    (void)radius;\n"
        "}\n",
    )

    hits = scanner.scan_file(source, pattern_by_id("GEO_DISTANCE_NO_COSLAT"))
    assert_true(len(hits) == 1, "comments and unrelated identifiers must not suppress geo violations")


def test_geo_real_coslat_context_suppresses_violation(tmpdir: Path) -> None:
    source = tmpdir / "geo_safe.cpp"
    write_file(
        source,
        "void f(float lat, float dLon) {\n"
        "    const float cosLat = cosf(lat);\n"
        "    float radius = dLon * dLon;\n"
        "    (void)cosLat;\n"
        "    (void)radius;\n"
        "}\n",
    )

    hits = scanner.scan_file(source, pattern_by_id("GEO_DISTANCE_NO_COSLAT"))
    assert_true(len(hits) == 0, "real cos(lat) correction in nearby code should suppress the geo warning")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="bug_pattern_scanner_") as tmp:
        tmpdir = Path(tmp)
        test_find_matching_files_handles_recursive_and_non_recursive_globs(tmpdir)
        test_millis_comment_words_do_not_suppress_violation(tmpdir)
        test_geo_comment_or_cost_name_do_not_suppress_violation(tmpdir)
        test_geo_real_coslat_context_suppresses_violation(tmpdir)

    print("[bug-pattern-scanner] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
