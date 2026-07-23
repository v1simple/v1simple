#!/usr/bin/env python3
"""Regression tests for the CI timing-budget and workflow-timeout contract."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile

import check_ci_budget as checker


def assert_raises(message: str, callback) -> None:
    try:
        callback()
    except ValueError:
        return
    raise AssertionError(message)


def write_contract(root: Path, max_seconds: object, timeout: str) -> tuple[Path, Path]:
    budgets = root / "ci_time_budgets.json"
    workflow = root / "ci.yml"
    budgets.write_text(
        json.dumps(
            {
                "budgets": {
                    "ci-test": {
                        "description": "test budget",
                        "max_seconds": max_seconds,
                    }
                }
            }
        ),
        encoding="utf-8",
    )
    workflow.write_text(
        "jobs:\n"
        "  test:\n"
        f"    timeout-minutes: {timeout}\n",
        encoding="utf-8",
    )
    return budgets, workflow


def test_repository_contract_passes() -> None:
    budgets, timeout_minutes = checker.read_contract()
    assert budgets["ci-test"]["max_seconds"] == 1800
    assert timeout_minutes == 36


def test_timeout_covers_setup_and_five_minutes_of_headroom() -> None:
    with tempfile.TemporaryDirectory(prefix="ci-budget-") as raw:
        budgets, workflow = write_contract(Path(raw), 1800, "35")
        assert_raises(
            "workflow timeout without setup allowance and five-minute headroom was accepted",
            lambda: checker.read_contract(budgets, workflow),
        )

        workflow.write_text(
            workflow.read_text(encoding="utf-8").replace("35", "36"),
            encoding="utf-8",
        )
        loaded, timeout_minutes = checker.read_contract(budgets, workflow)
        assert loaded["ci-test"]["max_seconds"] == 1800
        assert timeout_minutes == 36


def test_timeout_is_scoped_to_authoritative_job() -> None:
    with tempfile.TemporaryDirectory(prefix="ci-budget-") as raw:
        budgets, workflow = write_contract(Path(raw), 1800, "36")
        workflow.write_text(
            workflow.read_text(encoding="utf-8")
            + "  coverage:\n"
            + "    timeout-minutes: 45\n",
            encoding="utf-8",
        )
        _, timeout_minutes = checker.read_contract(budgets, workflow)
        assert timeout_minutes == 36

        workflow.write_text(
            "jobs:\n"
            "  test:\n"
            "    runs-on: ubuntu-latest\n"
            "  coverage:\n"
            "    timeout-minutes: 45\n",
            encoding="utf-8",
        )
        assert_raises(
            "unrelated job timeout substituted for authoritative timeout",
            lambda: checker.read_contract(budgets, workflow),
        )


def test_authoritative_timeout_must_be_single_positive_integer() -> None:
    with tempfile.TemporaryDirectory(prefix="ci-budget-") as raw:
        budgets, workflow = write_contract(Path(raw), 1800, "36")
        workflow.write_text(
            workflow.read_text(encoding="utf-8") + "    timeout-minutes: 40\n",
            encoding="utf-8",
        )
        assert_raises(
            "duplicate authoritative workflow timeout was accepted",
            lambda: checker.read_contract(budgets, workflow),
        )

        workflow.write_text(
            "jobs:\n  test:\n    timeout-minutes: unlimited\n",
            encoding="utf-8",
        )
        assert_raises(
            "non-integer workflow timeout was accepted",
            lambda: checker.read_contract(budgets, workflow),
        )

        workflow.write_text(
            "jobs:\n  test:\n    timeout-minutes: 0\n",
            encoding="utf-8",
        )
        assert_raises(
            "zero workflow timeout was accepted",
            lambda: checker.read_contract(budgets, workflow),
        )


def test_budget_must_be_positive_integer() -> None:
    for invalid in (True, 0, -1, 1800.0, "1800"):
        with tempfile.TemporaryDirectory(prefix="ci-budget-") as raw:
            budgets, workflow = write_contract(Path(raw), invalid, "35")
            assert_raises(
                f"invalid budget was accepted: {invalid!r}",
                lambda: checker.read_contract(budgets, workflow),
            )


def test_elapsed_seconds_are_typed_and_budget_boundary_is_inclusive() -> None:
    with tempfile.TemporaryDirectory(prefix="ci-budget-") as raw:
        timing = Path(raw) / "timing.json"
        budgets = {"ci-test": {"max_seconds": 1800}}
        timing.write_text('{"elapsed_seconds": 1800}', encoding="utf-8")
        elapsed = checker.read_elapsed_seconds(timing)
        assert checker.within_budget(budgets, "ci-test", elapsed)

        timing.write_text('{"elapsed_seconds": 1801}', encoding="utf-8")
        elapsed = checker.read_elapsed_seconds(timing)
        assert not checker.within_budget(budgets, "ci-test", elapsed)

        for invalid in (True, -1, 1800.0, "1800"):
            timing.write_text(
                json.dumps({"elapsed_seconds": invalid}),
                encoding="utf-8",
            )
            assert_raises(
                f"invalid elapsed time was accepted: {invalid!r}",
                lambda: checker.read_elapsed_seconds(timing),
            )


def main() -> int:
    tests = (
        test_repository_contract_passes,
        test_timeout_covers_setup_and_five_minutes_of_headroom,
        test_timeout_is_scoped_to_authoritative_job,
        test_authoritative_timeout_must_be_single_positive_integer,
        test_budget_must_be_positive_integer,
        test_elapsed_seconds_are_typed_and_budget_boundary_is_inclusive,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} CI timing budget regression tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
