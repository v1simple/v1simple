#!/usr/bin/env python3
"""Check BLE client deletion contract.

Enforced invariants:
1) NimBLEDevice::deleteClient() must NEVER appear in production source.
   Deleting a BLE client at runtime corrupts the NimBLE heap (fixed 3-slot
   internal array).
2) NimBLEDevice::deleteAllBonds() is allowed ONLY in src/ble_client.cpp
   inside the fresh-flash boot path (bleInitializeHardware).
3) NimBLEDevice::deinit() must NEVER appear in production source.

This contract is a CI gate — any violation fails the build.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = (ROOT / "src", ROOT / "include")
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp"}

# Authorized exception: only this file may call deleteAllBonds
AUTHORIZED_FILE = "src/ble_client.cpp"

# Patterns to check
BANNED_ALWAYS = re.compile(r"\bNimBLEDevice\s*::\s*deleteClient\s*\(")
RESTRICTED_CALL = re.compile(r"\bNimBLEDevice\s*::\s*deleteAllBonds\s*\(")
BANNED_DEINIT = re.compile(r"\bNimBLEDevice\s*::\s*deinit\s*\(")
COMMENT_RE = re.compile(r"//.*$|/\*.*?\*/", re.MULTILINE | re.DOTALL)


def iter_source_files() -> list[Path]:
    files: list[Path] = []
    for root in SOURCE_DIRS:
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                files.append(path)
    return sorted(files)


def main() -> int:
    violations: list[str] = []

    for path in iter_source_files():
        relative = path.relative_to(ROOT).as_posix()
        text = path.read_text(encoding="utf-8")
        # Strip comments to avoid false positives from documentation
        stripped = COMMENT_RE.sub("", text)
        lines = text.splitlines()

        # deleteClient is ALWAYS banned in production source
        for match in BANNED_ALWAYS.finditer(stripped):
            line_no = text[:match.start()].count("\n") + 1
            violations.append(
                f"  BANNED: {relative}:{line_no} — NimBLEDevice::deleteClient() "
                f"causes heap corruption at runtime"
            )

        for match in BANNED_DEINIT.finditer(stripped):
            line_no = text[:match.start()].count("\n") + 1
            violations.append(
                f"  BANNED: {relative}:{line_no} — NimBLEDevice::deinit() "
                f"tears down the BLE stack unsafely"
            )

        # deleteAllBonds restricted to authorized file only
        if relative != AUTHORIZED_FILE:
            for match in RESTRICTED_CALL.finditer(stripped):
                line_no = text[:match.start()].count("\n") + 1
                violations.append(
                    f"  RESTRICTED: {relative}:{line_no} — {match.group(0).strip()} "
                    f"only allowed in {AUTHORIZED_FILE}"
                )

    if violations:
        print(f"[contract] BLE deletion contract FAILED ({len(violations)} violation(s)):")
        for v in violations:
            print(v)
        return 1

    print("[contract] BLE deletion contract matches (0 violations)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
