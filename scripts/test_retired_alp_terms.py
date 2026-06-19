#!/usr/bin/env python3
"""Regression tests for scripts/check_retired_alp_terms.py."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_retired_alp_terms as retired  # type: ignore  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def scan_text(tmpdir: Path, text: str) -> list[tuple[int, str, str]]:
    source = tmpdir / "sample.cpp"
    source.write_text(text, encoding="utf-8")
    return retired.scan_file(source)


def test_alp_context_retired_word_is_flagged(tmpdir: Path) -> None:
    hits = scan_text(tmpdir, "// ALP indicator shows armed state\n")
    assert_true(len(hits) == 1, "ALP-context retired vocabulary must be blocked")


def test_non_alp_scan_language_is_allowed(tmpdir: Path) -> None:
    hits = scan_text(tmpdir, "// WiFi scan and BLE scan are legitimate non-ALP concepts\n")
    assert_true(len(hits) == 0, "non-ALP scan terminology should remain allowed")


def test_power_auto_arm_language_is_allowed(tmpdir: Path) -> None:
    hits = scan_text(tmpdir, "Serial.println(\"[AutoPowerOff] Armed - ALP heartbeat received\");\n")
    assert_true(len(hits) == 0, "auto-power arming is not ALP mode terminology")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="retired_alp_terms_") as tmp:
        tmpdir = Path(tmp)
        test_alp_context_retired_word_is_flagged(tmpdir)
        test_non_alp_scan_language_is_allowed(tmpdir)
        test_power_auto_arm_language_is_allowed(tmpdir)

    print("[retired-alp-terms] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
