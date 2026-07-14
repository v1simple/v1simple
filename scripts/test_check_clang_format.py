#!/usr/bin/env python3
"""Regression tests for scripts/check_clang_format.py.

Every case builds a throwaway repo root (its own .clang-format,
.clang-format-ignore and manifest) and drives the checker against it, so the
tests never depend on the state of the real tree. A gate that cannot fail is
not a gate: the drift, missing-entry, excluded-entry, unsorted-manifest and
version-mismatch paths are all asserted to return 1.
"""

from __future__ import annotations

import io
import shutil
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_clang_format as checker  # type: ignore  # noqa: E402

CLEAN_SOURCE = """#pragma once

int add(int left, int right) {
    return left + right;
}
"""

DRIFTED_SOURCE = """#pragma once

int add(int   left,int right){
      return left+right;
}
"""

GENERATED_HEADER = """#pragma once
static const unsigned char kBlob[] = {
        0x00,0x01,   0x02 };
"""


def assert_equal(actual: Any, expected: Any, message: str) -> None:
    if actual != expected:
        raise AssertionError(f"{message}: expected {expected!r}, got {actual!r}")


def assert_in(needle: str, haystack: str, message: str) -> None:
    if needle not in haystack:
        raise AssertionError(f"{message}: {needle!r} not found in:\n{haystack}")


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_root(tmpdir: Path, manifest_lines: list[str]) -> Path:
    """Build a miniature repo: real .clang-format, one clean and one drifted file."""
    tmpdir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / ".clang-format", tmpdir / ".clang-format")
    write(
        tmpdir / ".clang-format-ignore",
        "# generated data headers\ninclude/generated_blob.h\ninclude/FreeSans*.h\n",
    )
    write(tmpdir / "src" / "clean.cpp", CLEAN_SOURCE)
    write(tmpdir / "src" / "drifted.cpp", DRIFTED_SOURCE)
    write(tmpdir / "include" / "generated_blob.h", GENERATED_HEADER)
    write(
        tmpdir / "test" / "contracts" / "format_clean_manifest.txt",
        "# ratchet manifest\n" + "".join(f"{line}\n" for line in manifest_lines),
    )
    return tmpdir


def run_checker(root: Path, extra_args: list[str] | None = None) -> tuple[int, str]:
    stdout = io.StringIO()
    stderr = io.StringIO()
    with redirect_stdout(stdout), redirect_stderr(stderr):
        rc = checker.main(["--root", str(root), *(extra_args or [])])
    return rc, stdout.getvalue() + stderr.getvalue()


def test_clean_manifest_passes(tmpdir: Path) -> None:
    root = make_root(tmpdir, ["src/clean.cpp"])

    rc, output = run_checker(root)

    assert_equal(rc, 0, "a manifest of formatted files should pass")
    assert_in("ratchet clean", output, "clean run should say so")


def test_drift_in_a_manifest_file_fails(tmpdir: Path) -> None:
    root = make_root(tmpdir, ["src/clean.cpp", "src/drifted.cpp"])

    rc, output = run_checker(root)

    assert_equal(rc, 1, "drift in a ratcheted file must fail")
    assert_in("drifted from clang-format output", output, "drift should be named")
    assert_in("src/drifted.cpp", output, "the drifted path should be listed")


def test_drift_outside_the_manifest_still_passes(tmpdir: Path) -> None:
    """The ratchet must not gate files that have not been migrated yet."""
    root = make_root(tmpdir, ["src/clean.cpp"])

    rc, output = run_checker(root)

    assert_equal(rc, 0, "unratcheted drift must not fail the gate")
    assert_in("1 not ratcheted yet", output, "unratcheted files should be counted")


def test_clean_unratcheted_file_is_advertised(tmpdir: Path) -> None:
    """This hint is how the ratchet advances."""
    root = make_root(tmpdir, [])

    rc, output = run_checker(root)

    assert_equal(rc, 0, "an empty manifest gates nothing and must pass")
    assert_in("ALREADY clang-format clean", output, "clean files should be advertised")
    assert_in("+ src/clean.cpp", output, "the clean file should be offered for the manifest")
    if "+ src/drifted.cpp" in output:
        raise AssertionError("a drifted file must never be advertised as ready")


