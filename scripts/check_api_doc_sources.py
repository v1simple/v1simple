#!/usr/bin/env python3
"""Validate HTTP route docs stay aligned without stale source line citations."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
API_DOC = ROOT / "docs" / "API.md"
WIFI_ROUTES = ROOT / "src" / "wifi_routes.cpp"

ROUTE_REGISTRATION_RE = re.compile(r'server_\.on\("([^"]+)",\s*(HTTP_[A-Z]+),')
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


def module_api_source_errors() -> list[str]:
    errors: list[str] = []
    for doc in sorted((ROOT / "src" / "modules").glob("*/api.md")):
        text = doc.read_text(encoding="utf-8")
        for source_ref in MODULE_SOURCE_RE.findall(text):
            source_file = source_ref.split(":", 1)[0].strip()
            if not source_file or not (source_file.endswith(".h") or source_file.endswith(".cpp")):
                continue
            source_path = doc.parent / source_file
            if not source_path.exists():
                errors.append(f"{doc.relative_to(ROOT)} references missing source `{source_ref}`")
                continue

            if ":" in source_ref:
                line_part = source_ref.split(":", 1)[1]
                line_numbers = [int(n) for n in re.findall(r"\d+", line_part)]
                if line_numbers:
                    line_count = len(source_path.read_text(encoding="utf-8", errors="replace").splitlines())
                    for line_number in line_numbers:
                        if line_number > line_count:
                            errors.append(
                                f"{doc.relative_to(ROOT)} references `{source_ref}` "
                                f"but {source_file} has only {line_count} lines"
                            )
                            break
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
