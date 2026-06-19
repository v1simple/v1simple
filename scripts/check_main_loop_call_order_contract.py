#!/usr/bin/env python3
"""Check main loop call-order contract.

Enforced invariants in src/main.cpp::loop():
1) Key subsystem calls keep expected relative order.
2) Forbidden blocking calls are absent (delay, blocking semaphore take, long vTaskDelay).

Use --update to rewrite expected call-order snapshot.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Tuple

ROOT = Path(__file__).resolve().parents[1]
SRC_FILE = ROOT / "src" / "main.cpp"
CONTRACT_FILE = ROOT / "test" / "contracts" / "main_loop_call_order_contract.txt"

MASK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)
LOOP_START_RE = re.compile(r"\bvoid\s+loop\s*\(\s*\)\s*\{")

ORDERED_CALLS: Tuple[Tuple[str, re.Pattern[str]], ...] = (
    ("processLoopConnectionEarlyPhase", re.compile(r"\bprocessLoopConnectionEarlyPhase\s*\(")),
    ("shouldReturnEarlyFromLoopPowerTouchPhase", re.compile(r"\bshouldReturnEarlyFromLoopPowerTouchPhase\s*\(")),
    ("processLoopIngestPhase", re.compile(r"\bprocessLoopIngestPhase\s*\(")),
    ("obdRuntimeModule.update", re.compile(r"\bobdRuntimeModule\s*\.\s*update\s*\(")),
    ("speedSourceSelector.update", re.compile(r"\bspeedSourceSelector\s*\.\s*update\s*\(")),
    ("processLoopDisplayPreWifiPhase", re.compile(r"\bprocessLoopDisplayPreWifiPhase\s*\(")),
    ("processLoopWifiPhase", re.compile(r"\bprocessLoopWifiPhase\s*\(")),
    ("loopTelemetryModule.process", re.compile(r"\bloopTelemetryModule\s*\.\s*process\s*\(")),
    ("processLoopFinalizePhase", re.compile(r"\bprocessLoopFinalizePhase\s*\(")),
)

DELAY_RE = re.compile(r"\bdelay\s*\(")
XSEMAPHORE_PORTMAX_RE = re.compile(r"\bxSemaphoreTake\s*\([^)]*portMAX_DELAY[^)]*\)", re.DOTALL)
VTASKDELAY_RE = re.compile(r"\bvTaskDelay\s*\(")


def read_text(path: Path) -> str:
    if not path.exists():
        raise FileNotFoundError(f"Source file not found: {path}")
    return path.read_text(encoding="utf-8")


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


def extract_loop_body(source: str, masked_source: str) -> Tuple[int, int, str, str]:
    match = LOOP_START_RE.search(masked_source)
    if not match:
        raise ValueError("loop() definition not found in src/main.cpp")
    open_brace = match.end() - 1
    close_brace = find_matching_delim(masked_source, open_brace, "{", "}")
    body_source = source[open_brace + 1 : close_brace]
    body_masked = masked_source[open_brace + 1 : close_brace]
    return open_brace + 1, close_brace, body_source, body_masked


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


def print_diff(expected: List[str], actual: List[str]) -> None:
    expected_set = set(expected)
    actual_set = set(actual)
    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)

    print("[contract] main-loop-call-order snapshot mismatch")
    if missing:
        print("  missing:")
        for row in missing:
            print(f"    - {row}")
    if extra:
        print("  extra:")
        for row in extra:
            print(f"    + {row}")


def is_allowed_vtaskdelay_arg(arg_text: str) -> bool:
    compact = re.sub(r"\s+", "", arg_text)
    if re.fullmatch(r"(0|1)[uUlL]*", compact):
        return True
    if re.fullmatch(r"pdMS_TO_TICKS\((0|1)[uUlL]*\)", compact):
        return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite expected contract snapshot from current source",
    )
    args = parser.parse_args()

    source = read_text(SRC_FILE)
    masked = mask_comments_and_strings(source)
    loop_body_start, _loop_body_end, loop_source, loop_masked = extract_loop_body(source, masked)

    call_positions: Dict[str, int] = {}
    errors: List[str] = []
    for name, pattern in ORDERED_CALLS:
        match = pattern.search(loop_masked)
        if not match:
            errors.append(f"missing required call in loop(): {name}")
            continue
        call_positions[name] = match.start()

    actual_order = [
        name
        for name, _pos in sorted(call_positions.items(), key=lambda item: item[1])
    ]

    # Enforce relative order explicitly for clarity in failure output.
    if len(call_positions) == len(ORDERED_CALLS):
        previous_name = ORDERED_CALLS[0][0]
        for next_name, _ in ORDERED_CALLS[1:]:
            if call_positions[previous_name] >= call_positions[next_name]:
                errors.append(
                    f"call-order violation: {previous_name} must appear before {next_name}"
                )
            previous_name = next_name

    blocking_violations: List[str] = []
    for match in DELAY_RE.finditer(loop_masked):
        line = line_for_index(source, loop_body_start + match.start())
        blocking_violations.append(f"line={line} rule=forbidden_delay")

    for match in XSEMAPHORE_PORTMAX_RE.finditer(loop_masked):
        line = line_for_index(source, loop_body_start + match.start())
        blocking_violations.append(f"line={line} rule=forbidden_xSemaphoreTake_portMAX_DELAY")

    for match in VTASKDELAY_RE.finditer(loop_masked):
        open_paren = loop_body_start + match.end() - 1
        close_paren = find_matching_delim(masked, open_paren, "(", ")")
        arg_text = source[open_paren + 1 : close_paren]
        if is_allowed_vtaskdelay_arg(arg_text):
            continue
        line = line_for_index(source, loop_body_start + match.start())
        compact_arg = re.sub(r"\s+", "", arg_text)[:80]
        blocking_violations.append(
            f"line={line} rule=forbidden_vTaskDelay_arg arg={compact_arg}"
        )

    blocking_violations = sorted(set(blocking_violations))

    if args.update:
        write_lines(
            CONTRACT_FILE,
            "# Main loop call-order contract (required relative sequence)",
            actual_order,
        )
        print(f"Updated {CONTRACT_FILE}")

    expected_order = read_expected_lines(CONTRACT_FILE)
    ok = True

    if expected_order != actual_order:
        print_diff(expected_order, actual_order)
        ok = False

    if errors:
        print("[contract] main loop structure violations detected")
        for err in errors:
            print(f"  - {err}")
        ok = False

    if blocking_violations:
        print("[contract] main loop blocking-call violations detected")
        for row in blocking_violations:
            print(f"  - {row}")
        ok = False

    if not ok:
        print("\nRun with --update only when intentionally changing contract.")
        return 1

    print(
        "[contract] main-loop-call-order contract matches "
        f"({len(actual_order)} ordered calls, 0 blocking violations)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
