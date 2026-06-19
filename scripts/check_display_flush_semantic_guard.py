#!/usr/bin/env python3
"""Check display-flush semantic rules without snapshot coupling."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_display_flush_discipline_contract as contract  # type: ignore  # noqa: E402


def main() -> int:
    allocation_violations = contract.extract_allocation_violations()
    if allocation_violations:
        print("[guard] display flush semantic violations detected")
        for row in allocation_violations:
            print(f"  - {row}")
        return 1

    print("[guard] display flush semantic guard matches (0 allocation violations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
