#!/usr/bin/env python3
"""Fail-closed allow-list for native tests that compile production sources."""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

LINKED_NATIVE_TEST_PILOT_ENV = "V1_LINKED_NATIVE_TEST_PILOT"


@dataclass(frozen=True)
class LinkedNativeTestSpec:
    sources: tuple[str, ...]
    define: str
    library_include_dirs: tuple[str, ...] = ()


LINKED_NATIVE_TEST_SPECS = {
    "test_api_maintenance_runtime_matrix": LinkedNativeTestSpec(
        sources=(
            "src/modules/alp/alp_sd_logger.cpp",
            "src/modules/alp/alp_runtime_module.cpp",
            "src/modules/alp/alp_api_service.cpp",
            "src/modules/gps/gps_publishers.cpp",
            "src/modules/gps/gps_runtime_module.cpp",
            "src/modules/gps/gps_api_service.cpp",
            "src/modules/obd/obd_elm327_parser.cpp",
            "src/modules/obd/obd_runtime_module.cpp",
            "src/modules/obd/obd_runtime_transport.cpp",
            "src/modules/obd/obd_runtime_commands.cpp",
            "src/modules/obd/obd_runtime_state_machine.cpp",
            "src/modules/obd/obd_api_service.cpp",
            "src/modules/wifi/wifi_autopush_api_service.cpp",
            "src/modules/wifi/wifi_v1_profile_api_service.cpp",
        ),
        define="V1_LINKED_TEST_API_MAINTENANCE_RUNTIME_MATRIX",
        library_include_dirs=("ArduinoJson/src",),
    ),
    "test_ble_proxy_alloc": LinkedNativeTestSpec(
        sources=("src/ble_proxy.cpp",),
        define="V1_LINKED_TEST_BLE_PROXY_ALLOC",
        library_include_dirs=("ArduinoJson/src",),
    ),
    "test_ble_notification_delay_gate": LinkedNativeTestSpec(
        sources=("src/modules/ble/ble_notification_delay_gate.cpp",),
        define="V1_LINKED_TEST_BLE_NOTIFICATION_DELAY_GATE",
    ),
    "test_alp_event_latch": LinkedNativeTestSpec(
        sources=("src/modules/alp/alp_event_latch.cpp",),
        define="V1_LINKED_TEST_ALP_EVENT_LATCH",
    ),
    "test_obd_ble_client_race": LinkedNativeTestSpec(
        sources=("src/modules/obd/obd_ble_client.cpp",),
        define="V1_LINKED_TEST_OBD_BLE_CLIENT",
    ),
    "test_obd_transport_operation_barrier": LinkedNativeTestSpec(
        sources=("src/modules/obd/obd_transport_operation_barrier.cpp",),
        define="V1_LINKED_TEST_OBD_TRANSPORT_OPERATION_BARRIER",
    ),
    "test_obd_physical_link_preownership_barrier": LinkedNativeTestSpec(
        sources=("src/modules/obd/obd_physical_link_preownership_barrier.cpp",),
        define="V1_LINKED_TEST_OBD_PHYSICAL_LINK_PREOWNERSHIP_BARRIER",
    ),
    "test_wifi_scan_result_owner": LinkedNativeTestSpec(
        sources=("src/modules/wifi/wifi_scan_result_owner.cpp",),
        define="V1_LINKED_TEST_WIFI_SCAN_RESULT_OWNER",
    ),
    "test_wifi_client_enable_transaction": LinkedNativeTestSpec(
        sources=("src/modules/wifi/wifi_client_enable_transaction.cpp",),
        define="V1_LINKED_TEST_WIFI_CLIENT_ENABLE_TRANSACTION",
    ),
}


def linked_test_names() -> tuple[str, ...]:
    return tuple(sorted(LINKED_NATIVE_TEST_SPECS))


def linked_test_spec(test_name: str) -> LinkedNativeTestSpec:
    """Return an allow-listed spec; unknown suites are always rejected."""
    try:
        return LINKED_NATIVE_TEST_SPECS[test_name]
    except KeyError as exc:
        raise ValueError(
            f"native linked-source test suite is not allow-listed: {test_name!r}"
        ) from exc


def validate_manifest(project_root: Path) -> None:
    if not LINKED_NATIVE_TEST_SPECS:
        raise ValueError("native linked-source manifest must not be empty")

    seen_sources: set[str] = set()
    for test_name, spec in LINKED_NATIVE_TEST_SPECS.items():
        if not test_name.startswith("test_") or "/" in test_name or "\\" in test_name:
            raise ValueError(f"invalid native test suite name: {test_name!r}")
        if not spec.sources:
            raise ValueError(f"linked-source suite has no sources: {test_name}")
        if not spec.define.startswith("V1_LINKED_TEST_"):
            raise ValueError(f"invalid suite-specific linked-test define: {spec.define!r}")

        suite_dir = project_root / "test" / test_name
        if not suite_dir.is_dir():
            raise ValueError(f"allow-listed test suite does not exist: {suite_dir}")

        for source in spec.sources:
            source_path = PurePosixPath(source)
            if (
                source_path.is_absolute()
                or ".." in source_path.parts
                or not source_path.parts
                or source_path.parts[0] != "src"
                or source_path.suffix != ".cpp"
            ):
                raise ValueError(f"invalid linked production source path: {source!r}")
            if source in seen_sources:
                raise ValueError(f"production source is allow-listed more than once: {source}")
            seen_sources.add(source)
            if not (project_root / source).is_file():
                raise ValueError(f"allow-listed production source does not exist: {source}")

        for include_dir in spec.library_include_dirs:
            include_path = PurePosixPath(include_dir)
            if include_path.is_absolute() or ".." in include_path.parts or not include_path.parts:
                raise ValueError(f"invalid linked library include path: {include_dir!r}")

    try:
        linked_test_spec("__manifest_fail_closed_probe__")
    except ValueError:
        pass
    else:
        raise ValueError("linked-source manifest did not reject an unknown suite")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Validate every allow-listed suite and production source.",
    )
    parser.add_argument(
        "--suite",
        help="Print the allow-listed production sources for one exact suite name.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.check and not args.suite:
        print("one of --check or --suite is required", file=sys.stderr)
        return 2

    project_root = Path(__file__).resolve().parents[1]
    try:
        validate_manifest(project_root)
        if args.suite:
            spec = linked_test_spec(args.suite)
            print("\n".join(spec.sources))
    except ValueError as exc:
        print(f"native linked-source manifest error: {exc}", file=sys.stderr)
        return 2

    if args.check:
        print(
            "Native linked-source manifest OK: "
            f"{len(LINKED_NATIVE_TEST_SPECS)} suite(s), {len(seen_sources())} source(s)"
        )
    return 0


def seen_sources() -> set[str]:
    return {
        source
        for spec in LINKED_NATIVE_TEST_SPECS.values()
        for source in spec.sources
    }


if __name__ == "__main__":
    sys.exit(main())
