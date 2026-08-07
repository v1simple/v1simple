#!/usr/bin/env python3
"""Require immutable full-SHA refs for external GitHub workflow actions."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS_DIR = ROOT / ".github" / "workflows"
USES_LINE_RE = re.compile(
    r"^\s*(?:-\s*)?uses\s*:\s*(?P<value>[^#]+?)(?:\s+#.*)?$"
)
USES_PREFIX_RE = re.compile(r"^(?:-\s*)?uses\s*:")
PINNED_ACTION_RE = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")


def workflow_files() -> list[Path]:
    return sorted((*WORKFLOWS_DIR.glob("*.yml"), *WORKFLOWS_DIR.glob("*.yaml")))


def parse_uses_value(raw: str) -> str:
    value = raw.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        value = value[1:-1].strip()
    return value


def check_workflow(path: Path) -> tuple[int, list[str]]:
    errors: list[str] = []
    uses_count = 0
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            continue
        if not USES_PREFIX_RE.match(stripped):
            continue

        match = USES_LINE_RE.fullmatch(line)
        if not match:
            errors.append(f"{path.relative_to(ROOT)}:{line_number}: could not parse uses entry")
            continue

        uses_count += 1
        value = parse_uses_value(match.group("value"))
        if value.startswith("./"):
            continue
        if not PINNED_ACTION_RE.fullmatch(value):
            errors.append(
                f"{path.relative_to(ROOT)}:{line_number}: external action must use a "
                f"40-character lowercase commit SHA: {value}"
            )
    return uses_count, errors


def main() -> int:
    paths = workflow_files()
    if not paths:
        print(f"[workflow-pins] no workflow files found under {WORKFLOWS_DIR}")
        return 1

    errors: list[str] = []
    total_uses = 0
    for path in paths:
        uses_count, path_errors = check_workflow(path)
        total_uses += uses_count
        errors.extend(path_errors)

    if errors:
        print("[workflow-pins] immutable action pin contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(
        f"[workflow-pins] {total_uses} action references pinned across "
        f"{len(paths)} workflow files"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
