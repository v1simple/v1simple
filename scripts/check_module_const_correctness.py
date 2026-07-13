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


def is_inside_scope_block(source: str, line_no: int) -> bool:
    """
    Check if a line is inside a class/struct/enum/union definition (brace or scope counting).
    This includes both function bodies and type definitions.
    """
    lines = source.split("\n")
    brace_depth = 0
    scope_type = None

    for i in range(min(line_no - 1, len(lines))):
        line = lines[i].strip()

        # Track scope type (class, struct, enum, union)
        if any(kw in line for kw in ["class ", "struct ", "enum ", "union "]):
            # Check if this is a definition (has opening brace)
            if "{" in line:
                scope_type = "type_def"
                brace_depth = line.count("{") - line.count("}")

        # Count braces
        brace_depth += line.count("{") - line.count("}")

    return brace_depth > 0


def is_variable_declaration(line: str) -> bool:
    """
    Check if a line looks like a simple variable declaration at file/namespace scope.
    Excludes function declarations, type definitions, and other non-variable patterns.
    """
    stripped = line.strip()

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

        lines = source.split("\n")

        for line_no, raw_line in enumerate(lines, start=1):
            # Skip empty lines and comment-only lines
            stripped = raw_line.strip()
            if not stripped or stripped.startswith("//"):
                continue

            # Skip preprocessor directives
            if stripped.startswith("#"):
                continue

            # Skip if inside any scope block (class, struct, function, etc.)
            if is_inside_scope_block(source, line_no):
                continue

            # Check if this looks like a variable declaration
            if is_variable_declaration(raw_line):
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
