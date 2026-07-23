#!/usr/bin/env python3
"""Regression tests for the fail-closed rig-adapter registry and admission."""

from __future__ import annotations

import argparse
import builtins
from dataclasses import FrozenInstanceError, replace
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import bug_squash_hil_case_drivers as case_drivers
import bug_squash_hil_bsc16_rig as bsc16_rig
import bug_squash_hil_rig_adapters as adapters
import check_bug_squash_hil_qualification as qualification
import run_bug_squash_hil as runner


class RigAdapterRegistryTests(unittest.TestCase):
    def test_registry_is_exact_immutable_and_only_bsc16_is_implemented(self) -> None:
        self.assertEqual(adapters.CASE_IDS, case_drivers.CASE_IDS)
        self.assertEqual(tuple(adapters.ADAPTER_BY_CASE), adapters.CASE_IDS)
        self.assertEqual(len(adapters.ADAPTERS), 14)
        for adapter in adapters.ADAPTERS:
            with self.subTest(case_id=adapter.case_id):
                if adapter.case_id == "BSC-16":
                    self.assertTrue(adapter.implemented)
                    self.assertEqual(adapter.status, "implemented")
                    self.assertEqual(
                        adapter.source_path,
                        "scripts/bug_squash_hil_bsc16_rig.py",
                    )
                    self.assertEqual(adapter.entrypoint, "main")
                else:
                    self.assertFalse(adapter.implemented)
                    self.assertEqual(adapter.status, "unavailable")
                    self.assertIsNone(adapter.source_path)
                    self.assertIsNone(adapter.entrypoint)
                self.assertEqual(adapter.protocol_version, adapters.ADAPTER_PROTOCOL_VERSION)
                for role in adapter.roles:
                    expected_artifacts = adapters.RAW_ARTIFACTS_BY_CASE.get(
                        adapter.case_id,
                        adapters.RAW_ARTIFACTS,
                    )
                    self.assertEqual(role.raw_artifacts, expected_artifacts)
        with self.assertRaises(FrozenInstanceError):
            adapters.ADAPTERS[0].status = "implemented"  # type: ignore[misc]
        with self.assertRaises(TypeError):
            adapters.ADAPTER_BY_CASE["BSC-02"] = adapters.ADAPTERS[0]  # type: ignore[index]

    def test_bsc07_capture_contract_has_exact_power_safety_roles(self) -> None:
        role = adapters.get_rig_adapter("BSC-07").roles[0]
        self.assertEqual(
            tuple((item.role, item.filename) for item in role.raw_artifacts),
            (
                ("ap-traffic", "ap-traffic.json"),
                ("firmware-build", "firmware-build.json"),
                ("power-timeline", "power-timeline.json"),
                ("reset-summary", "reset-summary.json"),
                ("serial-log", "serial.log"),
                ("ui-health", "ui-health.json"),
            ),
        )

    def test_bsc12_capture_contract_has_exact_case_specific_limits(self) -> None:
        role = adapters.get_rig_adapter("BSC-12").roles[0]
        self.assertEqual(
            tuple((item.role, item.filename, item.maximum_bytes) for item in role.raw_artifacts),
            (
                ("firmware-build", "firmware-build.json", 1024 * 1024),
                ("persistence-after", "persistence-after.json", 512 * 1024),
                ("persistence-before", "persistence-before.json", 512 * 1024),
                ("power-reset-trace", "power-reset-trace.json", 2 * 1024 * 1024),
                ("serial-log", "serial.log", 16 * 1024 * 1024),
                ("shutdown-timeline", "shutdown-timeline.json", 2 * 1024 * 1024),
                ("wake-input-trace", "wake-input-trace.json", 2 * 1024 * 1024),
            ),
        )

    def test_bsc16_capture_contract_has_exact_power_evidence_roles(self) -> None:
        for role in adapters.get_rig_adapter("BSC-16").roles:
            self.assertEqual(
                tuple((item.role, item.filename) for item in role.raw_artifacts),
                (
                    ("build-evidence", "build-evidence.json"),
                    ("logic-analyzer", "logic-analyzer-capture"),
                    ("poweroff-log", "poweroff.log"),
                    ("serial-log", "serial.log"),
                    ("source-transitions", "source-transitions.ndjson"),
                ),
            )

    def test_bsc09_capture_contract_has_exact_case_specific_limits(self) -> None:
        role = adapters.get_rig_adapter("BSC-09").roles[0]
        self.assertEqual(
            tuple((item.role, item.filename, item.maximum_bytes) for item in role.raw_artifacts),
            (
                ("browser-projection", "browser-projection.json", 2 * 1024 * 1024),
                ("case-observation", "case-observation.json", 2 * 1024 * 1024),
                ("firmware-build", "firmware-build.json", 1024 * 1024),
                ("health-projection", "health-projection.json", 1024 * 1024),
                ("heap-projection", "heap-projection.json", 2 * 1024 * 1024),
                ("wifi-mode-projection", "wifi-mode-projection.json", 1024 * 1024),
                ("wifi-scan-projection", "wifi-scan-projection.json", 2 * 1024 * 1024),
            ),
        )
        self.assertIsNone(adapters.get_rig_adapter("BSC-09").source_path)
        self.assertIsNone(adapters.get_rig_adapter("BSC-09").entrypoint)

    def test_every_profile_role_run_and_capability_contract_matches(self) -> None:
        profile, errors = qualification.load_pinned_profile()
        self.assertEqual(errors, [])
        assert profile is not None
        for case in profile["required_cases"]:
            adapter = adapters.get_rig_adapter(case["id"])
            expected_roles = [
                (case["scenario"]["role_id"], case["scenario"]["build_kind"])
            ]
            if case["production_replay"] is not None:
                expected_roles.append(
                    (
                        case["production_replay"]["role_id"],
                        case["production_replay"]["build_kind"],
                    )
                )
            self.assertEqual(adapter.minimum_runs, case["minimum_runs"])
            self.assertEqual(
                adapter.required_dut_capabilities,
                tuple(case["required_dut_capabilities"]),
            )
            self.assertEqual(
                adapter.required_rig_capabilities,
                tuple(case["required_rig_capabilities"]),
            )
            self.assertEqual(
                tuple((role.role_id, role.build_kind) for role in adapter.roles),
                tuple(expected_roles),
            )

    def test_case_driver_handler_and_adapter_registries_agree(self) -> None:
        handlers = runner.case_handler_map()
        self.assertEqual(tuple(handlers), case_drivers.implemented_entrypoints())
        for case_id in adapters.CASE_IDS:
            driver = case_drivers.get_case_driver(case_id)
            adapter = adapters.get_rig_adapter(case_id)
            self.assertEqual(driver.case_id, adapter.case_id)
            self.assertIs(runner.resolve_case_handler(driver), handlers[driver.entrypoint])

    def test_registry_rejects_duplicate_unknown_unsafe_and_inconsistent_entries(self) -> None:
        first = adapters.ADAPTERS[0]
        second = adapters.ADAPTERS[1]
        mutations = (
            tuple(reversed(adapters.ADAPTERS)),
            (first, replace(second, adapter_id=first.adapter_id), *adapters.ADAPTERS[2:]),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation[0].case_id), self.assertRaises(
                adapters.RigAdapterContractError
            ):
                adapters._validate_registry(mutation)

        raw = first.roles[0].raw_artifacts[0]
        invalid_descriptors = (
            replace(first, case_id="BSC-15"),
            replace(first, protocol_version=True),
            replace(first, minimum_runs=True),
            replace(first, required_dut_capabilities=("serial", "serial")),
            replace(first, required_rig_capabilities=("../unsafe",)),
            replace(first, source_path="scripts/adapter.py"),
            replace(first, status="implemented"),
            replace(
                first,
                roles=(replace(first.roles[0], firmware_environment="waveshare-349"),),
            ),
            replace(
                first,
                roles=(
                    replace(
                        first.roles[0],
                        raw_artifacts=(replace(raw, filename="../serial.log"),),
                    ),
                ),
            ),
            replace(
                first,
                roles=(
                    replace(
                        first.roles[0],
                        raw_artifacts=(replace(raw, maximum_bytes=adapters.MAX_RAW_ARTIFACT_BYTES + 1),),
                    ),
                ),
            ),
        )
        for mutation in invalid_descriptors:
            with self.subTest(adapter=mutation), self.assertRaises(
                adapters.RigAdapterContractError
            ):
                adapters.validate_adapter_descriptor(mutation)
        with self.assertRaises(adapters.RigAdapterContractError):
            adapters.get_rig_adapter("BSC-15")

    def test_shared_admission_blocks_before_discovery_and_preserves_simulation(self) -> None:
        profile, errors = qualification.load_pinned_profile()
        self.assertEqual(errors, [])
        assert profile is not None
        case = next(item for item in profile["required_cases"] if item["id"] == "BSC-10")
        role_id = case["scenario"]["role_id"]
        args = argparse.Namespace(case_adapter=None)
        with mock.patch.object(runner, "test_hooks_enabled", return_value=False), mock.patch.object(
            runner, "read_git_state"
        ) as read_git, self.assertRaises(runner.RunnerError) as unavailable:
            runner.admit_case_rig_adapter(
                args,
                case_contract=case,
                role_id=role_id,
            )
        self.assertEqual(unavailable.exception.code, "case_rig_adapter_unavailable")
        read_git.assert_not_called()

        args.case_adapter = Path("untracked-adapter")
        with mock.patch.object(runner, "test_hooks_enabled", return_value=False), self.assertRaises(
            runner.RunnerError
        ) as override:
            runner.admit_case_rig_adapter(args, case_contract=case, role_id=role_id)
        self.assertEqual(override.exception.code, "untrusted_override")

        with mock.patch.object(runner, "test_hooks_enabled", return_value=True):
            admitted = runner.admit_case_rig_adapter(
                args,
                case_contract=case,
                role_id=role_id,
            )
        self.assertTrue(admitted.simulated)
        self.assertFalse(admitted.adapter.implemented)
        self.assertIsNone(admitted.source_sha256)

        changed = dict(case)
        changed["minimum_runs"] = 2
        with mock.patch.object(runner, "test_hooks_enabled", return_value=False), self.assertRaises(
            runner.RunnerError
        ) as mismatch:
            runner.admit_case_rig_adapter(args, case_contract=changed, role_id=role_id)
        self.assertEqual(mismatch.exception.code, "adapter_contract_invalid")

    def test_future_implemented_source_is_bound_to_the_target_commit(self) -> None:
        profile, errors = qualification.load_pinned_profile()
        self.assertEqual(errors, [])
        assert profile is not None
        case = next(item for item in profile["required_cases"] if item["id"] == "BSC-02")
        tracked = replace(
            adapters.get_rig_adapter("BSC-02"),
            status="implemented",
            source_path="scripts/bug_squash_hil_bsc02_rig.py",
            entrypoint="main",
        )
        adapters.validate_adapter_descriptor(tracked)
        with tempfile.TemporaryDirectory() as raw:
            repository = Path(raw)
            source = repository / tracked.source_path
            source.parent.mkdir(parents=True)
            source_bytes = b"#!/usr/bin/env python3\nraise SystemExit(0)\n"
            source.write_bytes(source_bytes)
            subprocess.run(["git", "init", "-q"], cwd=repository, check=True)
            subprocess.run(["git", "add", tracked.source_path], cwd=repository, check=True)
            subprocess.run(
                [
                    "git",
                    "-c",
                    "user.name=Adapter Test",
                    "-c",
                    "user.email=adapter@example.invalid",
                    "commit",
                    "-q",
                    "-m",
                    "fixture",
                ],
                cwd=repository,
                check=True,
            )
            git_state = runner.read_git_state(repository)
            self.assertEqual(
                runner.verify_tracked_rig_adapter_source(repository, git_state, tracked),
                hashlib.sha256(source_bytes).hexdigest(),
            )

            args = argparse.Namespace(case_adapter=None, repo_root=repository)
            with mock.patch.object(runner, "test_hooks_enabled", return_value=False), mock.patch.object(
                adapters, "get_rig_adapter", return_value=tracked
            ):
                admission = runner.admit_case_rig_adapter(
                    args,
                    case_contract=case,
                    role_id=case["scenario"]["role_id"],
                )
            self.assertFalse(admission.simulated)
            self.assertEqual(admission.git_state, git_state)
            self.assertEqual(admission.source_sha256, hashlib.sha256(source_bytes).hexdigest())

            source.write_bytes(b"#!/usr/bin/env python3\nraise SystemExit(1)\n")
            with self.assertRaises(runner.RunnerError) as mismatch:
                runner.verify_tracked_rig_adapter_source(repository, git_state, tracked)
            self.assertEqual(mismatch.exception.code, "adapter_source_mismatch")

            source.write_bytes(source_bytes)
            outside = repository / "outside.py"
            outside.write_bytes(source_bytes)
            source.unlink()
            source.symlink_to(outside)
            with self.assertRaises(runner.RunnerError):
                runner.verify_tracked_rig_adapter_source(repository, git_state, tracked)

            source.unlink()
            source.write_bytes(source_bytes)
            hardlink = repository / "hardlink.py"
            os.link(source, hardlink)
            with self.assertRaises(runner.RunnerError) as linked:
                runner.verify_tracked_rig_adapter_source(repository, git_state, tracked)
            self.assertEqual(linked.exception.code, "adapter_source_invalid")

            untracked = replace(
                tracked,
                source_path="scripts/bug_squash_hil_untracked_rig.py",
            )
            (repository / untracked.source_path).write_bytes(source_bytes)
            with self.assertRaises(runner.RunnerError) as missing:
                runner.verify_tracked_rig_adapter_source(repository, git_state, untracked)
            self.assertEqual(missing.exception.code, "adapter_source_invalid")

    def test_bsc16_adapter_fails_closed_without_an_interactive_bench(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            artifact_root = Path(raw)
            completed = subprocess.run(
                [
                    str(Path(__file__).resolve().parent / "bug_squash_hil_bsc16_rig.py"),
                    "--case",
                    "BSC-16",
                    "--role",
                    "fault-collection",
                    "--session-id",
                    "session",
                    "--attempt-id",
                    "attempt",
                    "--target-sha",
                    "a" * 40,
                    "--dut-alias",
                    "dut",
                    "--rig-alias",
                    "rig",
                    "--serial-port",
                    "/not/a/device",
                    "--template",
                    "/not/a/template",
                    "--inventory",
                    "/not/an/inventory",
                    "--pio-command",
                    "pio",
                    "--artifact-dir",
                    str(artifact_root),
                    "--raw-artifact-request-sha256",
                    "b" * 64,
                ],
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(completed.stdout, b"")
            self.assertEqual(list(artifact_root.iterdir()), [])

    def test_bsc16_adapter_reresolves_exact_serial_after_device_renumber(self) -> None:
        endpoint_resolver = bsc16_rig.SerialEndpointResolver(
            template=Path("/template"),
            inventory=Path("/inventory"),
            dut_alias="dut",
            pio_command="pio",
        )
        records = ([{"port": "/first"}], [{"port": "/second"}])

        def resolve_board(
            _inventory: object,
            _alias: str,
            _capabilities: tuple[str, ...],
            *,
            port_records: list[dict[str, str]],
        ) -> dict[str, object]:
            return {"endpoints": {"serial_port": port_records[0]["port"]}}

        with mock.patch.object(
            bsc16_rig.resolve_hil_board,
            "load_inventory",
            return_value=object(),
        ), mock.patch.object(
            bsc16_rig.resolve_hil_board,
            "enumerate_serial_ports",
            side_effect=records,
        ) as enumerate_ports, mock.patch.object(
            bsc16_rig.resolve_hil_board,
            "resolve_board",
            side_effect=resolve_board,
        ):
            self.assertEqual(endpoint_resolver(), "/first")
            self.assertEqual(endpoint_resolver(), "/second")
        self.assertEqual(enumerate_ports.call_count, 2)

    def test_bsc16_serial_endpoint_disables_reset_lines_before_open(self) -> None:
        handle = mock.Mock()

        def assert_safe_open() -> None:
            self.assertFalse(handle.dtr)
            self.assertFalse(handle.rts)
            self.assertEqual(handle.port, "/resolved-port")

        handle.open.side_effect = assert_safe_open
        with mock.patch.object(bsc16_rig.serial, "Serial", return_value=handle) as serial_factory:
            self.assertIs(
                bsc16_rig.open_serial_endpoint("/resolved-port", 0.2),
                handle,
            )
        serial_factory.assert_called_once_with(port=None, baudrate=115200, timeout=0.2)
        handle.open.assert_called_once_with()

    def test_bsc16_hil_command_waits_for_serial_open_before_write(self) -> None:
        events: list[str] = []

        class SerialHandle:
            def __enter__(self) -> "SerialHandle":
                return self

            def __exit__(self, *_args: object) -> None:
                return None

            def reset_input_buffer(self) -> None:
                events.append("reset")

            def write(self, _content: bytes) -> None:
                events.append("write")

            def flush(self) -> None:
                events.append("flush")

            def readline(self) -> bytes:
                return b'{"ok":true,"parse":"ok","result":"ok","case_id":"BSC-16"}\n'

        with mock.patch.object(
            bsc16_rig,
            "open_serial_endpoint",
            return_value=SerialHandle(),
        ), mock.patch.object(
            bsc16_rig.time,
            "sleep",
            side_effect=lambda _seconds: events.append("settle"),
        ):
            rows: list[dict[str, object]] = []
            bsc16_rig.send_hil_commands(
                lambda: "/resolved-port",
                ("V1HIL STATUS",),
                run_started=bsc16_rig.time.monotonic(),
                serial_rows=rows,
            )
        self.assertEqual(events, ["settle", "reset", "write", "flush"])
        self.assertEqual(len(rows), 1)

    def test_bsc16_hil_sequence_is_sent_as_one_bounded_serial_burst(self) -> None:
        writes: list[bytes] = []

        class SerialHandle:
            def __enter__(self) -> "SerialHandle":
                return self

            def __exit__(self, *_args: object) -> None:
                return None

            def reset_input_buffer(self) -> None:
                return None

            def write(self, content: bytes) -> None:
                writes.append(content)

            def flush(self) -> None:
                return None

            def readline(self) -> bytes:
                return b'{"ok":true,"parse":"ok","result":"ok","case_id":"BSC-16"}\n'

        commands = ("V1HIL STATUS", "V1HIL STATUS")
        with mock.patch.object(
            bsc16_rig,
            "open_serial_endpoint",
            return_value=SerialHandle(),
        ), mock.patch.object(bsc16_rig.time, "sleep"):
            bsc16_rig.send_hil_commands(
                lambda: "/resolved-port",
                commands,
                run_started=bsc16_rig.time.monotonic(),
                serial_rows=[],
            )
        self.assertEqual(writes, [b"V1HIL STATUS\nV1HIL STATUS\n"])
        with mock.patch.object(bsc16_rig, "open_serial_endpoint") as serial_open, self.assertRaises(
            bsc16_rig.AdapterError
        ):
            bsc16_rig.send_hil_commands(
                lambda: "/resolved-port",
                ("X" * bsc16_rig.HIL_SERIAL_BURST_MAX_BYTES,),
                run_started=bsc16_rig.time.monotonic(),
                serial_rows=[],
            )
        serial_open.assert_not_called()

    def test_bsc16_logic_capture_is_measured_and_high_bounce_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            accepted = root / "accepted.csv"
            accepted.write_text(
                "Time (s),gpio16\n"
                "0.000,1\n"
                "0.100,0\n"
                "0.104,1\n"
                "0.108,0\n"
                "1.000,1\n",
                encoding="utf-8",
            )
            destination = root / "capture"
            with mock.patch.object(builtins, "input", return_value=str(accepted)):
                self.assertEqual(bsc16_rig.import_logic_capture(destination), 8)
            self.assertEqual(destination.read_bytes(), accepted.read_bytes())

            rejected = root / "rejected.csv"
            rejected.write_text(
                "Time (s),gpio16\n"
                "0.000,1\n"
                "0.100,0\n"
                "0.115,1\n"
                "0.130,0\n"
                "1.000,1\n",
                encoding="utf-8",
            )
            with mock.patch.object(builtins, "input", return_value=str(rejected)), self.assertRaises(
                bsc16_rig.AdapterError
            ):
                bsc16_rig.import_logic_capture(root / "must-not-exist")


if __name__ == "__main__":
    unittest.main()
