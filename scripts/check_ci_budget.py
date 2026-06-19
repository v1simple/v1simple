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


def read_budgets() -> dict:
    if not BUDGETS_PATH.exists():
        raise FileNotFoundError(f"Budget file not found: {BUDGETS_PATH}")
    with open(BUDGETS_PATH) as f:
        data = json.load(f)
    budgets = data.get("budgets")
    if not isinstance(budgets, dict) or not budgets:
        raise ValueError("Budget file missing non-empty 'budgets' object.")
    return budgets


def self_check() -> int:
    try:
        budgets = read_budgets()
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"[budget] self-check failed: {exc}", file=sys.stderr)
        return 2

    if "ci-test" not in budgets:
        print("[budget] self-check failed: missing required 'ci-test' lane", file=sys.stderr)
        return 2

    print(f"[budget] self-check OK: {len(budgets)} lane(s) loaded")
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
        budgets = read_budgets()
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if lane not in budgets:
        print(f"Unknown lane '{lane}'. Valid: {', '.join(budgets)}", file=sys.stderr)
        return 2

    if not timing_path.exists():
        print(f"Timing file not found: {timing_path}", file=sys.stderr)
        return 2

    with open(timing_path) as f:
        timing = json.load(f)

    elapsed = timing.get("elapsed_seconds")
    if elapsed is None:
        print("Timing JSON missing 'elapsed_seconds' key.", file=sys.stderr)
        return 2

    max_seconds = budgets[lane]["max_seconds"]
    if elapsed > max_seconds:
        print(
            f"OVER BUDGET: {lane} took {elapsed}s, budget is {max_seconds}s "
            f"({elapsed - max_seconds}s over)"
        )
        return 1

    print(f"Within budget: {lane} took {elapsed}s / {max_seconds}s limit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
