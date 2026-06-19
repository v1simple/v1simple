#!/usr/bin/env python3
"""Ensure tracked V1 protocol comments cite tracked protocol summaries.

Official Valentine Research PDFs are intentionally not tracked. Source, tests,
and general docs must not point readers at local scratch paths as the primary
evidence chain; they should cite docs/V1_PROTOCOL_REFERENCES.md by anchor.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "docs" / "V1_PROTOCOL_REFERENCES.md"
SCOPES = ("README.md", "src", "include", "test", "docs")
EXCLUDED_DOC_PREFIXES = ("docs/reviews/",)
ALLOWED_LOCAL_REF_FILES = {"docs/V1_PROTOCOL_REFERENCES.md"}
FORBIDDEN_LOCAL_REFS = (
    ".scratch/AndroidESPLibrary2/Specification",
    "AndroidESPLibrary2/Specification/",
)
REQUIRED_REFERENCE_TOKENS = (
    "# V1 Protocol References",
    "## Packet framing",
    "## Packet IDs and volume responses",
    "## Version response",
    "## `infDisplayData`",
    "## Alert rows",
    "## User settings bytes",
    "## BLE GATT proxy surface",
)


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

    if not REFERENCE.is_file():
        errors.append("docs/V1_PROTOCOL_REFERENCES.md is required")
    else:
        reference_text = REFERENCE.read_text(encoding="utf-8")
        for token in REQUIRED_REFERENCE_TOKENS:
            if token not in reference_text:
                errors.append(f"docs/V1_PROTOCOL_REFERENCES.md missing required section: {token}")

    for path in tracked_files():
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT).as_posix()
        if rel in ALLOWED_LOCAL_REF_FILES:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if any(token in line for token in FORBIDDEN_LOCAL_REFS):
                errors.append(f"{rel}:{lineno}: cites local-only V1 protocol PDFs")

    if errors:
        print("[contract] V1 protocol docs contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] tracked V1 protocol comments cite tracked protocol references")
    return 0


if __name__ == "__main__":
    sys.exit(main())