def test_missing_manifest_entry_fails(tmpdir: Path) -> None:
    """A renamed or deleted file must not silently drop out of the ratchet."""
    root = make_root(tmpdir, ["src/clean.cpp", "src/renamed_away.cpp"])

    rc, output = run_checker(root)

    assert_equal(rc, 1, "a manifest entry with no file must fail")
    assert_in("do not exist", output, "missing entries should be named")
    assert_in("src/renamed_away.cpp", output, "the missing path should be listed")


def test_generated_manifest_entry_fails(tmpdir: Path) -> None:
    """Excluded files are skipped by clang-format and would pass vacuously."""
    root = make_root(tmpdir, ["include/generated_blob.h", "src/clean.cpp"])

    rc, output = run_checker(root)

    assert_equal(rc, 1, "an excluded/generated manifest entry must fail")
    assert_in("excluded by .clang-format-ignore", output, "the exclusion should be explained")
    assert_in("include/generated_blob.h", output, "the excluded path should be listed")


def test_generated_file_is_not_advertised(tmpdir: Path) -> None:
    root = make_root(tmpdir, ["src/clean.cpp"])

    _, output = run_checker(root)

    if "generated_blob.h" in output:
        raise AssertionError("excluded generated headers must never be offered for the manifest")


def test_unsorted_and_duplicate_manifest_rows_fail(tmpdir: Path) -> None:
    root = make_root(tmpdir, ["src/clean.cpp", "include/generated_blob.h", "src/clean.cpp"])

    rc, output = run_checker(root)

    assert_equal(rc, 1, "an unsorted manifest with duplicates must fail")
    assert_in("not sorted", output, "sort violations should be named")
    assert_in("duplicate entry", output, "duplicate rows should be named")


def test_version_mismatch_fails(tmpdir: Path) -> None:
    """An unpinned formatter makes the gate nondeterministic across machines."""
    root = make_root(tmpdir, ["src/clean.cpp"])

    rc, output = run_checker(root, ["--require-version", "0.0.1"])

    assert_equal(rc, 1, "a formatter version mismatch must fail")
    assert_in("is not the pinned version 0.0.1", output, "the mismatch should be explained")


def test_missing_formatter_fails(tmpdir: Path) -> None:
    root = make_root(tmpdir, ["src/clean.cpp"])

    rc, output = run_checker(root, ["--clang-format", str(root / "no-such-clang-format")])

    assert_equal(rc, 1, "a missing formatter must fail closed")
    assert_in("failed to execute", output, "the missing binary should be reported")


def test_real_manifest_is_sorted_and_populated() -> None:
    """Self-check: the committed manifest must parse, be sorted, and not be empty."""
    manifest, errors = checker.read_manifest(ROOT / checker.MANIFEST_RELPATH)

    assert_equal(errors, [], "the committed manifest must have no malformed rows")
    if not manifest:
        raise AssertionError("the committed manifest must not be empty")

    ignore_patterns = checker.read_ignore_patterns(ROOT / ".clang-format-ignore")
    for rel_path in manifest:
        if not (ROOT / rel_path).is_file():
            raise AssertionError(f"committed manifest entry does not exist: {rel_path}")
        if checker.is_excluded(rel_path, ignore_patterns):
            raise AssertionError(f"committed manifest entry is excluded/generated: {rel_path}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="clang_format_check_") as tmp:
        tmpdir = Path(tmp)
        test_clean_manifest_passes(tmpdir / "clean_pass")
        test_drift_in_a_manifest_file_fails(tmpdir / "drift_fail")
        test_drift_outside_the_manifest_still_passes(tmpdir / "unratcheted_drift")
        test_clean_unratcheted_file_is_advertised(tmpdir / "ratchet_hint")
        test_missing_manifest_entry_fails(tmpdir / "missing_entry")
        test_generated_manifest_entry_fails(tmpdir / "generated_entry")
        test_generated_file_is_not_advertised(tmpdir / "generated_hint")
        test_unsorted_and_duplicate_manifest_rows_fail(tmpdir / "malformed_manifest")
        test_version_mismatch_fails(tmpdir / "version_mismatch")
        test_missing_formatter_fails(tmpdir / "missing_formatter")
        test_real_manifest_is_sorted_and_populated()

    print("[format] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
