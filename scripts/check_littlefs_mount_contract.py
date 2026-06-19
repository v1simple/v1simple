#!/usr/bin/env python3
"""Static contracts for LittleFS mount configuration and image tooling."""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "littlefs_mount.h"
HELPER = ROOT / "src" / "littlefs_mount.cpp"
PARTITIONS = ROOT / "partitions_v1.csv"
PLATFORMIO = ROOT / "platformio.ini"
ENV = "waveshare-349"


def strip_cpp_comments_and_strings(text: str) -> str:
    result: list[str] = []
    i = 0
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line_comment"
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block_comment"
                i += 2
                continue
            if ch == '"':
                state = "string"
                result.append(" ")
                i += 1
                continue
            if ch == "'":
                state = "char"
                result.append(" ")
                i += 1
                continue
            result.append(ch)
            i += 1
            continue

        if state == "line_comment":
            if ch == "\n":
                result.append("\n")
                state = "code"
            i += 1
            continue

        if state == "block_comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 2
            else:
                if ch == "\n":
                    result.append("\n")
                i += 1
            continue

        if state == "string":
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                state = "code"
            if ch == "\n":
                result.append("\n")
                state = "code"
            i += 1
            continue

        if state == "char":
            if ch == "\\":
                i += 2
                continue
            if ch == "'":
                state = "code"
            if ch == "\n":
                result.append("\n")
                state = "code"
            i += 1
            continue

    return "".join(result)


def source_files() -> list[Path]:
    roots = [ROOT / "src", ROOT / "include"]
    suffixes = {".c", ".cc", ".cpp", ".h", ".hpp", ".ino"}
    files: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        files.extend(path for path in root.rglob("*") if path.is_file() and path.suffix in suffixes)
    return sorted(files)


def check_single_mount_call(errors: list[str]) -> None:
    call_token = "LittleFS" + ".begin("
    helper_count = 0
    for path in source_files():
        stripped = strip_cpp_comments_and_strings(path.read_text(encoding="utf-8", errors="ignore"))
        count = stripped.count(call_token)
        if path == HELPER:
            helper_count = count
        elif count:
            errors.append(f"{path.relative_to(ROOT)} contains raw LittleFS mount call")

    if helper_count != 1:
        errors.append(f"{HELPER.relative_to(ROOT)} must contain exactly one LittleFS mount call, found {helper_count}")


def header_text() -> str:
    if not HEADER.exists():
        raise FileNotFoundError(f"missing {HEADER.relative_to(ROOT)}")
    return HEADER.read_text(encoding="utf-8")


def constexpr_bool(text: str, name: str) -> str | None:
    match = re.search(rf"\b{name}\s*=\s*(true|false)\s*;", text)
    return match.group(1) if match else None


def constexpr_string(text: str, name: str) -> str | None:
    match = re.search(rf"\b{name}\s*(?:\[[^\]]*\])?\s*=\s*\"([^\"]*)\"\s*;", text)
    return match.group(1) if match else None


def constexpr_int(text: str, name: str) -> int | None:
    match = re.search(rf"\b{name}\s*=\s*(\d+)\s*;", text)
    return int(match.group(1)) if match else None


def partition_names() -> set[str]:
    names: set[str] = set()
    with PARTITIONS.open(newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row or row[0].strip().startswith("#"):
                continue
            names.add(row[0].strip())
    return names


def check_mount_constants(errors: list[str]) -> None:
    try:
        text = header_text()
    except FileNotFoundError as exc:
        errors.append(str(exc))
        return

    expected_bool = constexpr_bool(text, "kAutoFormat")
    expected_base = constexpr_string(text, "kBasePath")
    expected_open = constexpr_int(text, "kMaxOpenFiles")
    expected_label = constexpr_string(text, "kPartitionLabel")

    if expected_bool != "false":
        errors.append("kAutoFormat must be false")
    if expected_base != "/littlefs":
        errors.append('kBasePath must be "/littlefs"')
    if expected_open != 10:
        errors.append("kMaxOpenFiles must be 10")
    if expected_label != "storage":
        errors.append('kPartitionLabel must be "storage"')

    if expected_label and expected_label not in partition_names():
        errors.append(f'kPartitionLabel "{expected_label}" does not match a partition name')


def env_block(env: str) -> str:
    text = PLATFORMIO.read_text(encoding="utf-8")
    match = re.search(rf"^\[env:{re.escape(env)}\]\s*$([\s\S]*?)(?=^\[|\Z)", text, re.MULTILINE)
    if not match:
        raise ValueError(f"platformio.ini missing [env:{env}]")
    return match.group(1)


def strip_ini_comment(line: str) -> str:
    return line.split(";", 1)[0].strip()


def env_value(env: str, key: str) -> str | None:
    for line in env_block(env).splitlines():
        stripped = strip_ini_comment(line)
        if not stripped or "=" not in stripped:
            continue
        got_key, value = stripped.split("=", 1)
        if got_key.strip() == key:
            return value.strip()
    return None


def check_filesystem_config(errors: list[str]) -> None:
    try:
        fs_value = env_value(ENV, "board_build.filesystem")
    except ValueError as exc:
        errors.append(str(exc))
        return
    if fs_value != "littlefs":
        errors.append(f"[env:{ENV}] board_build.filesystem must be littlefs, got {fs_value!r}")


def candidate_files_for_forbidden_scan() -> list[Path]:
    skipped_dirs = {
        ".git",
        ".pio",
        ".artifacts",
        ".venv",
        "docs",
        "node_modules",
        "dist",
        "build",
        ".svelte-kit",
    }
    files: list[Path] = []
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        rel_parts = path.relative_to(ROOT).parts
        if any(part in skipped_dirs for part in rel_parts):
            continue
        files.append(path)
    return sorted(files)


def check_forbidden_image_tool(errors: list[str]) -> None:
    forbidden = "mk" + "littlefs"
    forbidden_tool = "tool-" + forbidden
    for path in candidate_files_for_forbidden_scan():
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        if path == PLATFORMIO:
            text = "\n".join(strip_ini_comment(line) for line in text.splitlines())
        if forbidden in text or forbidden_tool in text:
            errors.append(f"{path.relative_to(ROOT)} references forbidden filesystem image tool")


def main() -> int:
    errors: list[str] = []
    check_single_mount_call(errors)
    check_mount_constants(errors)
    check_filesystem_config(errors)
    check_forbidden_image_tool(errors)

    if errors:
        print("[contract] LittleFS mount contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] LittleFS mount/tooling contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
