#!/usr/bin/env python3
"""Mutation tests for the HIL rig-adapter protocol and raw artifact hasher."""

from __future__ import annotations

import copy
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import bug_squash_hil_adapter_protocol as protocol
import bug_squash_hil_rig_adapters as adapters


def implemented_adapter() -> adapters.RigAdapter:
    value = replace(
        adapters.get_rig_adapter("BSC-10"),
        status="implemented",
        source_path="scripts/bug_squash_hil_bsc10_rig.py",
        entrypoint="main",
    )
    adapters.validate_adapter_descriptor(value)
    return value


def valid_request(adapter: adapters.RigAdapter) -> dict[str, object]:
    return protocol.build_adapter_request(
        adapter=adapter,
        target_sha="a" * 40,
        profile_id="bug-squash-hil-v1",
        profile_version=5,
        profile_sha256="b" * 64,
        adapter_source_sha256="c" * 64,
        role_id="wifi-enable-transaction-fault",
        run_index=1,
        session_id="session-0123456789abcdef",
        attempt_id="attempt-0123456789abcdef",
        nonce="nonce-0123456789abcdef",
        dut_alias="opaque-dut",
        dut_capabilities=adapter.required_dut_capabilities,
        rig_alias="opaque-rig",
        rig_capabilities=adapter.required_rig_capabilities,
        firmware_binary_sha256="d" * 64,
    )


def valid_response(request: dict[str, object]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "protocol_version": adapters.ADAPTER_PROTOCOL_VERSION,
        "status": "complete",
        "request_commitment_sha256": request["request_commitment_sha256"],
        "nonce": request["nonce"],
    }


def write_raw_set(directory: Path, role: adapters.AdapterRoleContract) -> None:
    directory.mkdir()
    for index, artifact in enumerate(role.raw_artifacts, start=1):
        (directory / artifact.filename).write_bytes(
            f"raw-{index}-{artifact.role}\n".encode("ascii")
        )


