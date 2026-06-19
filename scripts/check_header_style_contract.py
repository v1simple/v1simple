#!/usr/bin/env python3
"""Check production header style.

Rules:
1) Every production header under src/ and include/ must use #pragma once.
2) File-level include guards are forbidden unless they are explicitly listed as
   a temporary native-test mock-shadow seam in
   test/contracts/header_style_mock_shadow_allowlist.txt.
3) Every mock-shadow exception must still be backed by a test/mocks header that
   defines the same guard macro, otherwise the exception is stale.

The remaining mock-shadow guards are technical debt: native tests currently use
same-name mock headers to suppress production headers when source files are
compiled through host shims. New production headers should be pragma-only.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST = ROOT / "test" / "contracts" / "header_style_mock_shadow_allowlist.txt"
PRODUCTION_DIRS = (ROOT / "src", ROOT / "include")
MOCK_DIR = ROOT / "test" / "mocks"
HEADER_SUFFIXES = {".h", ".hpp"}
MAX_MOCK_SHADOW_GUARDS = 10

PRAGMA_RE = re.compile(r"^\s*#\s*pragma\s+once\b", re.MULTILINE)
FILE_GUARD_RE = re.compile(
    r"^\s*#ifndef\s+(?P<macro>[A-Z][A-Z0-9_]*_H)\b\s*\n"
    r"\s*#define\s+(?P=macro)\b",
    re.MULTILINE,
)
ALLOW_RE = re.compile(r"^file=(?P<file>\S+)\s+macro=(?P<macro>[A-Z][A-Z0-9_]*_H)\b")


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def production_headers() -> list[Path]:
    out: list[Path] = []
    for root in PRODUCTION_DIRS:
        if root.exists():
            out.extend(p for p in root.rglob("*") if p.is_file() and p.suffix in HEADER_SUFFIXES)
    return sorted(out)


def read_allowlist() -> set[tuple[str, str]]:
    allowed: set[tuple[str, str]] = set()
    if not ALLOWLIST.exists():
        return allowed
    for line_no, raw in enumerate(ALLOWLIST.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = ALLOW_RE.match(line)
        if not match:
            raise ValueError(f"{rel(ALLOWLIST)}:{line_no}: malformed allowlist row: {raw}")
        allowed.add((match.group("file"), match.group("macro")))
    return allowed


def mock_guard_macros() -> set[str]:
    macros: set[str] = set()
    if not MOCK_DIR.exists():
        return macros
    for path in MOCK_DIR.rglob("*.h"):
        text = path.read_text(encoding="utf-8", errors="replace")
        macros.update(match.group("macro") for match in FILE_GUARD_RE.finditer(text))
    return macros


def main() -> int:
    try:
        allowed = read_allowlist()
    except ValueError as exc:
        print(f"[contract] header-style allowlist error: {exc}")
        return 1

    mock_macros = mock_guard_macros()
    actual_guards: set[tuple[str, str]] = set()
    missing_pragma: list[str] = []
    disallowed_guards: list[tuple[str, str]] = []
    stale_allowlist: list[tuple[str, str]] = []
    unbacked_allowlist: list[tuple[str, str]] = []
    guard_budget_errors: list[str] = []

    for path in production_headers():
        text = path.read_text(encoding="utf-8", errors="replace")
        rel_path = rel(path)
        if not PRAGMA_RE.search(text):
            missing_pragma.append(rel_path)
        for match in FILE_GUARD_RE.finditer(text):
            item = (rel_path, match.group("macro"))
            actual_guards.add(item)
            if item not in allowed:
                disallowed_guards.append(item)

    for item in sorted(allowed - actual_guards):
        stale_allowlist.append(item)
    for item in sorted(allowed):
        if item[1] not in mock_macros:
            unbacked_allowlist.append(item)

    if len(allowed) > MAX_MOCK_SHADOW_GUARDS:
        guard_budget_errors.append(
            f"allowlist has {len(allowed)} rows; maximum is {MAX_MOCK_SHADOW_GUARDS}"
        )
    if len(actual_guards) > MAX_MOCK_SHADOW_GUARDS:
        guard_budget_errors.append(
            f"production has {len(actual_guards)} guard exceptions; maximum is {MAX_MOCK_SHADOW_GUARDS}"
        )

    if missing_pragma or disallowed_guards or stale_allowlist or unbacked_allowlist or guard_budget_errors:
        if missing_pragma:
            print("[contract] production headers missing #pragma once:")
            for path in missing_pragma:
                print(f"  - {path}")
        if disallowed_guards:
            print("[contract] disallowed production file include guards:")
            for path, macro in sorted(disallowed_guards):
                print(f"  - {path}: {macro}")
        if stale_allowlist:
            print("[contract] stale header-style allowlist rows:")
            for path, macro in stale_allowlist:
                print(f"  - {path}: {macro}")
        if unbacked_allowlist:
            print("[contract] mock-shadow allowlist rows without matching test/mocks guard:")
            for path, macro in unbacked_allowlist:
                print(f"  - {path}: {macro}")
        if guard_budget_errors:
            print("[contract] mock-shadow guard budget exceeded:")
            for error in guard_budget_errors:
                print(f"  - {error}")
        print("\nUse #pragma once for new production headers. Remove mock-shadow exceptions as native tests are migrated.")
        return 1

    print(
        "[contract] header-style contract matches "
        f"({len(production_headers())} production headers, {len(actual_guards)} mock-shadow guard exceptions, "
        f"budget {MAX_MOCK_SHADOW_GUARDS})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
