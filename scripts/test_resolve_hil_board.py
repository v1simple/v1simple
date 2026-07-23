#!/usr/bin/env python3
"""Deterministic regression tests for the local HIL board resolver."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import resolve_hil_board as resolver  # noqa: E402
import hil_board_inventory_test_support as inventory_test_support  # noqa: E402


def document(*boards: dict[str, object]) -> dict[str, object]:
    return {"schema_version": 1, "boards": list(boards)}


def board(
    alias: str = "bench-a",
    capabilities: list[str] | None = None,
    **connection: object,
) -> dict[str, object]:
    return {
        "alias": alias,
        "capabilities": capabilities if capabilities is not None else ["serial"],
        **connection,
    }


class TemporaryInventories:
    def __init__(self, template: object, local: object | None = None) -> None:
        self._directory = tempfile.TemporaryDirectory()
        directory = Path(self._directory.name)
        self.template = directory / "board_inventory.json"
        self.local = directory / "board_inventory.local.json"
        self.signing_key, self.trust_root = inventory_test_support.create_test_signer(
            directory
        )
        self.template.write_text(json.dumps(template), encoding="utf-8")
        if local is not None:
            self.local.write_text(json.dumps(local), encoding="utf-8")
            inventory_test_support.sign_inventory(self.local, self.signing_key)

    def load(self) -> resolver.Inventory:
        return resolver.load_inventory(
            self.template,
            self.local,
            trust_root_path=self.trust_root,
        )

    def environment(self) -> dict[str, str]:
        return inventory_test_support.test_environment(self.trust_root)

    def close(self) -> None:
        self._directory.cleanup()

    def __enter__(self) -> "TemporaryInventories":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


class InventoryValidationTests(unittest.TestCase):
    def assert_error(self, code: str, payload: object) -> resolver.ResolverError:
        with TemporaryInventories(payload) as files:
            with self.assertRaises(resolver.ResolverError) as raised:
                resolver.load_inventory(files.template)
        self.assertEqual(raised.exception.code, code)
        return raised.exception

    def test_tracked_inventory_does_not_require_a_local_overlay(self) -> None:
        with TemporaryInventories(document()) as files:
            inventory = resolver.load_inventory(files.template)
        self.assertEqual(inventory.boards, {})

    def test_explicit_local_inventory_must_exist_and_be_signed(self) -> None:
        with TemporaryInventories(document()) as files:
            with self.assertRaises(resolver.ResolverError) as missing:
                files.load()
        self.assertEqual(missing.exception.code, "inventory_missing")

        with TemporaryInventories(document(), document()) as files:
            Path(f"{files.local}.sig").unlink()
            with self.assertRaises(resolver.ResolverError) as unsigned:
                files.load()
        self.assertEqual(unsigned.exception.code, "inventory_signature_missing")

    def test_tampered_or_substituted_local_inventory_is_rejected(self) -> None:
        with TemporaryInventories(
            document(),
            document(board(alias="bench-a", usb_serial="SERIAL-A")),
        ) as files:
            files.local.write_text(
                json.dumps(document(board(alias="bench-a", usb_serial="TAMPERED"))),
                encoding="utf-8",
            )
            with self.assertRaises(resolver.ResolverError) as tampered:
                files.load()
        self.assertEqual(tampered.exception.code, "inventory_authentication_failed")

        with TemporaryInventories(
            document(),
            document(board(alias="bench-a", usb_serial="SERIAL-A")),
        ) as files:
            _other_key, other_root = inventory_test_support.create_test_signer(
                files.local.parent / "other"
            )
            with self.assertRaises(resolver.ResolverError) as substituted:
                resolver.load_inventory(
                    files.template,
                    files.local,
                    trust_root_path=other_root,
                )
        self.assertEqual(substituted.exception.code, "inventory_authentication_failed")

    def test_duplicate_json_keys_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "inventory.json"
            path.write_text(
                '{"schema_version":1,"schema_version":1,"boards":[]}\n',
                encoding="utf-8",
            )
            with self.assertRaises(resolver.ResolverError) as raised:
                resolver.load_inventory(path)
        self.assertEqual(raised.exception.code, "inventory_invalid_json")

    def test_live_port_inventory_duplicate_json_keys_are_rejected(self) -> None:
        completed = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='{"ports":[],"ports":[]}\n',
        )
        with mock.patch.object(resolver.subprocess, "run", return_value=completed):
            with self.assertRaises(resolver.ResolverError) as raised:
                resolver.enumerate_serial_ports()
        self.assertEqual(raised.exception.code, "invalid_port_inventory")

    def test_local_board_replaces_same_template_alias(self) -> None:
        tracked = document(board(usb_serial="TRACKED"))
        local = document(board(capabilities=["lan"], lan_base_url="http://192.0.2.10/"))
        with TemporaryInventories(tracked, local) as files:
            inventory = files.load()
        selected = inventory.boards["bench-a"]
        self.assertEqual(selected.capabilities, ("lan",))
        self.assertIsNone(selected.usb_serial)
        self.assertEqual(selected.lan_base_url, "http://192.0.2.10")

    def test_local_board_is_added_to_template_boards(self) -> None:
        tracked = document(board(alias="bench-a", usb_serial="SERIAL-A"))
        local = document(board(alias="bench-b", usb_serial="SERIAL-B"))
        with TemporaryInventories(tracked, local) as files:
            inventory = files.load()
        self.assertEqual(set(inventory.boards), {"bench-a", "bench-b"})

    def test_schema_version_rejects_boolean_and_unknown_versions(self) -> None:
        for version in (True, 0, 2, "1"):
            with self.subTest(version=version):
                self.assert_error("unsupported_schema", {"schema_version": version, "boards": []})

    def test_old_operational_fields_are_rejected(self) -> None:
        payload = document(
            {
                "alias": "bench-a",
                "capabilities": ["serial"],
                "usb_serial": "SERIAL-A",
                "device_path": "/dev/first-device",
                "metrics_url": "http://192.0.2.10/metrics",
            }
        )
        self.assert_error("invalid_board", payload)

    def test_aliases_are_unique_lowercase_slugs(self) -> None:
        invalid = ("", "Bench A", "../bench", "-bench", "a" * 65, 7, None)
        for alias in invalid:
            with self.subTest(alias=alias):
                self.assert_error(
                    "invalid_alias",
                    document(board(alias=alias, usb_serial="SERIAL-A")),  # type: ignore[arg-type]
                )
        self.assert_error(
            "duplicate_alias",
            document(
                board(alias="bench-a", usb_serial="SERIAL-A"),
                board(alias="bench-a", usb_serial="SERIAL-B"),
            ),
        )

    def test_capabilities_are_nonempty_unique_lowercase_slugs(self) -> None:
        invalid = ([], ["Serial"], ["serial port"], ["-serial"], ["serial", "serial"], "serial")
        expected = (
            "invalid_capabilities",
            "invalid_capabilities",
            "invalid_capabilities",
            "invalid_capabilities",
            "duplicate_capability",
            "invalid_capabilities",
        )
        for capabilities, code in zip(invalid, expected):
            with self.subTest(capabilities=capabilities):
                payload = document(
                    board(
                        capabilities=capabilities,  # type: ignore[arg-type]
                        usb_serial="SERIAL-A",
                    )
                )
                self.assert_error(code, payload)

    def test_serial_capability_and_usb_identity_must_agree(self) -> None:
        self.assert_error("missing_usb_identity", document(board(capabilities=["serial"])))
        self.assert_error(
            "unused_usb_identity",
            document(board(capabilities=["display"], usb_serial="SERIAL-A")),
        )
        self.assert_error(
            "invalid_usb_identity",
            document(board(capabilities=["serial"], usb_serial="REPLACE_WITH_USB_SERIAL")),
        )

    def test_duplicate_usb_identity_is_rejected_without_echoing_identity(self) -> None:
        secret_identity = "LOCAL-SERIAL-DO-NOT-PRINT"
        error = self.assert_error(
            "duplicate_usb_identity",
            document(
                board(alias="bench-a", usb_serial=secret_identity),
                board(alias="bench-b", usb_serial=secret_identity),
            ),
        )
        self.assertNotIn(secret_identity, error.message)

    def test_lan_url_must_be_safe_absolute_base_url(self) -> None:
        invalid = (
            "ftp://192.0.2.1",
            "http://",
            "http://user:pass@192.0.2.1",
            "http://192.0.2.1?token=x",
            "http://192.0.2.1/#fragment",
            "http://192.0.2.1:99999",
            "http://host name",
            "http://host.local/path name",
            "http://-host.local",
            "http://127.0.0.1",
            "http://255.255.255.255",
        )
        for url in invalid:
            with self.subTest(url=url):
                self.assert_error(
                    "invalid_lan_base_url",
                    document(board(capabilities=["lan"], lan_base_url=url)),
                )
        self.assert_error(
            "unused_lan_base_url",
            document(board(capabilities=["display"], lan_base_url="http://192.0.2.1")),
        )


class SerialResolutionTests(unittest.TestCase):
    def test_exact_usb_serial_match_is_order_independent(self) -> None:
        records = [
            {"port": "/dev/wrong", "serial_number": "SERIAL-B"},
            {"port": "/dev/right", "serial_number": "SERIAL-A"},
        ]
        self.assertEqual(resolver.select_serial_port(records, "SERIAL-A"), "/dev/right")
        self.assertEqual(resolver.select_serial_port(reversed(records), "SERIAL-A"), "/dev/right")

    def test_never_falls_back_to_only_or_first_enumerated_port(self) -> None:
        records = [
            {"port": "/dev/first", "serial_number": "OTHER"},
            {"port": "/dev/unidentified"},
        ]
        with self.assertRaisesRegex(resolver.ResolverError, "not connected") as raised:
            resolver.select_serial_port(records, "SERIAL-A")
        self.assertEqual(raised.exception.code, "serial_board_not_connected")

    def test_platformio_hwid_serial_is_supported(self) -> None:
        records = [
            {
                "port": "/dev/cu.usbmodem1",
                "hwid": "USB VID:PID=303A:1001 SER=SERIAL-A LOCATION=1-1",
            }
        ]
        self.assertEqual(resolver.select_serial_port(records, "SERIAL-A"), "/dev/cu.usbmodem1")

    def test_single_macos_callout_node_wins_over_paired_tty_node(self) -> None:
        records = [
            {"port": "/dev/tty.usbmodem1", "serial_number": "SERIAL-A"},
            {"port": "/dev/cu.usbmodem1", "serial_number": "SERIAL-A"},
        ]
        self.assertEqual(resolver.select_serial_port(records, "SERIAL-A"), "/dev/cu.usbmodem1")

    def test_multiple_nonpaired_matches_are_ambiguous(self) -> None:
        records = [
            {"port": "/dev/ttyACM0", "serial_number": "SERIAL-A"},
            {"port": "/dev/ttyACM1", "serial_number": "SERIAL-A"},
        ]
        with self.assertRaises(resolver.ResolverError) as raised:
            resolver.select_serial_port(records, "SERIAL-A")
        self.assertEqual(raised.exception.code, "serial_board_ambiguous")

    def test_macos_pair_plus_extra_same_identity_is_ambiguous(self) -> None:
        records = [
            {"port": "/dev/cu.usbmodem1", "serial_number": "SERIAL-A"},
            {"port": "/dev/tty.usbmodem1", "serial_number": "SERIAL-A"},
            {"port": "/dev/ttyACM9", "serial_number": "SERIAL-A"},
        ]
        with self.assertRaises(resolver.ResolverError) as raised:
            resolver.select_serial_port(records, "SERIAL-A")
        self.assertEqual(raised.exception.code, "serial_board_ambiguous")

    def test_mismatched_macos_callin_and_callout_nodes_are_ambiguous(self) -> None:
        records = [
            {"port": "/dev/cu.usbmodem1", "serial_number": "SERIAL-A"},
            {"port": "/dev/tty.usbmodem2", "serial_number": "SERIAL-A"},
        ]
        with self.assertRaises(resolver.ResolverError) as raised:
            resolver.select_serial_port(records, "SERIAL-A")
        self.assertEqual(raised.exception.code, "serial_board_ambiguous")

    def test_port_payload_accepts_list_or_ports_wrapper_only(self) -> None:
        record = {"port": "/dev/right", "serial_number": "SERIAL-A"}
        self.assertEqual(resolver.parse_port_records([record]), (record,))
        self.assertEqual(resolver.parse_port_records({"ports": [record]}), (record,))
        for invalid in ({}, {"ports": {}}, ["not-an-object"]):
            with self.subTest(invalid=invalid), self.assertRaises(resolver.ResolverError):
                resolver.parse_port_records(invalid)


class LanResolutionTests(unittest.TestCase):
    def test_parser_uses_most_recent_exact_firmware_ip_line(self) -> None:
        serial_text = "\n".join(
            [
                "booting",
                "[WiFiClient] Connected! IP: invalid-old-address",
                "unrelated IP: 203.0.113.1",
                "[WiFiClient] Connected! IP: 192.0.2.11",
            ]
        )
        self.assertEqual(resolver.parse_firmware_lan_base_url(serial_text), "http://192.0.2.11")

    def test_parser_rejects_invalid_unusable_and_missing_addresses(self) -> None:
        cases = (
            ("[WiFiClient] Connected! IP: 999.1.1.1", "invalid_firmware_ip"),
            ("[WiFiClient] Connected! IP: 0.0.0.0", "invalid_firmware_ip"),
            ("[WiFiClient] Connected! IP: 127.0.0.1", "invalid_firmware_ip"),
            ("[WiFiClient] Connected! IP: 224.0.0.1", "invalid_firmware_ip"),
            ("[WiFiClient] Connected! IP: 255.255.255.255", "invalid_firmware_ip"),
            ("timestamp [WiFiClient] Connected! IP: 192.0.2.10", "firmware_ip_not_found"),
            ("IP: 192.0.2.10", "firmware_ip_not_found"),
        )
        for serial_text, code in cases:
            with self.subTest(serial_text=serial_text), self.assertRaises(resolver.ResolverError) as raised:
                resolver.parse_firmware_lan_base_url(serial_text)
            self.assertEqual(raised.exception.code, code)


class BoardResolutionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.usb_serial = "LOCAL-USB-IDENTITY"
        self.inventory = resolver.Inventory(
            boards={
                "bench-a": resolver.Board(
                    alias="bench-a",
                    capabilities=("device-tests", "display", "lan", "serial"),
                    usb_serial=self.usb_serial,
                    lan_base_url="http://192.0.2.10",
                )
            }
        )

    def test_resolution_emits_only_requested_capabilities_and_endpoints(self) -> None:
        resolved = resolver.resolve_board(
            self.inventory,
            "bench-a",
            ["serial", "device-tests", "serial"],
            port_records=[{"port": "/dev/right", "serial_number": self.usb_serial}],
        )
        self.assertEqual(
            resolved,
            {
                "schema_version": 1,
                "alias": "bench-a",
                "capabilities": ["device-tests", "serial"],
                "endpoints": {"serial_port": "/dev/right"},
            },
        )
        serialized = json.dumps(resolved)
        self.assertNotIn(self.usb_serial, serialized)
        self.assertNotIn("display", serialized)
        self.assertNotIn("lan_base_url", serialized)

    def test_explicit_lan_url_does_not_require_or_parse_serial_log(self) -> None:
        resolved = resolver.resolve_board(
            self.inventory,
            "bench-a",
            ["lan"],
        )
        self.assertEqual(resolved["endpoints"], {"lan_base_url": "http://192.0.2.10"})

    def test_lan_url_can_come_from_firmware_serial_line(self) -> None:
        usb_serial = "LOCAL-USB-IDENTITY"
        dynamic = resolver.Inventory(
            boards={
                "bench-a": resolver.Board(
                    alias="bench-a",
                    capabilities=("lan", "serial"),
                    usb_serial=usb_serial,
                )
            }
        )
        collected_ports: list[str] = []
        resolved = resolver.resolve_board(
            dynamic,
            "bench-a",
            ["lan", "serial"],
            port_records=[{"port": "/dev/bench-a", "serial_number": usb_serial}],
            lan_collector=lambda port: (
                collected_ports.append(port) or "http://192.0.2.15"
            ),
        )
        self.assertEqual(
            resolved["endpoints"],
            {"lan_base_url": "http://192.0.2.15", "serial_port": "/dev/bench-a"},
        )
        self.assertEqual(collected_ports, ["/dev/bench-a"])

    def test_dynamic_lan_collector_receives_only_resolved_board_port(self) -> None:
        dynamic = resolver.Inventory(
            boards={
                "bench-a": resolver.Board(
                    alias="bench-a",
                    capabilities=("lan", "serial"),
                    usb_serial="SERIAL-A",
                ),
                "bench-b": resolver.Board(
                    alias="bench-b",
                    capabilities=("lan", "serial"),
                    usb_serial="SERIAL-B",
                ),
            }
        )
        ports = [
            {"port": "/dev/bench-a", "serial_number": "SERIAL-A"},
            {"port": "/dev/bench-b", "serial_number": "SERIAL-B"},
        ]
        collected_ports: list[str] = []
        resolver.resolve_board(
            dynamic,
            "bench-a",
            ["lan", "serial"],
            port_records=ports,
            lan_collector=lambda port: (
                collected_ports.append(port) or "http://192.0.2.15"
            ),
        )
        self.assertEqual(collected_ports, ["/dev/bench-a"])

    def test_dynamic_lan_requires_serial_in_same_resolution(self) -> None:
        dynamic = resolver.Inventory(
            boards={"bench-a": resolver.Board(alias="bench-a", capabilities=("lan",))}
        )
        with self.assertRaises(resolver.ResolverError) as raised:
            resolver.resolve_board(
                dynamic,
                "bench-a",
                ["lan"],
                lan_collector=lambda _port: "http://192.0.2.15",
            )
        self.assertEqual(raised.exception.code, "dynamic_lan_requires_serial")

    def test_dynamic_lan_requires_resolver_owned_collector(self) -> None:
        dynamic = resolver.Inventory(
            boards={
                "bench-a": resolver.Board(
                    alias="bench-a",
                    capabilities=("lan", "serial"),
                    usb_serial="SERIAL-A",
                )
            }
        )
        with self.assertRaises(resolver.ResolverError) as raised:
            resolver.resolve_board(
                dynamic,
                "bench-a",
                ["lan", "serial"],
                port_records=[
                    {"port": "/dev/bench-a", "serial_number": "SERIAL-A"}
                ],
            )
        self.assertEqual(raised.exception.code, "dynamic_lan_collector_required")

    def test_missing_alias_capability_or_lan_endpoint_fails_closed(self) -> None:
        cases = (
            ("missing", ["serial"], "unknown_alias"),
            ("bench-a", [], "missing_capability"),
            ("bench-a", ["radio"], "capability_unavailable"),
        )
        for alias, capabilities, code in cases:
            with self.subTest(code=code), self.assertRaises(resolver.ResolverError) as raised:
                resolver.resolve_board(self.inventory, alias, capabilities)
            self.assertEqual(raised.exception.code, code)

        dynamic = resolver.Inventory(
            boards={"bench-a": resolver.Board(alias="bench-a", capabilities=("lan",))}
        )
        with self.assertRaises(resolver.ResolverError) as raised:
            resolver.resolve_board(dynamic, "bench-a", ["lan"])
        self.assertEqual(raised.exception.code, "dynamic_lan_requires_serial")

    def test_non_string_requested_capability_fails_closed(self) -> None:
        with self.assertRaises(resolver.ResolverError) as raised:
            resolver.resolve_board(
                self.inventory,
                "bench-a",
                [7],  # type: ignore[list-item]
            )
        self.assertEqual(raised.exception.code, "invalid_capability")


class AttestationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.resolution = {
            "schema_version": 1,
            "alias": "dut-primary",
            "capabilities": ["device-tests", "lan", "serial"],
            "endpoints": {
                "lan_base_url": "http://192.0.2.15",
                "serial_port": "/dev/synthetic-port",
            },
        }
        self.observed_at = "2026-07-21T20:00:00Z"

    def test_attestation_is_exact_sanitized_schema(self) -> None:
        attestation = resolver.build_resolution_attestation(
            self.resolution,
            observed_at_utc=self.observed_at,
        )
        self.assertEqual(
            set(attestation),
            {
                "schema_version",
                "resolver_schema_version",
                "alias",
                "capabilities",
                "resolution_sha256",
                "observed_at_utc",
            },
        )
        self.assertEqual(attestation["schema_version"], 1)
        self.assertEqual(attestation["resolver_schema_version"], 1)
        self.assertEqual(attestation["alias"], "dut-primary")
        self.assertEqual(
            attestation["capabilities"],
            ["device-tests", "lan", "serial"],
        )
        self.assertEqual(attestation["observed_at_utc"], self.observed_at)
        serialized = json.dumps(attestation, sort_keys=True)
        self.assertNotIn("192.0.2.15", serialized)
        self.assertNotIn("/dev/", serialized)
        self.assertNotIn("lan_base_url", serialized)
        self.assertNotIn("serial_port", serialized)

    def test_attestation_hash_is_canonical_stable_and_resolution_bound(self) -> None:
        reordered = {
            "endpoints": {
                "serial_port": "/dev/synthetic-port",
                "lan_base_url": "http://192.0.2.15",
            },
            "capabilities": ["device-tests", "lan", "serial"],
            "alias": "dut-primary",
            "schema_version": 1,
        }
        first = resolver.build_resolution_attestation(
            self.resolution,
            observed_at_utc=self.observed_at,
        )
        second = resolver.build_resolution_attestation(
            reordered,
            observed_at_utc=self.observed_at,
        )
        self.assertEqual(first["resolution_sha256"], second["resolution_sha256"])

        changed = copy.deepcopy(self.resolution)
        changed["endpoints"]["lan_base_url"] = "http://192.0.2.16"
        third = resolver.build_resolution_attestation(
            changed,
            observed_at_utc=self.observed_at,
        )
        self.assertNotEqual(first["resolution_sha256"], third["resolution_sha256"])

    def test_attestation_rejects_schema_capability_and_time_drift(self) -> None:
        cases = (
            ({**self.resolution, "unexpected": True}, self.observed_at, "invalid_resolution"),
            (
                {**self.resolution, "capabilities": ["serial", "lan"]},
                self.observed_at,
                "invalid_resolution",
            ),
            (self.resolution, "2026-07-21T16:00:00-04:00", "invalid_attestation_time"),
        )
        for resolution, observed_at, expected_code in cases:
            with self.subTest(expected_code=expected_code):
                with self.assertRaises(resolver.ResolverError) as raised:
                    resolver.build_resolution_attestation(
                        resolution,
                        observed_at_utc=observed_at,
                    )
                self.assertEqual(raised.exception.code, expected_code)


class CliTests(unittest.TestCase):
    def test_local_outputs_create_parents_but_never_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "nested" / "attestation.json"
            resolver._write_local_json(path, {"safe": True}, "write_failed")
            original = path.read_text(encoding="utf-8")
            with self.assertRaises(resolver.ResolverError) as raised:
                resolver._write_local_json(path, {"safe": False}, "write_failed")
            self.assertEqual(raised.exception.code, "write_failed")
            self.assertEqual(path.read_text(encoding="utf-8"), original)

    def test_cli_success_is_machine_readable_and_sanitized(self) -> None:
        usb_serial = "LOCAL-USB-IDENTITY"
        local = document(
            board(
                alias="bench-a",
                capabilities=["device-tests", "serial"],
                usb_serial=usb_serial,
            )
        )
        with TemporaryInventories(document(), local) as files:
            ports_path = files.local.parent / "ports.json"
            ports_path.write_text(
                json.dumps([{"port": "/dev/right", "serial_number": usb_serial}]),
                encoding="utf-8",
            )
            completed = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "resolve_hil_board.py"),
                    "bench-a",
                    "--capability",
                    "device-tests",
                    "--capability",
                    "serial",
                    "--template",
                    str(files.template),
                    "--inventory",
                    str(files.local),
                    "--ports-json",
                    str(ports_path),
                ],
                cwd=ROOT,
                env=files.environment(),
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        output = json.loads(completed.stdout)
        self.assertEqual(output["alias"], "bench-a")
        self.assertEqual(output["capabilities"], ["device-tests", "serial"])
        self.assertNotIn("endpoints", output)
        self.assertNotIn("/dev/right", completed.stdout)
        self.assertNotIn(usb_serial, completed.stdout)

    def test_cli_writes_raw_resolution_locally_but_stdout_is_attestation_only(self) -> None:
        local = document(
            board(
                alias="bench-a",
                capabilities=["lan"],
                lan_base_url="http://192.0.2.15",
            )
        )
        with TemporaryInventories(document(), local) as files:
            resolution_path = files.local.parent / "local-resolution.json"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "resolve_hil_board.py"),
                    "bench-a",
                    "--capability",
                    "lan",
                    "--template",
                    str(files.template),
                    "--inventory",
                    str(files.local),
                    "--local-resolution-output",
                    str(resolution_path),
                ],
                cwd=ROOT,
                env=files.environment(),
                capture_output=True,
                text=True,
                check=False,
            )
            raw_resolution = json.loads(resolution_path.read_text(encoding="utf-8"))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        output = json.loads(completed.stdout)
        self.assertNotIn("endpoints", output)
        self.assertNotIn("192.0.2.15", completed.stdout)
        self.assertEqual(
            raw_resolution["endpoints"],
            {"lan_base_url": "http://192.0.2.15"},
        )

    def test_cli_error_is_machine_readable_and_does_not_echo_usb_identity(self) -> None:
        usb_serial = "LOCAL-USB-IDENTITY"
        local = document(board(alias="bench-a", usb_serial=usb_serial))
        with TemporaryInventories(document(), local) as files:
            ports_path = files.local.parent / "ports.json"
            ports_path.write_text("[]", encoding="utf-8")
            completed = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "resolve_hil_board.py"),
                    "bench-a",
                    "--capability",
                    "serial",
                    "--template",
                    str(files.template),
                    "--inventory",
                    str(files.local),
                    "--ports-json",
                    str(ports_path),
                ],
                cwd=ROOT,
                env=files.environment(),
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(json.loads(completed.stderr)["error"]["code"], "serial_board_not_connected")
        self.assertNotIn(usb_serial, completed.stderr)

    def test_cli_writes_sanitized_attestation_without_raw_identifiers(self) -> None:
        usb_serial = "LOCAL-USB-IDENTITY"
        local = document(
            board(
                alias="dut-primary",
                capabilities=["device-tests", "serial"],
                usb_serial=usb_serial,
            )
        )
        with TemporaryInventories(document(), local) as files:
            ports_path = files.local.parent / "ports.json"
            ports_path.write_text(
                json.dumps(
                    [{"port": "/dev/synthetic-port", "serial_number": usb_serial}]
                ),
                encoding="utf-8",
            )
            attestation_path = files.local.parent / "resolver-attestation.json"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "resolve_hil_board.py"),
                    "dut-primary",
                    "--capability",
                    "device-tests",
                    "--capability",
                    "serial",
                    "--template",
                    str(files.template),
                    "--inventory",
                    str(files.local),
                    "--ports-json",
                    str(ports_path),
                    "--attestation-output",
                    str(attestation_path),
                ],
                cwd=ROOT,
                env=files.environment(),
                capture_output=True,
                text=True,
                check=False,
            )
            attestation_text = attestation_path.read_text(encoding="utf-8")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        attestation = json.loads(attestation_text)
        self.assertEqual(attestation["alias"], "dut-primary")
        self.assertEqual(attestation["capabilities"], ["device-tests", "serial"])
        self.assertNotIn(usb_serial, attestation_text)
        self.assertNotIn("/dev/", attestation_text)
        self.assertNotIn("serial_port", attestation_text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