class AdapterProtocolTests(unittest.TestCase):
    def test_request_and_response_are_canonical_exact_and_nonce_bound(self) -> None:
        adapter = implemented_adapter()
        request = valid_request(adapter)
        encoded = protocol.canonical_json_bytes(request)
        self.assertEqual(
            protocol.parse_adapter_request(encoded, adapter=adapter, expected=request),
            request,
        )
        response = valid_response(request)
        self.assertEqual(
            protocol.parse_adapter_response(protocol.canonical_json_bytes(response), request=request),
            response,
        )
        with self.assertRaises(protocol.AdapterProtocolError):
            protocol.build_adapter_request(
                adapter=adapters.get_rig_adapter("BSC-10"),
                target_sha="a" * 40,
                profile_id="bug-squash-hil-v1",
                profile_version=5,
                profile_sha256="b" * 64,
                adapter_source_sha256="c" * 64,
                role_id="wifi-enable-transaction-fault",
                run_index=1,
                session_id="session-0123456789abcdef",
                attempt_id="attempt-0123456789abcdef",
                nonce="nonce-0123456789abcdef",
                dut_alias="opaque-dut",
                dut_capabilities=adapter.required_dut_capabilities,
                rig_alias="opaque-rig",
                rig_capabilities=adapter.required_rig_capabilities,
                firmware_binary_sha256="d" * 64,
            )

    def test_recomputed_request_commitment_cannot_substitute_runner_fields(self) -> None:
        adapter = implemented_adapter()
        expected = valid_request(adapter)

        def set_nested(path: tuple[object, ...], value: object):
            def mutate(payload: dict[str, object]) -> None:
                cursor: object = payload
                for component in path[:-1]:
                    assert isinstance(cursor, (dict, list))
                    cursor = cursor[component]  # type: ignore[index]
                assert isinstance(cursor, (dict, list))
                cursor[path[-1]] = value  # type: ignore[index]

            return mutate

        mutations = (
            set_nested(("target_sha",), "e" * 40),
            set_nested(("profile", "id"), "substituted-profile"),
            set_nested(("profile", "version"), 6),
            set_nested(("profile", "sha256"), "e" * 64),
            set_nested(("case", "id"), "BSC-09"),
            set_nested(("case", "adapter_id"), "bug-squash-bsc-09-rig-v1"),
            set_nested(("case", "adapter_source_sha256"), "e" * 64),
            set_nested(("role_id",), "wifi-enable-production-replay"),
            set_nested(("run_index",), True),
            set_nested(("session_id",), "session-substituted"),
            set_nested(("attempt_id",), "attempt-substituted"),
            set_nested(("nonce",), "nonce-substituted"),
            set_nested(("dut", "alias"), "other-dut"),
            set_nested(("dut", "capabilities"), ["serial"]),
            set_nested(("rig", "alias"), "other-rig"),
            set_nested(("rig", "capabilities"), ["artifact-capture"]),
            set_nested(("firmware", "environment"), "waveshare-349"),
            set_nested(("firmware", "binary_sha256"), "e" * 64),
            set_nested(("raw_artifacts", 0, "role"), "substituted-role"),
            lambda payload: payload.update({"adapter_artifact_sha256": "e" * 64}),
        )
        for mutate in mutations:
            payload = copy.deepcopy(expected)
            mutate(payload)
            payload.pop("request_commitment_sha256", None)
            payload["request_commitment_sha256"] = protocol.canonical_commitment(
                protocol.REQUEST_DOMAIN, payload
            )
            with self.subTest(payload=payload), self.assertRaises(
                protocol.AdapterProtocolError
            ):
                protocol.validate_adapter_request(payload, adapter=adapter, expected=expected)

    def test_duplicate_keys_and_adapter_declared_hashes_are_rejected(self) -> None:
        adapter = implemented_adapter()
        request = valid_request(adapter)
        duplicate = (
            b'{"schema_version":1,"schema_version":1,"protocol_version":1}'
        )
        with self.assertRaises(protocol.AdapterProtocolError):
            protocol.parse_adapter_request(duplicate, adapter=adapter, expected=request)
        response = valid_response(request)
        response["artifact_sha256"] = {"serial-log": "f" * 64}
        with self.assertRaises(protocol.AdapterProtocolError):
            protocol.validate_adapter_response(response, request=request)
        for field, value in (
            ("nonce", "nonce-substituted"),
            ("request_commitment_sha256", "f" * 64),
            ("status", "pass"),
            ("schema_version", True),
            ("protocol_version", True),
        ):
            changed = valid_response(request)
            changed[field] = value
            with self.subTest(field=field), self.assertRaises(protocol.AdapterProtocolError):
                protocol.validate_adapter_response(changed, request=request)

    def test_runner_hashes_exact_raw_set_and_writes_canonical_external_manifest(self) -> None:
        adapter = implemented_adapter()
        role = adapter.roles[0]
        request = valid_request(adapter)
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            raw_directory = root / "raw"
            write_raw_set(raw_directory, role)
            manifest_path = root / "raw-artifact-manifest.json"
            manifest = protocol.collect_raw_artifacts(
                raw_directory=raw_directory,
                role=role,
                request_commitment_sha256=str(request["request_commitment_sha256"]),
                manifest_path=manifest_path,
            )
            self.assertEqual(
                manifest_path.read_bytes(), protocol.canonical_json_bytes(manifest) + b"\n"
            )
            self.assertEqual(
                [item["role"] for item in manifest["artifacts"]],
                [artifact.role for artifact in role.raw_artifacts],
            )
            for item in manifest["artifacts"]:
                path = raw_directory / item["filename"]
                self.assertEqual(item["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
                self.assertEqual(item["size_bytes"], path.stat().st_size)
            committed = dict(manifest)
            commitment = committed.pop("manifest_commitment_sha256")
            self.assertEqual(
                commitment,
                protocol.canonical_commitment(protocol.MANIFEST_DOMAIN, committed),
            )
            parsed = protocol.read_collected_raw_artifacts(
                raw_directory=raw_directory,
                role=role,
                manifest=manifest,
            )
            self.assertEqual(
                parsed,
                {
                    artifact.role: (raw_directory / artifact.filename).read_bytes()
                    for artifact in role.raw_artifacts
                },
            )

    def test_verified_read_rejects_bytes_changed_after_manifest_collection(self) -> None:
        adapter = implemented_adapter()
        role = adapter.roles[0]
        request = valid_request(adapter)
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            raw_directory = root / "raw"
            write_raw_set(raw_directory, role)
            manifest = protocol.collect_raw_artifacts(
                raw_directory=raw_directory,
                role=role,
                request_commitment_sha256=str(request["request_commitment_sha256"]),
                manifest_path=root / "manifest.json",
            )
            first = role.raw_artifacts[0]
            original = raw_directory / first.filename
            original.write_bytes(b"different bytes with a different digest\n")
            with self.assertRaises(protocol.AdapterProtocolError) as raised:
                protocol.read_collected_raw_artifacts(
                    raw_directory=raw_directory,
                    role=role,
                    manifest=manifest,
                )
            self.assertEqual(raised.exception.code, "raw_artifact_changed")

            original.write_bytes(b"raw-1-adapter-transcript\n")
            tampered = copy.deepcopy(manifest)
            tampered["artifacts"][0]["sha256"] = "f" * 64
            with self.assertRaises(protocol.AdapterProtocolError) as raised:
                protocol.read_collected_raw_artifacts(
                    raw_directory=raw_directory,
                    role=role,
                    manifest=tampered,
                )
            self.assertEqual(raised.exception.code, "manifest_invalid")

    def test_raw_set_rejects_missing_extra_symlink_hardlink_nonregular_and_size_drift(self) -> None:
        adapter = implemented_adapter()
        role = adapter.roles[0]
        request = valid_request(adapter)

        def expect_failure(mutator) -> None:
            with tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                raw_directory = root / "raw"
                write_raw_set(raw_directory, role)
                mutator(root, raw_directory)
                with self.assertRaises(protocol.AdapterProtocolError):
                    protocol.collect_raw_artifacts(
                        raw_directory=raw_directory,
                        role=role,
                        request_commitment_sha256=str(request["request_commitment_sha256"]),
                        manifest_path=root / "manifest.json",
                    )

        filename = role.raw_artifacts[0].filename
        expect_failure(lambda _root, directory: (directory / filename).unlink())
        expect_failure(lambda _root, directory: (directory / "extra.log").write_text("extra"))

        def symlink(root: Path, directory: Path) -> None:
            target = root / "outside.log"
            target.write_text("outside")
            (directory / filename).unlink()
            (directory / filename).symlink_to(target)

        expect_failure(symlink)

        def hardlink(root: Path, directory: Path) -> None:
            target = root / "outside.log"
            target.write_text("outside")
            (directory / filename).unlink()
            os.link(target, directory / filename)

        expect_failure(hardlink)

        def fifo(_root: Path, directory: Path) -> None:
            (directory / filename).unlink()
            os.mkfifo(directory / filename)

        expect_failure(fifo)
        expect_failure(lambda _root, directory: (directory / filename).write_bytes(b""))

        largest = min(role.raw_artifacts, key=lambda item: item.maximum_bytes)
        expect_failure(
            lambda _root, directory: (directory / largest.filename).write_bytes(
                b"x" * (largest.maximum_bytes + 1)
            )
        )

    def test_raw_directory_and_manifest_paths_fail_closed(self) -> None:
        adapter = implemented_adapter()
        role = adapter.roles[0]
        request = valid_request(adapter)
        commitment = str(request["request_commitment_sha256"])
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            real = root / "real"
            write_raw_set(real, role)
            linked = root / "raw"
            linked.symlink_to(real, target_is_directory=True)
            with self.assertRaises(protocol.AdapterProtocolError):
                protocol.collect_raw_artifacts(
                    raw_directory=linked,
                    role=role,
                    request_commitment_sha256=commitment,
                    manifest_path=root / "manifest.json",
                )

        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            directory = root / "raw"
            write_raw_set(directory, role)
            with self.assertRaises(protocol.AdapterProtocolError):
                protocol.collect_raw_artifacts(
                    raw_directory=directory,
                    role=role,
                    request_commitment_sha256=commitment,
                    manifest_path=directory / "manifest.json",
                )
            manifest = root / "manifest.json"
            manifest.write_text("occupied")
            with self.assertRaises(protocol.AdapterProtocolError):
                protocol.collect_raw_artifacts(
                    raw_directory=directory,
                    role=role,
                    request_commitment_sha256=commitment,
                    manifest_path=manifest,
                )

    def test_parent_directory_replacement_is_detected(self) -> None:
        adapter = implemented_adapter()
        role = adapter.roles[0]
        request = valid_request(adapter)
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            directory = root / "raw"
            write_raw_set(directory, role)
            moved = root / "moved"
            original_hash = protocol._hash_raw_file
            swapped = False

            def swap_then_hash(*args, **kwargs):
                nonlocal swapped
                if not swapped:
                    swapped = True
                    directory.rename(moved)
                    write_raw_set(directory, role)
                return original_hash(*args, **kwargs)

            with mock.patch.object(protocol, "_hash_raw_file", side_effect=swap_then_hash), self.assertRaises(
                protocol.AdapterProtocolError
            ) as raised:
                protocol.collect_raw_artifacts(
                    raw_directory=directory,
                    role=role,
                    request_commitment_sha256=str(request["request_commitment_sha256"]),
                    manifest_path=root / "manifest.json",
                )
            self.assertEqual(raised.exception.code, "raw_artifact_changed")


if __name__ == "__main__":
    unittest.main()
