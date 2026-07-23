#!/usr/bin/env python3
"""Check CI lane elapsed time against budgets defined in tools/ci_time_budgets.json.

Usage:
    python3 scripts/check_ci_budget.py <lane> <timing-json>

Where <lane> is one of the keys in tools/ci_time_budgets.json.
and <timing-json> is a path to a JSON file with {"elapsed_seconds": N}.

Exit codes:
    0 = within budget
    1 = over budget
    2 = usage / input error
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUDGETS_PATH = ROOT / "tools" / "ci_time_budgets.json"
CI_WORKFLOW_PATH = ROOT / ".github" / "workflows" / "ci.yml"
CI_WORKFLOW_HEADROOM_SECONDS = 300
CI_WORKFLOW_SETUP_ALLOWANCE_SECONDS = 60


def read_budgets(path: Path = BUDGETS_PATH) -> dict:
    if not path.exists():
        raise FileNotFoundError(f"Budget file not found: {path}")
    with open(path) as f:
        data = json.load(f)
    budgets = data.get("budgets")
    if not isinstance(budgets, dict) or not budgets:
        raise ValueError("Budget file missing non-empty 'budgets' object.")
    for lane, definition in budgets.items():
        if (
            not isinstance(lane, str)
            or not lane
            or not isinstance(definition, dict)
            or not isinstance(definition.get("description"), str)
            or not definition["description"]
            or not isinstance(definition.get("max_seconds"), int)
            or isinstance(definition["max_seconds"], bool)
            or definition["max_seconds"] <= 0
        ):
            raise ValueError(f"Budget lane '{lane}' has an invalid definition.")
    return budgets


def read_ci_timeout_minutes(path: Path = CI_WORKFLOW_PATH) -> int:
    try:
        workflow = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"CI workflow is unavailable: {type(exc).__name__}") from exc
    in_jobs = False
    in_authoritative_job = False
    values: list[str] = []
    for raw_line in workflow.splitlines():
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indentation = len(raw_line) - len(raw_line.lstrip(" "))
        if indentation == 0:
            in_jobs = stripped == "jobs:"
            in_authoritative_job = False
            continue
        if not in_jobs:
            continue
        if indentation == 2 and stripped.endswith(":"):
            in_authoritative_job = stripped == "test:"
            continue
        if (
            in_authoritative_job
            and indentation == 4
            and stripped.startswith("timeout-minutes:")
        ):
            values.append(stripped.partition(":")[2].strip())
    if len(values) != 1 or not values[0].isdigit():
        raise ValueError(
            "CI workflow jobs.test must declare exactly one integer timeout-minutes value."
        )
    timeout_minutes = int(values[0])
    if timeout_minutes <= 0:
        raise ValueError("CI workflow jobs.test timeout-minutes must be positive.")
    return timeout_minutes


def read_contract(
    budgets_path: Path = BUDGETS_PATH,
    workflow_path: Path = CI_WORKFLOW_PATH,
) -> tuple[dict, int]:
    budgets = read_budgets(budgets_path)
    if "ci-test" not in budgets:
        raise ValueError("Budget file missing required 'ci-test' lane.")
    timeout_minutes = read_ci_timeout_minutes(workflow_path)
    required_timeout_seconds = (
        budgets["ci-test"]["max_seconds"]
        + CI_WORKFLOW_SETUP_ALLOWANCE_SECONDS
        + CI_WORKFLOW_HEADROOM_SECONDS
    )
    if timeout_minutes * 60 < required_timeout_seconds:
        raise ValueError(
            "CI workflow timeout must cover the ci-test budget, setup allowance, "
            f"and {CI_WORKFLOW_HEADROOM_SECONDS}s headroom."
        )
    return budgets, timeout_minutes


def read_elapsed_seconds(path: Path) -> int:
    if not path.exists():
        raise FileNotFoundError(f"Timing file not found: {path}")
    with open(path) as f:
        timing = json.load(f)
    elapsed = timing.get("elapsed_seconds")
    if (
        not isinstance(elapsed, int)
        or isinstance(elapsed, bool)
        or elapsed < 0
    ):
        raise ValueError("Timing JSON must contain a non-negative integer elapsed_seconds.")
    return elapsed


def within_budget(budgets: dict, lane: str, elapsed_seconds: int) -> bool:
    return elapsed_seconds <= budgets[lane]["max_seconds"]


def self_check() -> int:
    try:
        budgets, timeout_minutes = read_contract()
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"[budget] self-check failed: {exc}", file=sys.stderr)
        return 2

    print(
        f"[budget] self-check OK: {len(budgets)} lane(s) loaded; "
        f"CI timeout={timeout_minutes}m"
    )
    return 0


def main() -> int:
    if len(sys.argv) == 1:
        return self_check()

    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <lane> <timing-json>", file=sys.stderr)
        return 2

    lane = sys.argv[1]
    timing_path = Path(sys.argv[2])

    try:
        budgets, _ = read_contract()
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if lane not in budgets:
        print(f"Unknown lane '{lane}'. Valid: {', '.join(budgets)}", file=sys.stderr)
        return 2

    try:
        elapsed = read_elapsed_seconds(timing_path)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 2

    max_seconds = budgets[lane]["max_seconds"]
    if not within_budget(budgets, lane, elapsed):
        print(
            f"OVER BUDGET: {lane} took {elapsed}s, budget is {max_seconds}s "
            f"({elapsed - max_seconds}s over)"
        )
        return 1

    print(f"Within budget: {lane} took {elapsed}s / {max_seconds}s limit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
