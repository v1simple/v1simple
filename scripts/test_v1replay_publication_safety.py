#!/usr/bin/env python3
"""Regression tests for v1replay's filesystem publication guard."""

from __future__ import annotations

from contextlib import redirect_stdout
import importlib.util
from io import StringIO
from pathlib import Path
import sys
import tempfile
from types import ModuleType


ROOT = Path(__file__).resolve().parents[1]
GUARD_PATH = ROOT / "tools" / "v1replay" / "verify" / "check_publication_safety.py"
sys.dont_write_bytecode = True


def load_guard() -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        "v1replay_publication_guard_under_test",
        GUARD_PATH,
    )
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load publication guard: {GUARD_PATH}")
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)
    return guard


GUARD = load_guard()


def run_guard(relative: str, contents: str = "safe fixture\n") -> tuple[int, str]:
    with tempfile.TemporaryDirectory(prefix="v1replay-publication-") as raw:
        replay_root = Path(raw)
        subject = replay_root / relative
        subject.parent.mkdir(parents=True, exist_ok=True)
        subject.write_text(contents, encoding="utf-8")

        GUARD.ROOT = replay_root
        output = StringIO()
        with redirect_stdout(output):
            status = GUARD.main()
        return status, output.getvalue()


def test_direct_swift_test_is_publishable() -> None:
    status, output = run_guard(
        "Tests/v1replayTests/V1ProtocolContractTests.swift",
        "import XCTest\n",
    )
    assert status == 0, output
    assert "publication safety: OK" in output


def test_nested_swift_test_is_rejected() -> None:
    status, output = run_guard(
        "Tests/v1replayTests/Nested/ContractTests.swift"
    )
    assert status == 1, output
    assert "outside the source-only allowlist" in output


def test_non_swift_test_is_rejected() -> None:
    status, output = run_guard("Tests/v1replayTests/ContractTests.txt")
    assert status == 1, output
    assert "outside the source-only allowlist" in output


def test_private_directory_is_rejected() -> None:
    status, output = run_guard("captures/ContractTests.swift")
    assert status == 1, output
    assert "private-data directory is not publishable" in output


def test_data_suffix_is_rejected() -> None:
    status, output = run_guard("Tests/v1replayTests/ContractTests.json")
    assert status == 1, output
    assert "data or media files are not publishable" in output


def test_filesystem_scan_rejects_file_without_git_tracking() -> None:
    # The temporary root has no Git metadata. Detection therefore proves the
    # local guard scans filesystem contents rather than only tracked files.
    status, output = run_guard("untracked-note.txt")
    assert status == 1, output
    assert "outside the source-only allowlist" in output


def main() -> int:
    tests = (
        test_direct_swift_test_is_publishable,
        test_nested_swift_test_is_rejected,
        test_non_swift_test_is_rejected,
        test_private_directory_is_rejected,
        test_data_suffix_is_rejected,
        test_filesystem_scan_rejects_file_without_git_tracking,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} v1replay publication guard regression tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
