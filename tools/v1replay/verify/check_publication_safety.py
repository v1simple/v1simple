#!/usr/bin/env python3
"""Enforce v1replay's source-only publication boundary."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IGNORED_PARTS = {".build", "__pycache__"}

EXACT_ALLOWED = {
    Path(".gitignore"),
    Path("LIGHTBLUE_CRIB.md"),
    Path("Package.swift"),
    Path("README.md"),
    Path("Resources/Info.plist"),
    Path("scripts/build.sh"),
    Path("verify/check_publication_safety.py"),
    Path("verify/verify_protocol.py"),
}

PRIVATE_SUFFIXES = {
    ".bin", ".btsnoop", ".csv", ".gz", ".heic", ".jpeg", ".jpg",
    ".json", ".jsonl", ".log", ".m4v", ".mov", ".mp4", ".pcap",
    ".pcapng", ".png", ".tsv", ".zip",
}
PRIVATE_DIRECTORY_NAMES = {"captures", "encounters", "exports", "fixtures", "recordings"}

# These patterns intentionally target high-confidence publication leaks rather
# than ordinary protocol terms. Components are split so the checker does not
# match its own pattern declarations.
TEXT_PATTERNS = {
    "macOS user path": re.compile("/" + "Users" + r"/[A-Za-z0-9._-]+/"),
    "Linux user path": re.compile("/" + "home" + r"/[A-Za-z0-9._-]+/"),
    "Windows user path": re.compile(r"[A-Za-z]:\\" + "Users" + r"\\[^\\]+\\"),
    "absolute capture timestamp": re.compile(
        r"\b20\d{2}[-_]\d{2}[-_]\d{2}[T_ ]\d{2}[:_]?\d{2}(?:[:_]?\d{2})?Z?\b"
    ),
    "email address": re.compile(r"\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b", re.I),
    "private key": re.compile("BEGIN " + "PRIVATE KEY"),
    "bearer token": re.compile("Bearer" + r"\s+[A-Za-z0-9._~-]{16,}"),
    "assigned secret": re.compile(
        r"(?i)\b(?:api[_-]?key|access[_-]?token|client[_-]?secret|password)\b\s*[:=]\s*['\"][^'\"]+"
    ),
}


def is_allowed(relative: Path) -> bool:
    if relative in EXACT_ALLOWED:
        return True
    return (
        len(relative.parts) == 3
        and relative.parts[:2] == ("Sources", "v1replay")
        and relative.suffix == ".swift"
    )


def iter_publishable_files():
    for path in sorted(ROOT.rglob("*")):
        relative = path.relative_to(ROOT)
        if any(part in IGNORED_PARTS for part in relative.parts):
            continue
        if path.is_symlink():
            yield path, relative, "symbolic links are not allowed"
        elif path.is_file():
            yield path, relative, None


def main() -> int:
    failures: list[str] = []

    for path, relative, problem in iter_publishable_files():
        if problem:
            failures.append(f"{relative}: {problem}")
            continue
        if any(part.lower() in PRIVATE_DIRECTORY_NAMES for part in relative.parts):
            failures.append(f"{relative}: private-data directory is not publishable")
            continue
        if path.suffix.lower() in PRIVATE_SUFFIXES:
            failures.append(f"{relative}: data or media files are not publishable")
            continue
        if not is_allowed(relative):
            failures.append(f"{relative}: file is outside the source-only allowlist")
            continue

        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            failures.append(f"{relative}: publishable files must be UTF-8 text")
            continue

        for label, pattern in TEXT_PATTERNS.items():
            if pattern.search(text):
                failures.append(f"{relative}: contains a possible {label}")

    if failures:
        print("v1replay publication safety: FAILED")
        for failure in failures:
            print("  - " + failure)
        return 1

    print("v1replay publication safety: OK (source-only tree)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
