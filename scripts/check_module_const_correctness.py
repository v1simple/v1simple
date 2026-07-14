#!/usr/bin/env python3
"""Check that module headers declare only const/constexpr global state."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULES_DIR = ROOT / "src" / "modules"


def mask_comments_and_strings(source: str) -> str:
    """Replace comments and string literals with spaces to avoid false matches."""
    result = []
    i = 0
    while i < len(source):
        # Handle block comments
        if i < len(source) - 1 and source[i : i + 2] == "/*":
            end = source.find("*/", i + 2)
            if end != -1:
                result.append(" " * (end + 2 - i))
                i = end + 2
                continue
        # Handle line comments
        if i < len(source) - 1 and source[i : i + 2] == "//":
            newline = source.find("\n", i)
            if newline != -1:
                result.append(" " * (newline - i) + "\n")
                i = newline + 1
            else:
                result.append(" " * (len(source) - i))
                i = len(source)
            continue
        # Handle string literals (double quotes)
        if source[i] == '"':
            result.append(" ")
            i += 1
            while i < len(source):
                if source[i] == "\\":
                    result.append("  ")
                    i += 2
                elif source[i] == '"':
                    result.append(" ")
                    i += 1
                    break
                else:
                    result.append(" ")
                    i += 1
            continue
        # Handle character literals (single quotes)
        if source[i] == "'":
            result.append(" ")
            i += 1
            while i < len(source):
                if source[i] == "\\":
                    result.append("  ")
                    i += 2
                elif source[i] == "'":
                    result.append(" ")
                    i += 1
                    break
                else:
                    result.append(" ")
                    i += 1
            continue
        result.append(source[i])
        i += 1
    return "".join(result)


def strip_preprocessor(masked: str) -> str:
    """Blank out preprocessor directives (including line continuations).

    The line-based scanner used to skip any line starting with '#'. Statement-based
    scanning needs the same exclusion, so directives are replaced by spaces (newlines
    preserved) to keep byte offsets — and therefore reported line numbers — intact.
    """
    out = list(masked)
    i = 0
    n = len(masked)
    while i < n:
        # Find first non-space char of the line.
        line_start = i
        while i < n and masked[i] in " \t":
            i += 1
        if i < n and masked[i] == "#":
            # Blank the directive, honouring backslash line continuations.
            while i < n:
                ch = masked[i]
                if ch == "\n":
                    if i > 0 and masked[:i].rstrip(" \t").endswith("\\"):
                        i += 1  # continued directive: keep blanking
                        continue
                    break
                out[i] = " "
                i += 1
        # Advance to end of line.
        while i < n and masked[i] != "\n":
            i += 1
        i += 1
        if i <= line_start:  # safety: never loop forever
            i = line_start + 1
    return "".join(out)


def iter_file_scope_statements(masked: str):
    """Yield (start_index, statement_text) for every ';'-terminated statement at
    file/namespace scope (brace depth 0).

    Working on logical statements rather than physical lines is what makes this
    guard clang-format proof: a declaration that gets wrapped across lines (e.g.
    broken after '=') is still evaluated as one statement.
    """
    depth = 0
    start: int | None = None
    for idx, ch in enumerate(masked):
        if ch == "{":
            depth += 1
            start = None
        elif ch == "}":
            depth = max(0, depth - 1)
            start = None
        elif depth == 0:
            if ch == ";":
                if start is not None:
                    yield start, masked[start : idx + 1]
                start = None
            elif not ch.isspace() and start is None:
                start = idx


def is_variable_declaration(line: str) -> bool:
    """
    Check if a statement looks like a simple variable declaration at file/namespace scope.
    Excludes function declarations, type definitions, and other non-variable patterns.

    Accepts a whole logical statement (possibly spanning several physical lines);
    interior whitespace is collapsed so wrapped declarations read like single-line ones.
    """
    stripped = re.sub(r"\s+", " ", line).strip()

    # Must end with semicolon (not part of larger statement)
    if not stripped.endswith(";"):
        return False

    # Skip common non-declarations
    skip_keywords = [
        "typedef",
        "using",
        "enum",
        "class",
        "struct",
        "union",
        "#include",
        "#define",
        "#if",
        "#endif",
    ]
    if any(keyword in stripped for keyword in skip_keywords):
        return False

    # Skip extern, const, constexpr, static declarations
    modifiers = ["extern", "const", "constexpr", "static"]
    if any(modifier in stripped for modifier in modifiers):
        return False

    # Skip if contains parentheses (likely function)
    if "(" in stripped or ")" in stripped:
        return False

    # Skip template declarations
    if "template" in stripped or "<" in stripped:
        return False

    # Basic check: looks like "type name;" or "type name = value;"
    # At namespace/file scope, non-const globals are problematic
    # Match: identifier [*&]* identifier [= ...] ;
    pattern = r"^\s*[a-zA-Z_:][a-zA-Z0-9_:<>\s]*[\*&]?\s+[a-zA-Z_][a-zA-Z0-9_]*\s*=?.*;"
    return re.match(pattern, stripped) is not None


def scan_module_headers() -> list[str]:
    """Scan all .h files under src/modules/ for non-const global declarations."""
    violations: list[str] = []

    if not MODULES_DIR.exists():
        return violations

    for header_path in sorted(MODULES_DIR.rglob("*.h")):
        relative = header_path.relative_to(ROOT).as_posix()
        source = header_path.read_text(encoding="utf-8", errors="replace")

        # Mask comments/strings first (this also removes the old block-comment
        # false-positive path), then drop preprocessor directives. Both keep byte
        # offsets stable so reported line numbers stay accurate.
        masked = strip_preprocessor(mask_comments_and_strings(source))

        # Statement-scoped, not line-scoped: brace depth 0 == file/namespace scope,
        # which is the same set of declarations the old line scanner targeted.
        for start_index, statement in iter_file_scope_statements(masked):
            if is_variable_declaration(statement):
                line_no = source.count("\n", 0, start_index) + 1
                violations.append(f"file={relative} line={line_no}")

    return violations


def main() -> int:
    violations = scan_module_headers()

    if violations:
        print("[guard] module const-correctness violations detected")
        for row in violations:
            print(f"  - {row}")
        return 1

    print("[guard] module const-correctness guard matches (0 non-const globals)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
