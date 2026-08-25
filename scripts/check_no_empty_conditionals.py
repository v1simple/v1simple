#!/usr/bin/env python3
"""Reject unexplained empty C/C++ control-flow bodies.

An intentional empty body must contain a reason in this exact form:
    // EMPTY_BODY_OK: explanation
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = ("src", "include", "test")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".ino"}
ANNOTATION_RE = re.compile(r"EMPTY_BODY_OK:\s*\S[^\r\n]*")


@dataclass(frozen=True)
class Finding:
    offset: int
    kind: str


def mask_comments_and_literals(source: str) -> str:
    masked = list(source)
    index = 0
    length = len(source)

    def blank(start: int, end: int) -> None:
        for pos in range(start, end):
            if masked[pos] not in "\r\n":
                masked[pos] = " "

    while index < length:
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = length if end < 0 else end
            blank(index, end)
            index = end
            continue
        if source.startswith("/*", index):
            end_marker = source.find("*/", index + 2)
            end = length if end_marker < 0 else end_marker + 2
            blank(index, end)
            index = end
            continue
        if source.startswith('R"', index):
            delimiter_end = source.find("(", index + 2)
            if delimiter_end >= 0:
                delimiter = source[index + 2 : delimiter_end]
                closing = ")" + delimiter + '"'
                closing_at = source.find(closing, delimiter_end + 1)
                end = length if closing_at < 0 else closing_at + len(closing)
                blank(index, end)
                index = end
                continue
        if source[index] in "\"'":
            quote = source[index]
            end = index + 1
            while end < length:
                if source[end] == "\\":
                    end += 2
                    continue
                end += 1
                if source[end - 1] == quote:
                    break
            blank(index, min(end, length))
            index = end
            continue
        index += 1

    return "".join(masked)


def control_keyword_before_brace(masked: str, brace_at: int) -> str | None:
    cursor = brace_at - 1
    while cursor >= 0 and masked[cursor].isspace():
        cursor -= 1
    if cursor < 0:
        return None

    prefix = masked[: cursor + 1]
    direct = re.search(r"\b(else|do)\s*$", prefix)
    if direct:
        return direct.group(1)
    if masked[cursor] != ")":
        return None

    depth = 1
    cursor -= 1
    while cursor >= 0 and depth:
        if masked[cursor] == ")":
            depth += 1
        elif masked[cursor] == "(":
            depth -= 1
        cursor -= 1
    if depth:
        return None

    before_condition = masked[: cursor + 1]
    match = re.search(r"\b(if|for|while|switch)(?:\s+constexpr)?\s*$", before_condition)
    return match.group(1) if match else None


def find_empty_bodies(source: str) -> list[Finding]:
    masked = mask_comments_and_literals(source)
    findings: list[Finding] = []
    for match in re.finditer(r"\{\s*\}", masked):
        kind = control_keyword_before_brace(masked, match.start())
        if kind is None:
            continue
        original_body = source[match.start() + 1 : match.end() - 1]
        if not ANNOTATION_RE.search(original_body):
            findings.append(Finding(match.start(), kind))
    return findings


def iter_source_files(paths: list[Path]) -> list[Path]:
    files: list[Path] = []
    for path in paths:
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            files.append(path)
        elif path.is_dir():
            files.extend(
                candidate
                for candidate in path.rglob("*")
                if candidate.is_file() and candidate.suffix in SOURCE_SUFFIXES
            )
    return sorted(set(files))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path)
    args = parser.parse_args()
    paths = args.paths or [ROOT / directory for directory in SOURCE_DIRS]

    failure_count = 0
    for path in iter_source_files(paths):
        source = path.read_text(encoding="utf-8")
        for finding in find_empty_bodies(source):
            line = source.count("\n", 0, finding.offset) + 1
            try:
                display_path = path.resolve().relative_to(ROOT)
            except ValueError:
                display_path = path
            print(f"{display_path}:{line}: empty {finding.kind} body lacks EMPTY_BODY_OK reason")
            failure_count += 1

    if failure_count:
        print(f"Found {failure_count} unexplained empty control-flow body/bodies.", file=sys.stderr)
        return 1
    print("No unexplained empty control-flow bodies found.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
