#!/usr/bin/env python3
"""Validate HTTP route docs stay aligned without stale source line citations."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
API_DOC = ROOT / "docs" / "API.md"
WIFI_ROUTES = ROOT / "src" / "wifi_routes.cpp"

# Whitespace-tolerant: clang-format may re-wrap the registration across lines.
# A missed route here would silently drop it from the docs-coverage comparison.
ROUTE_REGISTRATION_RE = re.compile(r'server_\.on\(\s*"([^"]+)"\s*,\s*(HTTP_[A-Z]+)\s*,')
DOC_ROUTE_RE = re.compile(r"^###\s+(GET|POST|PUT|PATCH|DELETE)\s+`([^`]+)`", re.MULTILINE)
STALE_SOURCE_RE = re.compile(r"^\*\*Source:\*\*.*\b[\w./-]+:\d+\b", re.MULTILINE)
MODULE_SOURCE_RE = re.compile(r"^\*\*Source:\*\*\s+`([^`]+)`", re.MULTILINE)


def source_routes() -> set[str]:
    text = WIFI_ROUTES.read_text(encoding="utf-8")
    routes = {
        f"{method.removeprefix('HTTP_')} {path}"
        for path, method in ROUTE_REGISTRATION_RE.findall(text)
    }
    if "server_.onNotFound" in text:
        routes.add("GET /_app/*")
    return routes


def documented_routes() -> set[str]:
    text = API_DOC.read_text(encoding="utf-8")
    return {f"{method} {path}" for method, path in DOC_ROUTE_RE.findall(text)}


def heading_symbol(heading: str) -> str | None:
    """The symbol a `#### `void begin(...)`` / `#### `struct Foo`` heading documents."""
    match = re.search(r"`([^`]+)`", heading)
    if not match:
        return None
    signature = match.group(1).strip()
    typed = re.match(r"(?:struct|class|enum(?:\s+class)?)\s+(\w+)", signature)
    if typed:
        return typed.group(1)
    func = re.search(r"(\w+)\s*\(", signature)
    if func:
        return func.group(1)
    return None


def module_api_source_errors() -> list[str]:
    errors: list[str] = []
    for doc in sorted((ROOT / "src" / "modules").glob("*/api.md")):
        lines = doc.read_text(encoding="utf-8").splitlines()
        for index, line in enumerate(lines):
            match = re.match(r"^\*\*Source:\*\*\s+`([^`]+)`", line)
            if not match:
                continue
            source_ref = match.group(1)
            source_file = source_ref.split(":", 1)[0].strip()
            if not source_file or not (source_file.endswith(".h") or source_file.endswith(".cpp")):
                continue
            source_path = doc.parent / source_file
            if not source_path.exists():
                errors.append(f"{doc.relative_to(ROOT)} references missing source `{source_ref}`")
                continue
            if ":" not in source_ref:
                continue

            source_lines = source_path.read_text(encoding="utf-8", errors="replace").splitlines()
            line_numbers = [int(n) for n in re.findall(r"\d+", source_ref.split(":", 1)[1])]
            if not line_numbers:
                continue

            out_of_range = [n for n in line_numbers if n > len(source_lines)]
            if out_of_range:
                errors.append(
                    f"{doc.relative_to(ROOT)} references `{source_ref}` "
                    f"but {source_file} has only {len(source_lines)} lines"
                )
                continue

            # An in-range citation can still point at the WRONG code, and that
            # failure is silent -- 83 of 140 anchors had drifted onto unrelated
            # lines before this check existed, because reformatting and edits
            # shift line numbers and nothing was verifying the target. So assert
            # the cited line actually declares the symbol the heading documents.
            heading = next(
                (lines[j] for j in range(index, -1, -1) if lines[j].startswith("#")),
                "",
            )
            symbol = heading_symbol(heading)
            if not symbol:
                continue
            cited = source_lines[line_numbers[0] - 1]
            if symbol not in cited:
                errors.append(
                    f"{doc.relative_to(ROOT)} cites `{source_ref}` for {symbol!r}, "
                    f"but {source_file}:{line_numbers[0]} is: {cited.strip()[:60]!r}"
                )
    return errors


def main() -> int:
    doc_text = API_DOC.read_text(encoding="utf-8")
    errors: list[str] = []

    stale_sources = STALE_SOURCE_RE.findall(doc_text)
    if stale_sources:
        errors.append("docs/API.md contains exact source line citations:")
        errors.extend(f"    {line}" for line in stale_sources[:20])
        if len(stale_sources) > 20:
            errors.append(f"    ... {len(stale_sources) - 20} more")

    src = source_routes()
    docs = documented_routes()
    missing = sorted(src - docs)
    docs_only = sorted(docs - src)
    if missing:
        errors.append("routes registered in src/wifi_routes.cpp but missing from docs/API.md:")
        errors.extend(f"    {route}" for route in missing)
    if docs_only:
        errors.append("routes documented in docs/API.md but not registered in src/wifi_routes.cpp:")
        errors.extend(f"    {route}" for route in docs_only)

    module_source_errors = module_api_source_errors()
    if module_source_errors:
        errors.append("per-module api.md files reference missing source files:")
        errors.extend(f"    {error}" for error in module_source_errors)

    if errors:
        print("[contract] API docs source contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(f"[contract] API docs cover {len(docs)} HTTP routes and module source anchors are present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
