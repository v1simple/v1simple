#!/usr/bin/env python3
"""Check BLE hot-path callback contracts.

Enforced invariants:
1) BLE callback bodies avoid forbidden operations in hot paths.
2) parser->parse() is only called from BleQueueModule::process().

Use --update to rewrite expected snapshot.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[1]
BLE_CALLBACK_FILES: Tuple[Path, ...] = (
    ROOT / "src" / "ble_client.cpp",
    ROOT / "src" / "ble_connection.cpp",
    ROOT / "src" / "ble_proxy.cpp",
    ROOT / "src" / "modules" / "obd" / "obd_ble_client.cpp",
)
BLE_QUEUE_FILE = ROOT / "src" / "modules" / "ble" / "ble_queue_module.cpp"
CONTRACT_FILE = ROOT / "test" / "contracts" / "ble_hot_path_contract.txt"

SOURCE_SCAN_ROOT = ROOT / "src"

BLE_CLIENT_CALLBACK_TARGETS: Sequence[Sequence[str]] = (
    ("V1BLEClient::notifyCallback",),
    ("V1BLEClient::ClientCallbacks::onConnect",),
    ("V1BLEClient::ClientCallbacks::onDisconnect",),
    ("V1BLEClient::ScanCallbacks::onScanResult", "V1BLEClient::ScanCallbacks::onResult"),
    ("V1BLEClient::ScanCallbacks::onScanEnd",),
    ("V1BLEClient::ProxyWriteCallbacks::onWrite",),
    ("V1BLEClient::ProxyServerCallbacks::onConnect",),
    ("V1BLEClient::ProxyServerCallbacks::onDisconnect",),
    ("ObdScanCallback::onResult",),
    ("ObdScanCallback::onScanEnd",),
    ("ObdClientCallback::onDisconnect",),
)

BLE_QUEUE_CALLBACK_TARGETS: Sequence[Sequence[str]] = (
    ("BleQueueModule::onNotify",),
)

FORBIDDEN_PATTERNS = (
    ("forbidden_serial_print", re.compile(r"\bSerial\s*\.\s*print(?:f|ln)?\s*\(")),
    ("forbidden_log_call", re.compile(r"\blog_[A-Za-z0-9_]*\s*\(")),
    ("forbidden_esp_log", re.compile(r"\bESP_LOG[A-Za-z0-9_]*\s*\(")),
    ("forbidden_string", re.compile(r"\bString\b")),
    ("forbidden_new", re.compile(r"\bnew\b")),
    ("forbidden_malloc", re.compile(r"\b(?:malloc|calloc|realloc|free)\s*\(")),
    ("forbidden_delay", re.compile(r"\bdelay\s*\(")),
    ("forbidden_vtaskdelay", re.compile(r"\bvTaskDelay\s*\(")),
    (
        "forbidden_xsemaphoretake_portmaxdelay",
        re.compile(r"\bxSemaphoreTake\s*\([^)]*portMAX_DELAY[^)]*\)", re.DOTALL),
    ),
)

PARSER_PARSE_RE = re.compile(r"\bparser\s*->\s*parse\s*\(")
MASK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)
SIGNATURE_QUALIFIERS = ("const", "override", "final", "noexcept")


@dataclass(frozen=True)
class FunctionBody:
    name: str
    source_path: Path
    open_brace_index: int
    close_brace_index: int


def read_text(path: Path) -> str:
    if not path.exists():
        raise FileNotFoundError(f"Source file not found: {path}")
    return path.read_text(encoding="utf-8")


def to_relative(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def line_for_index(source: str, index: int) -> int:
    return source.count("\n", 0, index) + 1


def mask_comments_and_strings(source: str) -> str:
    def _mask(match: re.Match[str]) -> str:
        return "".join("\n" if ch == "\n" else " " for ch in match.group(0))

    return MASK_RE.sub(_mask, source)


def find_matching_delim(masked_source: str, open_index: int, open_ch: str, close_ch: str) -> int:
    depth = 0
    for idx in range(open_index, len(masked_source)):
        ch = masked_source[idx]
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return idx
    raise ValueError(f"Unbalanced delimiters while parsing near index {open_index}")


def skip_signature_qualifiers(masked_source: str, index: int) -> int:
    idx = index
    while idx < len(masked_source):
        while idx < len(masked_source) and masked_source[idx].isspace():
            idx += 1

        advanced = False
        for qualifier in SIGNATURE_QUALIFIERS:
            end = idx + len(qualifier)
            if masked_source.startswith(qualifier, idx) and (
                end >= len(masked_source)
                or not (masked_source[end].isalnum() or masked_source[end] == "_")
            ):
                idx = end
                advanced = True
                break
        if not advanced:
            break

    return idx


def extract_function_body(
    source: str,
    masked_source: str,
    path: Path,
    qualified_name_candidates: Sequence[str],
) -> FunctionBody | None:
    for qualified_name in qualified_name_candidates:
        name_re = re.compile(re.escape(qualified_name) + r"\s*\(")
        for match in name_re.finditer(masked_source):
            open_paren = masked_source.find("(", match.start())
            if open_paren == -1:
                continue
            close_paren = find_matching_delim(masked_source, open_paren, "(", ")")
            idx = skip_signature_qualifiers(masked_source, close_paren + 1)
            if idx >= len(masked_source):
                continue
            if masked_source[idx] == ";":
                continue
            if masked_source[idx] != "{":
                continue
            close_brace = find_matching_delim(masked_source, idx, "{", "}")
            return FunctionBody(
                name=qualified_name,
                source_path=path,
                open_brace_index=idx,
                close_brace_index=close_brace,
            )
    return None


def read_expected_lines(path: Path) -> List[str]:
    if not path.exists():
        return []
    lines: List[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        lines.append(line)
    return lines


def write_lines(path: Path, header: str, lines: List[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = [header, ""]
    payload.extend(lines)
    payload.append("")
    path.write_text("\n".join(payload), encoding="utf-8")


def make_callback_violations(
    source: str,
    masked_source: str,
    source_path: Path,
    callback_targets: Sequence[Sequence[str]],
) -> List[str]:
    violations: List[str] = []
    relative = to_relative(source_path)

    for candidates in callback_targets:
        body = extract_function_body(source, masked_source, source_path, candidates)
        expected_label = candidates[0]
        if body is None:
            violations.append(
                f"scope={expected_label} file={relative} line=0 rule=missing_callback_body"
            )
            continue

        body_start = body.open_brace_index + 1
        body_end = body.close_brace_index
        body_masked = masked_source[body_start:body_end]

        for rule, pattern in FORBIDDEN_PATTERNS:
            for match in pattern.finditer(body_masked):
                line = line_for_index(source, body_start + match.start())
                violations.append(
                    f"scope={body.name} file={relative} line={line} rule={rule}"
                )

    return violations


def make_multi_source_callback_violations(
    callback_sources: Sequence[Tuple[Path, str, str]],
    callback_targets: Sequence[Sequence[str]],
) -> List[str]:
    violations: List[str] = []
    fallback_file = to_relative(callback_sources[0][0]) if callback_sources else "src/ble_client.cpp"

    for candidates in callback_targets:
        body: FunctionBody | None = None
        selected_source = ""
        selected_masked = ""
        expected_label = candidates[0]

        for path, source, masked_source in callback_sources:
            body = extract_function_body(source, masked_source, path, candidates)
            if body is not None:
                selected_source = source
                selected_masked = masked_source
                break

        if body is None:
            violations.append(
                f"scope={expected_label} file={fallback_file} line=0 rule=missing_callback_body"
            )
            continue

        body_start = body.open_brace_index + 1
        body_end = body.close_brace_index
        body_masked = selected_masked[body_start:body_end]
        relative = to_relative(body.source_path)

        for rule, pattern in FORBIDDEN_PATTERNS:
            for match in pattern.finditer(body_masked):
                line = line_for_index(selected_source, body_start + match.start())
                violations.append(
                    f"scope={body.name} file={relative} line={line} rule={rule}"
                )

    return violations


def make_parser_parse_violations(ble_queue_process: FunctionBody) -> List[str]:
    violations: List[str] = []
    for path in sorted(SOURCE_SCAN_ROOT.rglob("*.cpp")):
        source = read_text(path)
        masked_source = mask_comments_and_strings(source)
        relative = to_relative(path)
        for match in PARSER_PARSE_RE.finditer(masked_source):
            allowed = (
                path == ble_queue_process.source_path
                and ble_queue_process.open_brace_index <= match.start() <= ble_queue_process.close_brace_index
            )
            if allowed:
                continue
            line = line_for_index(source, match.start())
            violations.append(
                f"scope=parser->parse file={relative} line={line} rule=parse_outside_ble_queue_process"
            )
    return violations


def print_diff(expected: List[str], actual: List[str]) -> None:
    expected_set = set(expected)
    actual_set = set(actual)
    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)

    print("[contract] ble-hot-path snapshot mismatch")
    if missing:
        print("  missing:")
        for row in missing:
            print(f"    - {row}")
    if extra:
        print("  extra:")
        for row in extra:
            print(f"    + {row}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite expected contract snapshot from current source",
    )
    args = parser.parse_args()

    callback_sources: List[Tuple[Path, str, str]] = []
    for path in BLE_CALLBACK_FILES:
        if not path.exists():
            continue
        source = read_text(path)
        callback_sources.append((path, source, mask_comments_and_strings(source)))

    ble_queue_source = read_text(BLE_QUEUE_FILE)
    ble_queue_masked = mask_comments_and_strings(ble_queue_source)

    ble_queue_process = extract_function_body(
        ble_queue_source,
        ble_queue_masked,
        BLE_QUEUE_FILE,
        ("BleQueueModule::process",),
    )
    violations: List[str] = []
    if ble_queue_process is None:
        violations.append(
            f"scope=BleQueueModule::process file={to_relative(BLE_QUEUE_FILE)} line=0 rule=missing_process_body"
        )
    else:
        violations.extend(make_parser_parse_violations(ble_queue_process))

    violations.extend(make_multi_source_callback_violations(callback_sources, BLE_CLIENT_CALLBACK_TARGETS))
    violations.extend(
        make_callback_violations(
            ble_queue_source,
            ble_queue_masked,
            BLE_QUEUE_FILE,
            BLE_QUEUE_CALLBACK_TARGETS,
        )
    )
    actual = sorted(set(violations))

    if args.update:
        write_lines(
            CONTRACT_FILE,
            "# BLE hot-path contract violations (expected to stay empty)",
            actual,
        )
        print(f"Updated {CONTRACT_FILE}")
        if actual:
            print("[contract] BLE hot-path contract has violations; resolve before merge.")
            for row in actual:
                print(f"  - {row}")
            return 1
        return 0

    expected = read_expected_lines(CONTRACT_FILE)
    ok = True
    if expected != actual:
        print_diff(expected, actual)
        ok = False

    if actual:
        print("[contract] BLE hot-path violations detected")
        for row in actual:
            print(f"  - {row}")
        ok = False

    if not ok:
        print("\nRun with --update only when intentionally changing contract.")
        return 1

    print("[contract] BLE hot-path contract matches (0 violations)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
