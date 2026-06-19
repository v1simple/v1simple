#!/usr/bin/env python3
"""Check BLE hot-path semantic rules without snapshot coupling."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_ble_hot_path_contract as contract  # type: ignore  # noqa: E402


def main() -> int:
    callback_sources = []
    for path in contract.BLE_CALLBACK_FILES:
        if not path.exists():
            continue
        source = contract.read_text(path)
        callback_sources.append((path, source, contract.mask_comments_and_strings(source)))

    ble_queue_source = contract.read_text(contract.BLE_QUEUE_FILE)
    ble_queue_masked = contract.mask_comments_and_strings(ble_queue_source)
    ble_queue_process = contract.extract_function_body(
        ble_queue_source,
        ble_queue_masked,
        contract.BLE_QUEUE_FILE,
        ("BleQueueModule::process",),
    )

    violations: list[str] = []
    if ble_queue_process is None:
        violations.append(
            f"scope=BleQueueModule::process file={contract.to_relative(contract.BLE_QUEUE_FILE)} line=0 rule=missing_process_body"
        )
    else:
        violations.extend(contract.make_parser_parse_violations(ble_queue_process))

    violations.extend(contract.make_multi_source_callback_violations(callback_sources, contract.BLE_CLIENT_CALLBACK_TARGETS))
    violations.extend(
        contract.make_callback_violations(
            ble_queue_source,
            ble_queue_masked,
            contract.BLE_QUEUE_FILE,
            contract.BLE_QUEUE_CALLBACK_TARGETS,
        )
    )
    violations = sorted(set(violations))

    if violations:
        print("[guard] BLE hot-path semantic violations detected")
        for row in violations:
            print(f"  - {row}")
        return 1

    print("[guard] BLE hot-path semantic guard matches (0 violations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
