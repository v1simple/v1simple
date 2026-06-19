#!/usr/bin/env python3
"""Forbid ``std::function`` in production source trees.

Contract:
1) No ``std::function`` type references in ``include/`` or ``src/``.
2) Comments and string literals are ignored — only real type usage counts.
3) Any future need for ``std::function`` must be added explicitly to
   ``test/contracts/std_function_allowlist.txt`` (one ``file:line`` per entry).

Rationale:
``std::function`` allocates on the heap for any non-trivial capture. On the
ESP32-S3 this is pressure we cannot afford in module wiring — use ``begin()``
direct pointer injection or a ``Providers`` struct with function pointers
instead. See ``ProviderCallbackBindings::member`` in
``include/provider_callback_bindings.h``.

Test code (``test/``) is exempt because test mocks can pay the heap cost.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (ROOT / "include", ROOT / "src")
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp"}
ALLOWLIST_FILE = ROOT / "test" / "contracts" / "std_function_allowlist.txt"

# Match comments and string literals so we can strip them before grepping.
# Covers // line, /* block */, "double", and 'single' with escape handling.
MASK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)
STD_FUNCTION_RE = re.compile(r"\bstd::function\b")


def mask_comments_and_strings(source: str) -> str:
    """Blank out comments and string literals while preserving line numbers."""

    def _mask(match: re.Match[str]) -> str:
        return "".join("\n" if ch == "\n" else " " for ch in match.group(0))

    return MASK_RE.sub(_mask, source)


def iter_source_files() -> list[Path]:
    files: list[Path] = []
    for root in SOURCE_ROOTS:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                files.append(path)
    return sorted(files)


def load_allowlist(path: Path) -> set[str]:
    if not path.exists():
        return set()
    entries: set[str] = set()
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        entries.add(line)
    return entries


def scan_hits() -> list[tuple[str, int, str]]:
    hits: list[tuple[str, int, str]] = []
    for path in iter_source_files():
        text = path.read_text(encoding="utf-8")
        masked = mask_comments_and_strings(text)
        relative = path.relative_to(ROOT).as_posix()
        # Use masked text's line structure (line-aligned with original).
        for line_no, masked_line in enumerate(masked.splitlines(), start=1):
            if STD_FUNCTION_RE.search(masked_line):
                original_line = text.splitlines()[line_no - 1].strip()
                hits.append((relative, line_no, original_line))
    return hits


def main() -> int:
    allowlist = load_allowlist(ALLOWLIST_FILE)
    hits = scan_hits()

    disallowed: list[tuple[str, int, str]] = []
    used_allowlist: set[str] = set()
    for relative, line_no, snippet in hits:
        key = f"{relative}:{line_no}"
        if key in allowlist:
            used_allowlist.add(key)
        else:
            disallowed.append((relative, line_no, snippet))

    stale = sorted(allowlist - used_allowlist)
    if stale:
        print("[contract] std-function: allowlist has stale entries that no longer match any hit:")
        for key in stale:
            print(f"    - {key}")
        print(f"    Remove them from {ALLOWLIST_FILE.relative_to(ROOT)}.")

    if disallowed:
        print("[contract] std-function: forbidden occurrence(s) in production source:")
        for relative, line_no, snippet in disallowed:
            print(f"    {relative}:{line_no}: {snippet}")
        print(
            "\nUse begin() direct pointer injection or a Providers struct with function "
            "pointers instead.\n"
            "If the occurrence is unavoidable, add its 'file:line' entry to "
            f"{ALLOWLIST_FILE.relative_to(ROOT)} with a comment explaining why."
        )
        return 1

    if stale:
        return 1

    print(
        "[contract] std-function contract intact "
        f"(scanned {sum(1 for _ in iter_source_files())} files, "
        f"allowlist={len(allowlist)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
