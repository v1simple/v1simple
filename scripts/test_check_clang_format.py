#!/usr/bin/env python3
"""Regression tests for scripts/check_clang_format.py.

Every case builds a throwaway repo root (its own .clang-format and
.clang-format-ignore) and drives the checker against it, so the tests never
depend on the state of the real tree. A gate that cannot fail is not a gate:
the drift, version-mismatch, missing-formatter, and vacuous-scope paths are all
asserted to return 1.

The gate was a ratchet while the reformat landed in slices; it now globs src/
and include/, so the manifest cases are gone and replaced by the glob cases.
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


def make_root(tmpdir: Path, *, drifted: bool, ignore_everything: bool = False) -> Path:
    """Build a miniature repo: real .clang-format, a clean file, a generated header."""
    tmpdir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / ".clang-format", tmpdir / ".clang-format")
    patterns = "src/*\ninclude/*\n" if ignore_everything else "include/generated_blob.h\ninclude/FreeSans*.h\n"
    write(tmpdir / ".clang-format-ignore", "# exclusions\n" + patterns)
    write(tmpdir / "src" / "clean.cpp", CLEAN_SOURCE)
    if drifted:
        write(tmpdir / "src" / "drifted.cpp", DRIFTED_SOURCE)
    write(tmpdir / "include" / "generated_blob.h", GENERATED_HEADER)
    return tmpdir


def run_checker(root: Path, extra_args: list[str] | None = None) -> tuple[int, str]:
    stdout = io.StringIO()
    stderr = io.StringIO()
    with redirect_stdout(stdout), redirect_stderr(stderr):
        rc = checker.main(["--root", str(root), *(extra_args or [])])
    return rc, stdout.getvalue() + stderr.getvalue()


def test_clean_tree_passes(tmpdir: Path) -> None:
    root = make_root(tmpdir, drifted=False)

    rc, output = run_checker(root)

    assert_equal(rc, 0, "a fully formatted tree should pass")
    assert_in("clang-format clean", output, "clean run should say so")


def test_any_drift_fails(tmpdir: Path) -> None:
    """The whole point of the flip: drift anywhere in src/ now fails, no opt-in."""
    root = make_root(tmpdir, drifted=True)

    rc, output = run_checker(root)

    assert_equal(rc, 1, "drift anywhere under src/ must fail")
    assert_in("drifted from clang-format output", output, "drift should be named")
    assert_in("src/drifted.cpp", output, "the drifted path should be listed")


def test_generated_file_is_not_gated(tmpdir: Path) -> None:
    """The generated header is deliberately unformatted; it must not fail the gate."""
    root = make_root(tmpdir, drifted=False)

    rc, output = run_checker(root)

    assert_equal(rc, 0, "an excluded generated header must not fail the gate")
    if "generated_blob.h" in output:
        raise AssertionError("excluded generated headers must not be reported")


def test_excluding_everything_fails(tmpdir: Path) -> None:
    """A gate that checks nothing passes vacuously -- fail closed instead."""
    root = make_root(tmpdir, drifted=True, ignore_everything=True)

    rc, output = run_checker(root)

    assert_equal(rc, 1, "an exclusion list that swallows every file must fail")
    assert_in("no source files to gate", output, "the vacuous scope should be explained")


def test_version_mismatch_fails(tmpdir: Path) -> None:
    """An unpinned formatter makes the gate nondeterministic across machines."""
    root = make_root(tmpdir, drifted=False)

    rc, output = run_checker(root, ["--require-version", "0.0.1"])

    assert_equal(rc, 1, "a formatter version mismatch must fail")
    assert_in("is not the pinned version 0.0.1", output, "the mismatch should be explained")


def test_missing_formatter_fails(tmpdir: Path) -> None:
    root = make_root(tmpdir, drifted=False)

    rc, output = run_checker(root, ["--clang-format", str(root / "no-such-clang-format")])

    assert_equal(rc, 1, "a missing formatter must fail closed")
    assert_in("failed to execute", output, "the missing binary should be reported")


def test_missing_ignore_file_fails(tmpdir: Path) -> None:
    root = make_root(tmpdir, drifted=False)
    (root / ".clang-format-ignore").unlink()

    rc, output = run_checker(root)

    assert_equal(rc, 1, "a missing exclusion list must fail closed")
    assert_in("missing exclusion list", output, "the missing file should be reported")


def test_real_tree_is_in_scope() -> None:
    """Self-check: the committed tree must actually have files to gate."""
    ignore_patterns = checker.read_ignore_patterns(ROOT / ".clang-format-ignore")
    gated = [rel for rel in checker.source_files(ROOT) if not checker.is_excluded(rel, ignore_patterns)]
    if len(gated) < 100:
        raise AssertionError(f"expected the firmware tree to gate 100+ files, got {len(gated)}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="clang_format_check_") as tmp:
        tmpdir = Path(tmp)
        test_clean_tree_passes(tmpdir / "clean_pass")
        test_any_drift_fails(tmpdir / "drift_fail")
        test_generated_file_is_not_gated(tmpdir / "generated_skip")
        test_excluding_everything_fails(tmpdir / "vacuous_scope")
        test_version_mismatch_fails(tmpdir / "version_mismatch")
        test_missing_formatter_fails(tmpdir / "missing_formatter")
        test_missing_ignore_file_fails(tmpdir / "missing_ignore")
        test_real_tree_is_in_scope()

    print("[format] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
