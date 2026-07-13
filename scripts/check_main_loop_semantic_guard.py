#!/usr/bin/env python3
"""Check main loop semantic invariants without snapshot coupling."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_main_loop_call_order_contract as contract  # type: ignore  # noqa: E402


def main() -> int:
    source = contract.read_text(contract.SRC_FILE)
    masked = contract.mask_comments_and_strings(source)
    loop_body_start, _loop_body_end, _loop_source, loop_masked = contract.extract_loop_body(source, masked)

    call_positions: dict[str, int] = {}
    errors: list[str] = []
    for name, pattern in contract.ORDERED_CALLS:
        match = pattern.search(loop_masked)
        if not match:
            errors.append(f"missing required call in loop(): {name}")
            continue
        call_positions[name] = match.start()

    if len(call_positions) == len(contract.ORDERED_CALLS):
        previous_name = contract.ORDERED_CALLS[0][0]
        for next_name, _ in contract.ORDERED_CALLS[1:]:
            if call_positions[previous_name] >= call_positions[next_name]:
                errors.append(
                    f"call-order violation: {previous_name} must appear before {next_name}"
                )
            previous_name = next_name

    blocking_violations: list[str] = []
    for match in contract.DELAY_RE.finditer(loop_masked):
        line = contract.line_for_index(source, loop_body_start + match.start())
        blocking_violations.append(f"line={line} rule=forbidden_delay")

    for match in contract.XSEMAPHORE_PORTMAX_RE.finditer(loop_masked):
        line = contract.line_for_index(source, loop_body_start + match.start())
        blocking_violations.append(f"line={line} rule=forbidden_xSemaphoreTake_portMAX_DELAY")

    for match in contract.VTASKDELAY_RE.finditer(loop_masked):
        open_paren = loop_body_start + match.end() - 1
        close_paren = contract.find_matching_delim(masked, open_paren, "(", ")")
        arg_text = source[open_paren + 1 : close_paren]
        if contract.is_allowed_vtaskdelay_arg(arg_text):
            continue
        line = contract.line_for_index(source, loop_body_start + match.start())
        compact_arg = "".join(arg_text.split())[:80]
        blocking_violations.append(
            f"line={line} rule=forbidden_vTaskDelay_arg arg={compact_arg}"
        )

    blocking_violations = sorted(set(blocking_violations))
    if errors or blocking_violations:
        if errors:
            print("[guard] main loop structure violations detected")
            for row in errors:
                print(f"  - {row}")
        if blocking_violations:
            print("[guard] main loop blocking-call violations detected")
            for row in blocking_violations:
                print(f"  - {row}")
        return 1

    print(
        "[guard] main loop semantic guard matches "
        f"({len(call_positions)} ordered calls, 0 blocking violations)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
