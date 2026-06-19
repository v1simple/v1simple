#!/usr/bin/env python3
"""Ensure tracked source/tests cite tracked ALP protocol evidence."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN = ("ALP_PROTOCOL_REFERENCE", "ALP_MANUAL_EXTRACTS")
SCOPES = ("README.md", "src", "include", "test", "docs")
EXCLUDED_DOC_PREFIXES = ("docs/REPO_REVIEW_",)


def tracked_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", *SCOPES],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    files: list[Path] = []
    for line in result.stdout.splitlines():
        if any(line.startswith(prefix) for prefix in EXCLUDED_DOC_PREFIXES):
            continue
        files.append(ROOT / line)
    return files


def main() -> int:
    errors: list[str] = []

    evidence = ROOT / "docs" / "ALP_PROTOCOL_EVIDENCE.md"
    if not evidence.is_file():
        errors.append("docs/ALP_PROTOCOL_EVIDENCE.md is required")

    for path in tracked_files():
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if any(token in line for token in FORBIDDEN):
                rel = path.relative_to(ROOT)
                errors.append(f"{rel}:{lineno}: cites local-only ALP notes")

    if errors:
        print("[contract] ALP protocol docs contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] tracked ALP comments cite tracked protocol evidence")
    return 0


if __name__ == "__main__":
    sys.exit(main())
