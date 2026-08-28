#!/usr/bin/env python3
"""Ensure the required CI gate compiles the physical car-install target."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CI_GATE = ROOT / "scripts" / "ci-test.sh"
CI_WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"


def main() -> int:
    gate = CI_GATE.read_text(encoding="utf-8")
    workflow = CI_WORKFLOW.read_text(encoding="utf-8")
    required_build = (
        'run_step "Car install firmware build" "$PIO_CMD" run -e esp32-s3-car-install'
    )
    errors: list[str] = []
    if required_build not in gate:
        errors.append("scripts/ci-test.sh does not compile esp32-s3-car-install")
    if "bash ./scripts/ci-test.sh" not in workflow:
        errors.append(".github/workflows/ci.yml does not run the authoritative CI gate")
    if errors:
        for error in errors:
            print(f"[ci-car-build] {error}", file=sys.stderr)
        return 1
    print("[ci-car-build] required CI gate compiles esp32-s3-car-install")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
