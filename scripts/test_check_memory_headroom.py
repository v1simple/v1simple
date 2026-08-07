#!/usr/bin/env python3
"""Regression tests for scripts/check_memory_headroom.py."""

from __future__ import annotations

import json
from pathlib import Path
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_memory_headroom as checker  # type: ignore  # noqa: E402


def assert_equal(actual: Any, expected: Any, message: str) -> None:
    if actual != expected:
        raise AssertionError(f"{message}: expected {expected!r}, got {actual!r}")


def sample_report(*, diram_free: int = 173_471) -> str:
    diram_total = 341_760
    return json.dumps(
        {
            "version": "1.2",
            "layout": [
                {"name": "Flash Code", "total": 0, "used": 1_523_288, "free": 0},
                {
                    "name": "DIRAM",
                    "total": diram_total,
                    "used": diram_total - diram_free,
                    "free": diram_free,
                },
                {"name": "IRAM", "total": 16_384, "used": 16_384, "free": 0},
            ],
        }
    )


def test_structured_memory_preserves_exclusive_and_shared_pools() -> None:
    memory = checker.parse_esp_idf_size_memory(sample_report())

    assert_equal(
        memory["iram"],
        {"used_bytes": 16_384, "limit_bytes": 16_384, "headroom_bytes": 0},
        "exclusive IRAM row",
    )
    assert_equal(
        memory["diram"],
        {"used_bytes": 168_289, "limit_bytes": 341_760, "headroom_bytes": 173_471},
        "shared DIRAM row",
    )


def test_full_exclusive_iram_is_informational() -> None:
    memory = checker.parse_esp_idf_size_memory(sample_report())
    infos, warnings, errors = checker.evaluate_headroom(
        memory,
        warn_diram_zero=True,
        fail_diram_zero=False,
    )

    assert_equal(len(infos), 1, "full exclusive IRAM should emit one information message")
    assert_equal(warnings, [], "full exclusive IRAM should not warn")
    assert_equal(errors, [], "full exclusive IRAM should not fail")


def test_zero_shared_diram_obeys_warning_and_failure_policy() -> None:
    memory = checker.parse_esp_idf_size_memory(sample_report(diram_free=0))
    _, warnings, errors = checker.evaluate_headroom(
        memory,
        warn_diram_zero=True,
        fail_diram_zero=False,
    )
    assert_equal(warnings, ["shared DIRAM has zero headroom"], "shared DIRAM warning")
    assert_equal(errors, [], "warning policy should not fail")

    _, warnings, errors = checker.evaluate_headroom(
        memory,
        warn_diram_zero=True,
        fail_diram_zero=True,
    )
    assert_equal(warnings, [], "failure policy should supersede warning")
    assert_equal(errors, ["shared DIRAM has zero headroom"], "shared DIRAM failure")


def test_missing_shared_pool_fails_closed() -> None:
    report = json.loads(sample_report())
    report["layout"] = [entry for entry in report["layout"] if entry["name"] != "DIRAM"]
    try:
        checker.parse_esp_idf_size_memory(json.dumps(report))
    except ValueError as exc:
        if "diram" not in str(exc):
            raise AssertionError(f"missing DIRAM error should identify the row: {exc}") from exc
    else:
        raise AssertionError("missing DIRAM row should fail closed")


def test_non_object_report_fails_closed() -> None:
    try:
        checker.parse_esp_idf_size_memory("[]")
    except ValueError as exc:
        if "root" not in str(exc):
            raise AssertionError(f"invalid-root error should identify the root: {exc}") from exc
    else:
        raise AssertionError("non-object report should fail closed")


def main() -> int:
    test_structured_memory_preserves_exclusive_and_shared_pools()
    test_full_exclusive_iram_is_informational()
    test_zero_shared_diram_obeys_warning_and_failure_policy()
    test_missing_shared_pool_fails_closed()
    test_non_object_report_fails_closed()
    print("[memory-headroom] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
