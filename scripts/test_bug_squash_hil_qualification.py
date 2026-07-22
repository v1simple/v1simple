#!/usr/bin/env python3
"""Adversarial regressions for check_bug_squash_hil_qualification.py."""

from __future__ import annotations

import copy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Callable
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_bug_squash_hil_qualification as qualification  # type: ignore  # noqa: E402
import resolve_hil_board as resolver  # type: ignore  # noqa: E402

TARGET_SHA = subprocess.check_output(
    ["git", "rev-parse", "HEAD"],
    cwd=ROOT,
    text=True,
).strip()
VALIDATION_NOW = datetime(2026, 7, 21, 23, 59, 59, tzinfo=timezone.utc)
SYNTHETIC_REPOSITORY_STATE = qualification.RepositoryState(
    head_sha=TARGET_SHA,
    target_commit_utc=datetime(2025, 7, 21, 19, 59, 0, tzinfo=timezone.utc),
    firmware_version="1.0.6",
    worktree_clean=True,
)
ORIGINAL_READ_REPOSITORY_STATE = qualification.read_repository_state
SYNTHETIC_TREE_SHA256 = "1" * 64
SYNTHETIC_TOOL_IDENTITY = {
    "schema_version": 1,
    "platformio": {
        "sha256": "2" * 64,
        "package_sha256": "6" * 64,
        "version": "PlatformIO Core, version 6.1.19",
    },
    "python": {"sha256": "3" * 64, "version": "3.13.5"},
    "git": {"sha256": "4" * 64, "version": "git version 2.50.1"},
    "esptool": {"sha256": "5" * 64, "version": "esptool v5.2.0"},
}
SYNTHETIC_TOOL_IDENTITY["identity_sha256"] = qualification.canonical_commitment(
    "v1simple.hil.build-tools.v1",
    SYNTHETIC_TOOL_IDENTITY,
)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def has_error(errors: list[str], text: str) -> bool:
    return any(text in error for error in errors)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def expected_fact_value(contract: dict[str, Any]) -> bool | int:
    if contract["type"] == "boolean":
        return contract["expected"]
    return contract["minimum"]


def make_observation(
    case_id: str,
    role: dict[str, Any],
    run_index: int,
    run_id: str,
    source_artifact_id: str,
) -> dict[str, Any]:
    timestamp = "2025-07-21T20:30:00Z"
    return {
        "schema_version": 1,
        "case_id": case_id,
        "role_id": role["role_id"],
        "run_index": run_index,
        "run_id": run_id,
        "dut_alias": "dut-primary",
        "rig_alias": "rig-primary",
        "build_kind": role["build_kind"],
        "instrumentation_mode": "hybrid",
        "source_artifact_id": source_artifact_id,
        "started_at_utc": "2025-07-21T20:15:00Z",
        "completed_at_utc": "2025-07-21T20:45:00Z",
        "stimuli": [
            {"id": event_id, "observed": True, "at_utc": timestamp}
            for event_id in role["stimulus_ids"]
        ],
        "faults": [
            {
                "id": event_id,
                "armed_at_utc": timestamp,
                "triggered_at_utc": timestamp,
                "cleared_at_utc": timestamp,
            }
            for event_id in role["fault_ids"]
        ],
        "barriers": [
            {
                "id": event_id,
                "ready_at_utc": timestamp,
                "released_at_utc": timestamp,
                "timed_out": False,
            }
            for event_id in role["barrier_ids"]
        ],
        "vbus_isolated": role["vbus_isolation_required"],
        "resets": {
            "expected_kind": role["reset_contract"]["expected_kind"],
            "planned": role["reset_contract"]["expected_count"],
            "observed": role["reset_contract"]["expected_count"],
            "unexpected": 0,
        },
        "facts": {
            fact["id"]: expected_fact_value(fact)
            for fact in role["facts"]
        },
    }


def make_source_evidence(
    observation: dict[str, Any],
    role: dict[str, Any],
    case: dict[str, Any],
    profile: dict[str, Any],
    execution_provenance_sha256: str,
) -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    for event in observation["stimuli"]:
        records.append(
            {
                "kind": "stimulus",
                "id": event["id"],
                "at_utc": event["at_utc"],
                "value": True,
            }
        )
    for event in observation["faults"]:
        for phase in ("armed", "triggered", "cleared"):
            records.append(
                {
                    "kind": f"fault-{phase}",
                    "id": event["id"],
                    "at_utc": event[f"{phase}_at_utc"],
                    "value": True,
                }
            )
    for event in observation["barriers"]:
        records.extend(
            [
                {
                    "kind": "barrier-ready",
                    "id": event["id"],
                    "at_utc": event["ready_at_utc"],
                    "value": True,
                },
                {
                    "kind": "barrier-released",
                    "id": event["id"],
                    "at_utc": event["released_at_utc"],
                    "value": True,
                },
                {
                    "kind": "barrier-timeout",
                    "id": event["id"],
                    "at_utc": event["released_at_utc"],
                    "value": False,
                },
            ]
        )
    completed_at = observation["completed_at_utc"]
    records.append(
        {
            "kind": "isolation",
            "id": "vbus-isolated",
            "at_utc": completed_at,
            "value": observation["vbus_isolated"],
        }
    )
    for key in ("planned", "observed", "unexpected"):
        records.append(
            {
                "kind": "reset",
                "id": key,
                "at_utc": completed_at,
                "value": observation["resets"][key],
            }
        )
    for fact in role["facts"]:
        fact_id = fact["id"]
        records.append(
            {
                "kind": "fact",
                "id": fact_id,
                "at_utc": completed_at,
                "value": observation["facts"][fact_id],
            }
        )
    records.sort(key=lambda record: record["at_utc"])
    payload = {
        "schema_version": 1,
        "case_id": observation["case_id"],
        "role_id": observation["role_id"],
        "run_id": observation["run_id"],
        "case_definition_sha256": qualification.canonical_commitment(
            "v1simple.hil.case-definition.v1",
            case,
        ),
        "driver_contract_sha256": qualification.canonical_commitment(
            "v1simple.hil.case-driver-contract.v1",
            profile["case_driver_contract"],
        ),
        "execution_provenance_sha256": execution_provenance_sha256,
        "records": records,
    }
    return qualification.with_provenance_commitment(
        "v1simple.hil.case-source.v1",
        payload,
    )


def make_valid_artifact(
    tmpdir: Path,
) -> tuple[Path, dict[str, Any], dict[str, Any]]:
    profile, profile_errors = qualification.load_pinned_profile()
    assert_true(profile is not None and not profile_errors, f"profile errors: {profile_errors}")
    assert profile is not None

    pack = tmpdir / "pack"
    pack.mkdir(parents=True)
    artifacts: list[dict[str, Any]] = []

    def add_artifact(
        artifact_id: str,
        scope: str,
        role: str,
        artifact_format: str,
        relative_path: str,
        content: Any,
    ) -> None:
        path = pack / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        if artifact_format == "json":
            write_json(path, content)
        else:
            assert isinstance(content, bytes)
            path.write_bytes(content)
        artifacts.append(
            {
                "id": artifact_id,
                "scope": scope,
                "role": role,
                "format": artifact_format,
                "path": relative_path,
                "sha256": sha256(path),
            }
        )

    build_binary_ids: dict[str, str] = {}
    build_binary_shas: dict[str, str] = {}
    active_builds = [
        build
        for build in profile["build_contracts"]
        if build["implementation_status"] == "active"
    ]
    for build_index, build in enumerate(active_builds, start=1):
        artifact_id = f"firmware-{build['kind']}"
        relative_path = f"binaries/{build['kind']}.bin"
        add_artifact(
            artifact_id,
            "qualification",
            build["binary_role"],
            "binary",
            relative_path,
            f"synthetic-{build_index}-{build['kind']}\n".encode(),
        )
        build_binary_ids[build["kind"]] = artifact_id
        build_binary_shas[build["kind"]] = artifacts[-1]["sha256"]
    build_records: list[dict[str, Any]] = []
    for build in active_builds:
        contract_sha = qualification.canonical_commitment(
            "v1simple.hil.build-contract.v1",
            build,
        )
        started_at = "2025-07-21T20:01:00Z"
        completed_at = "2025-07-21T20:04:00Z"
        log_artifact_id = f"build-log-{build['kind']}"
        log_header = qualification.expected_build_log_header(
            target_git_sha=TARGET_SHA,
            target_tree_sha256=SYNTHETIC_TREE_SHA256,
            build_contract_sha256=contract_sha,
            tool_identity_sha256=SYNTHETIC_TOOL_IDENTITY["identity_sha256"],
            kind=build["kind"],
            environment=build["environment"],
            started_at_utc=started_at,
            completed_at_utc=completed_at,
        )
        add_artifact(
            log_artifact_id,
            "qualification",
            "build-log",
            "text",
            f"logs/{build['kind']}.log",
            ("\n".join(log_header) + "\nsynthetic successful build output\n").encode(),
        )
        log_sha = artifacts[-1]["sha256"]
        record = {
            "kind": build["kind"],
            "firmware_version": "1.0.6",
            "environment": build["environment"],
            "commit_sha": TARGET_SHA,
            "build_command": build["build_command"],
            "build_contract_sha256": contract_sha,
            "binary_artifact_id": build_binary_ids[build["kind"]],
            "binary_sha256": build_binary_shas[build["kind"]],
            "log_artifact_id": log_artifact_id,
            "log_sha256": log_sha,
            "source_worktree_clean": True,
            "started_at_utc": started_at,
            "completed_at_utc": completed_at,
            "input_commitment_sha256": qualification.build_input_commitment(
                target_git_sha=TARGET_SHA,
                target_tree_sha256=SYNTHETIC_TREE_SHA256,
                firmware_version="1.0.6",
                contract=build,
                tools=SYNTHETIC_TOOL_IDENTITY,
            ),
            "output_commitment_sha256": qualification.build_output_commitment(
                binary_sha256=build_binary_shas[build["kind"]],
                log_sha256=log_sha,
                started_at_utc=started_at,
                completed_at_utc=completed_at,
            ),
        }
        build_records.append(
            qualification.with_provenance_commitment(
                "v1simple.hil.build-provenance.v1",
                record,
            )
        )

    build_manifest_payload = {
        "schema_version": 3,
        "target_git_sha": TARGET_SHA,
        "target_tree_sha256": SYNTHETIC_TREE_SHA256,
        "build_contracts_sha256": qualification.build_contracts_sha256(profile),
        "observed_at_utc": "2025-07-21T20:05:00Z",
        "generator": {
            "path": "scripts/generate_bug_squash_build_evidence.py",
            "sha256": qualification.git_blob_sha256(
                TARGET_SHA,
                "scripts/generate_bug_squash_build_evidence.py",
            ),
        },
        "tools": SYNTHETIC_TOOL_IDENTITY,
        "builds": build_records,
    }
    build_manifest = qualification.with_provenance_commitment(
        "v1simple.hil.build-manifest.v1",
        build_manifest_payload,
    )
    add_artifact(
        "build-manifest",
        "qualification",
        "build-manifest",
        "json",
        "manifests/build-manifest.json",
        build_manifest,
    )

    dut_capabilities = sorted(
        {
            capability
            for case in profile["required_cases"]
            for capability in case["required_dut_capabilities"]
        }
    )
    rig_capabilities = sorted(
        {
            capability
            for case in profile["required_cases"]
            for capability in case["required_rig_capabilities"]
        }
    )
    dut_resolution = {
        "schema_version": 1,
        "alias": "dut-primary",
        "capabilities": dut_capabilities,
        "endpoints": {"serial_port": "synthetic-endpoint"},
    }
    dut_binding = {
        "schema_version": 1,
        "commitment_salt_hex": "a1" * 32,
        "inventory_record": {
            "alias": "dut-primary",
            "capabilities": dut_capabilities,
            "connection": {
                "lan_base_url": None,
                "usb_serial": "synthetic-local-identity",
            },
        },
        "resolution": dut_resolution,
    }
    write_json(pack / "local/dut-resolution.json", dut_binding)
    resolver_attestation = qualification.build_board_inventory_attestation(
        dut_binding,
        observed_at_utc="2025-07-21T20:10:00Z",
    )
    add_artifact(
        "resolver-attestation",
        "qualification",
        "resolver-attestation",
        "json",
        "manifests/resolver-attestation.json",
        resolver_attestation,
    )
    rig_resolution = {
        "schema_version": 1,
        "alias": "rig-primary",
        "capabilities": rig_capabilities,
        "endpoints": {},
    }
    rig_binding = {
        "schema_version": 1,
        "commitment_salt_hex": "b2" * 32,
        "inventory_record": {
            "alias": "rig-primary",
            "capabilities": rig_capabilities,
            "connection": {"lan_base_url": None, "usb_serial": None},
        },
        "resolution": rig_resolution,
    }
    write_json(pack / "local/rig-resolution.json", rig_binding)
    rig_attestation = qualification.build_board_inventory_attestation(
        rig_binding,
        observed_at_utc="2025-07-21T20:11:00Z",
    )
    add_artifact(
        "rig-resolver-attestation",
        "qualification",
        "resolver-attestation",
        "json",
        "manifests/rig-resolver-attestation.json",
        rig_attestation,
    )
    execution_provenance = qualification.expected_execution_provenance(
        profile,
        TARGET_SHA,
    )
    add_artifact(
        "execution-provenance",
        "qualification",
        "execution-provenance",
        "json",
        "manifests/execution-provenance.json",
        execution_provenance,
    )

    case_results: list[dict[str, Any]] = []
    for case in profile["required_cases"]:
        runs: list[dict[str, Any]] = []
        roles = qualification.required_case_roles(case)
        for run_index in range(1, case["minimum_runs"] + 1):
            run_id = f"run-{case['id'].lower().replace('-', '')}-{run_index}"
            role_refs: list[dict[str, str]] = []
            for role_id, role in roles.items():
                artifact_id = f"{case['id']}-run{run_index}-{role_id}"
                source_artifact_id = f"{artifact_id}-source"
                relative_path = (
                    f"cases/{case['id']}/run-{run_index}/{role_id}.json"
                )
                observation = make_observation(
                    case["id"],
                    role,
                    run_index,
                    run_id,
                    source_artifact_id,
                )
                add_artifact(
                    source_artifact_id,
                    case["id"],
                    f"{role_id}-source",
                    "json",
                    f"cases/{case['id']}/run-{run_index}/{role_id}-source.json",
                    make_source_evidence(
                        observation,
                        role,
                        case,
                        profile,
                        execution_provenance["provenance_sha256"],
                    ),
                )
                add_artifact(
                    artifact_id,
                    case["id"],
                    role_id,
                    "json",
                    relative_path,
                    observation,
                )
                role_refs.append({"role_id": role_id, "artifact_id": artifact_id})
            runs.append(
                {
                    "run_index": run_index,
                    "run_id": run_id,
                    "dut_alias": "dut-primary",
                    "rig_alias": "rig-primary",
                    "roles": role_refs,
                }
            )
        case_results.append({"id": case["id"], "result": "PASS", "runs": runs})

    artifact_path = pack / "qualification-result.json"
    payload = {
        "schema_version": 1,
        "profile_id": profile["profile_id"],
        "profile_version": profile["profile_version"],
        "profile_sha256": qualification.PINNED_PROFILE_SHA256,
        "result": "PASS",
        "target_git_sha": TARGET_SHA,
        "started_at_utc": "2025-07-21T20:00:00Z",
        "completed_at_utc": "2025-07-21T21:00:00Z",
        "safety": {
            "unexpected_panics": 0,
            "unexpected_watchdog_resets": 0,
        },
        "duts": [
            {
                "alias": "dut-primary",
                "capabilities": dut_capabilities,
                "resolver_attestation_artifact_id": "resolver-attestation",
                "local_resolution_path": "local/dut-resolution.json",
            }
        ],
        "rigs": [
            {
                "alias": "rig-primary",
                "capabilities": rig_capabilities,
                "resolver_attestation_artifact_id": "rig-resolver-attestation",
                "local_resolution_path": "local/rig-resolution.json",
            }
        ],
        "build_manifest_artifact_id": "build-manifest",
        "execution_provenance_artifact_id": "execution-provenance",
        "evidence_artifacts": artifacts,
        "cases": case_results,
    }
    write_json(artifact_path, payload)
    return artifact_path, payload, profile


def validate(payload: Any, artifact_path: Path) -> list[str]:
    def repository_state(candidate: Any, expected: Any) -> tuple[Any, list[str]]:
        if candidate == TARGET_SHA and expected == TARGET_SHA:
            return SYNTHETIC_REPOSITORY_STATE, []
        return ORIGINAL_READ_REPOSITORY_STATE(candidate, expected)

    with mock.patch.object(
        qualification,
        "read_repository_state",
        side_effect=repository_state,
    ), mock.patch.object(
        qualification,
        "git_tree_sha256",
        return_value=SYNTHETIC_TREE_SHA256,
    ), mock.patch.object(
        qualification,
        "current_build_tool_identity",
        return_value=SYNTHETIC_TOOL_IDENTITY,
    ), mock.patch.object(qualification, "validate_firmware_image"):
        return qualification.validate_artifact(
            payload,
            artifact_path,
            TARGET_SHA,
            now=VALIDATION_NOW,
        )


def artifact_entry(payload: dict[str, Any], artifact_id: str) -> dict[str, Any]:
    return next(
        entry
        for entry in payload["evidence_artifacts"]
        if entry["id"] == artifact_id
    )


def case_result(payload: dict[str, Any], case_id: str) -> dict[str, Any]:
    return next(case for case in payload["cases"] if case["id"] == case_id)


def role_artifact_id(
    payload: dict[str, Any],
    case_id: str,
    role_id: str,
    run_index: int = 1,
) -> str:
    run = case_result(payload, case_id)["runs"][run_index - 1]
    return next(
        role["artifact_id"]
        for role in run["roles"]
        if role["role_id"] == role_id
    )


def source_artifact_id(
    artifact_path: Path,
    payload: dict[str, Any],
    case_id: str,
    role_id: str,
) -> str:
    observation_id = role_artifact_id(payload, case_id, role_id)
    entry = artifact_entry(payload, observation_id)
    observation = json.loads(
        (artifact_path.parent / entry["path"]).read_text(encoding="utf-8")
    )
    return observation["source_artifact_id"]


def rewrite_json_artifact(
    artifact_path: Path,
    payload: dict[str, Any],
    artifact_id: str,
    mutate: Callable[[dict[str, Any]], None],
) -> dict[str, Any]:
    entry = artifact_entry(payload, artifact_id)
    path = artifact_path.parent / entry["path"]
    content = json.loads(path.read_text(encoding="utf-8"))
    mutate(content)
    write_json(path, content)
    entry["sha256"] = sha256(path)
    return content


def recompute_self_declared_build_commitments(
    manifest: dict[str, Any],
    profile: dict[str, Any],
) -> None:
    """Model an evidence author who recomputes every unkeyed build digest."""
    contracts = {contract["kind"]: contract for contract in profile["build_contracts"]}
    for build in manifest["builds"]:
        contract = contracts[build["kind"]]
        build["input_commitment_sha256"] = qualification.build_input_commitment(
            target_git_sha=manifest["target_git_sha"],
            target_tree_sha256=manifest["target_tree_sha256"],
            firmware_version=build["firmware_version"],
            contract=contract,
            tools=manifest["tools"],
        )
        build["output_commitment_sha256"] = qualification.build_output_commitment(
            binary_sha256=build["binary_sha256"],
            log_sha256=build["log_sha256"],
            started_at_utc=build["started_at_utc"],
            completed_at_utc=build["completed_at_utc"],
        )
        build_without_provenance = {
            key: value for key, value in build.items() if key != "provenance_sha256"
        }
        build["provenance_sha256"] = qualification.canonical_commitment(
            "v1simple.hil.build-provenance.v1",
            build_without_provenance,
        )
    manifest_without_provenance = {
        key: value for key, value in manifest.items() if key != "provenance_sha256"
    }
    manifest["provenance_sha256"] = qualification.canonical_commitment(
        "v1simple.hil.build-manifest.v1",
        manifest_without_provenance,
    )


def test_pinned_profile_has_exact_case_specific_contracts(tmpdir: Path) -> None:
    del tmpdir
    profile, errors = qualification.load_pinned_profile()
    assert_true(profile is not None and not errors, f"profile errors: {errors}")
    assert profile is not None
    ids = tuple(case["id"] for case in profile["required_cases"])
    assert_true(ids == qualification.EXPECTED_CASE_IDS, "exact case inventory")
    assert_true(profile["profile_version"] == 4, "profile must be version 4")
    assert_true(
        {build["kind"] for build in profile["build_contracts"]}
        == {"production", "hil-fault", "car-production"},
        "all build identities must be pinned",
    )
    assert_true(profile["qualification_status"] == "blocked", "status is explicit")
    builds = {build["kind"]: build for build in profile["build_contracts"]}
    assert_true(
        builds["hil-fault"]["implementation_status"] == "blocked"
        and builds["hil-fault"]["environment"] is None
        and builds["hil-fault"]["build_command"] == [],
        "unimplemented HIL control must not claim a build environment",
    )
    assert_true(
        {kind for kind, build in builds.items() if build["implementation_status"] == "active"}
        == {"production", "car-production"},
        "only real PlatformIO environments are active",
    )
    assert_true(
        profile["activation_contract"]["minimum_ready_profile_version"] == 3,
        "version 3 is the minimum provenance-aware profile",
    )
    assert_true(
        profile["activation_contract"]["required_validator_provenance_version"] == 1
        and qualification.INTEGRITY_PROVENANCE_VERIFIER_VERSION == 1
        and qualification.AUTHENTICATED_PROVENANCE_VERIFIER_VERSION is None,
        "integrity schema exists but no authenticated verifier is claimed",
    )
    assert_true(
        {
            requirement["id"]
            for requirement in profile["activation_contract"]["requirements"]
            if requirement["status"] == "blocked"
        }
        == {
            "bounded-hil-fault-control",
            "authenticated-build-generator-provenance",
            "authenticated-board-inventory-resolver-provenance",
        },
        "the three remaining unauthenticated activation roots stay blocked",
    )
    assert_true(
        profile["build_provenance_contract"]["status"] == "integrity-bound"
        and profile["board_provenance_contract"]["status"] == "integrity-bound",
        "build and board commitments are not mislabeled authenticated or active",
    )
    for case in profile["required_cases"]:
        assert_true(case["scenario"]["stimulus_ids"], f"{case['id']} stimuli")
        assert_true(case["scenario"]["facts"], f"{case['id']} facts")
        assert_true("barrier_ids" in case["scenario"], f"{case['id']} barriers")
        assert_true(
            "vbus_isolation_required" in case["scenario"],
            f"{case['id']} VBUS contract",
        )
        assert_true("reset_contract" in case["scenario"], f"{case['id']} reset")
        fact_ids = [fact["id"] for fact in case["scenario"]["facts"]]
        assert_true(len(fact_ids) == len(set(fact_ids)), f"{case['id']} fact IDs")
        replay = case["production_replay"]
        if case["fault_build_required"]:
            assert_true(case["scenario"]["build_kind"] == "hil-fault", case["id"])
            assert_true(replay is not None, f"{case['id']} production replay")
            assert_true(replay["build_kind"] == "production", case["id"])
            assert_true(not replay["fault_ids"], f"{case['id']} replay faults absent")
        assert_true(
            bool(replay) == case["production_replay_required"],
            f"{case['id']} replay declaration",
        )


def test_incomplete_profile_rejects_synthetic_pass_pack(tmpdir: Path) -> None:
    artifact_path, payload, _ = make_valid_artifact(tmpdir)
    errors = validate(payload, artifact_path)
    assert_true(
        has_error(errors, "profile is blocked and cannot accept PASS evidence"),
        "blocked infrastructure cannot be papered over with synthetic evidence",
    )
    assert_true(
        has_error(errors, "requires a blocked or unimplemented build contract"),
        "fault roles cannot execute without an active control build",
    )


def test_self_authored_build_and_board_integrity_cannot_activate(tmpdir: Path) -> None:
    profile, profile_errors = qualification.load_pinned_profile()
    assert_true(profile is not None and profile_errors == [], "profile loads")
    assert profile is not None

    fake_bin = tmpdir / "fake-bin"
    fake_bin.mkdir()
    fake_pio = fake_bin / "pio"
    fake_pio.write_text(
        "#!/bin/sh\necho 'PlatformIO Core, version 6.1.19'\n",
        encoding="utf-8",
    )
    fake_pio.chmod(0o755)
    with mock.patch.dict(
        os.environ,
        {"PATH": f"{fake_bin}{os.pathsep}{os.environ.get('PATH', '')}"},
        clear=False,
    ):
        qualification.current_build_tool_identity.cache_clear()
        fake_tools = qualification.current_build_tool_identity()
    qualification.current_build_tool_identity.cache_clear()
    assert_true(
        fake_tools["platformio"]["sha256"] == qualification.file_sha256(fake_pio),
        "PATH-selected fake PlatformIO can author internally consistent integrity metadata",
    )

    stale_binary = tmpdir / "stale-firmware.bin"
    stale_log = tmpdir / "forged-build.log"
    stale_binary.write_bytes(b"stale-valid-image-placeholder")
    stale_log.write_text("author supplied log\n", encoding="utf-8")
    production = next(
        contract
        for contract in profile["build_contracts"]
        if contract["kind"] == "production"
    )
    forged_input = qualification.build_input_commitment(
        target_git_sha=TARGET_SHA,
        target_tree_sha256=SYNTHETIC_TREE_SHA256,
        firmware_version="1.0.6",
        contract=production,
        tools=fake_tools,
    )
    forged_output = qualification.build_output_commitment(
        binary_sha256=qualification.file_sha256(stale_binary),
        log_sha256=qualification.file_sha256(stale_log),
        started_at_utc="2025-07-21T20:01:00Z",
        completed_at_utc="2025-07-21T20:02:00Z",
    )
    assert_true(bool(forged_input and forged_output), "forged integrity digests are computable")

    authored_binding = {
        "schema_version": 1,
        "commitment_salt_hex": "d4" * 32,
        "inventory_record": {
            "alias": "dut-author-supplied",
            "capabilities": ["serial"],
            "connection": {
                "lan_base_url": None,
                "usb_serial": "author-controlled-identity",
            },
        },
        "resolution": {
            "schema_version": 1,
            "alias": "dut-author-supplied",
            "capabilities": ["serial"],
            "endpoints": {"serial_port": "author-controlled-endpoint"},
        },
    }
    authored_attestation = qualification.build_board_inventory_attestation(
        authored_binding,
        observed_at_utc="2025-07-21T20:03:00Z",
    )
    assert_true(
        bool(authored_attestation["inventory_commitment_sha256"]),
        "an evidence author can self-consistently commit an untrusted board binding",
    )

    activation_errors = qualification.profile_activation_errors(profile)
    blocker_codes = {blocker["code"] for blocker in profile["blockers"]}
    assert_true(
        has_error(activation_errors, "authenticated provenance verifier is not implemented")
        and has_error(
            activation_errors,
            "authenticated-build-generator-provenance",
        )
        and has_error(
            activation_errors,
            "authenticated-board-inventory-resolver-provenance",
        )
        and "build-generator-provenance-not-authenticated" in blocker_codes
        and "board-resolution-provenance-not-authenticated" in blocker_codes,
        "self-consistent build and board commitments cannot activate qualification",
    )


def test_profile_ready_mutation_cannot_authenticate_forged_provenance(tmpdir: Path) -> None:
    artifact_path, payload, profile = make_valid_artifact(tmpdir)
    simulated_ready = copy.deepcopy(profile)
    simulated_ready["qualification_status"] = "ready"
    simulated_ready["blockers"] = []
    simulated_ready["activation_contract"]["status"] = "active"
    for requirement in simulated_ready["activation_contract"]["requirements"]:
        requirement["status"] = "active"
    hil_build = next(
        build
        for build in simulated_ready["build_contracts"]
        if build["kind"] == "hil-fault"
    )
    hil_build.update(
        {
            "implementation_status": "active",
            "blocker_code": None,
            "environment": "synthetic-hil",
            "build_command": ["pio", "run", "-e", "synthetic-hil"],
        }
    )
    simulated_ready["case_driver_contract"].update(
        {"status": "active", "driver_source_paths": ["scripts/run_bug_squash_hil.py"]}
    )
    simulated_ready["fault_control_contract"].update(
        {
            "status": "active",
            "implementation_source_paths": ["scripts/run_bug_squash_hil.py"],
            "test_source_paths": ["scripts/test_bug_squash_hil_runner.py"],
        }
    )
    with mock.patch.object(
        qualification,
        "load_pinned_profile",
        return_value=(simulated_ready, []),
    ):
        errors = validate(payload, artifact_path)
    assert_true(
        has_error(errors, "execution_provenance must exactly match")
        and has_error(errors, "builds is missing required kind: hil-fault")
        and has_error(errors, "driver_contract_sha256 must bind"),
        "profile mutation cannot authenticate old build, driver, or execution evidence",
    )


def test_profile_scope_digest_and_case_set_cannot_be_overridden(tmpdir: Path) -> None:
    artifact_path, payload, _ = make_valid_artifact(tmpdir)
    candidate = copy.deepcopy(payload)
    candidate["profile_sha256"] = "f" * 64
    assert_true(has_error(validate(candidate, artifact_path), "pinned tracked profile"), "digest")

    content = qualification.DEFAULT_PROFILE.read_bytes().replace(
        b'"BSC-16"',
        b'"BSC-15"',
        1,
    )
    profile, errors = qualification.validate_profile_bytes(content)
    assert_true(profile is None and has_error(errors, "digest mismatch"), "custom profile")

    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "check_bug_squash_hil_qualification.py"),
            "--artifact",
            str(artifact_path),
            "--expected-git-sha",
            TARGET_SHA,
            "--profile",
            str(qualification.DEFAULT_PROFILE),
        ],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    assert_true(
        result.returncode != 0 and "unrecognized arguments" in result.stdout,
        "no custom profile",
    )


def test_target_sha_must_be_nonzero_existing_head(tmpdir: Path) -> None:
    artifact_path, payload, _ = make_valid_artifact(tmpdir)
    for value, expected in (
        ("0" * 40, "all-zero"),
        ("f" * 40, "existing repository commit"),
        ("short", "full 40-character"),
    ):
        candidate = copy.deepcopy(payload)
        candidate["target_git_sha"] = value
        errors = validate(candidate, artifact_path)
        assert_true(has_error(errors, expected), f"reject target {value}")

    errors = qualification.validate_artifact(
        payload,
        artifact_path,
        "0" * 40,
        now=VALIDATION_NOW,
    )
    assert_true(has_error(errors, "expected_git_sha must be a nonzero"), "zero expected")


def test_repository_state_comes_from_git_tree_and_live_cleanliness(tmpdir: Path) -> None:
    del tmpdir

    def git_result(stdout: str, returncode: int = 0) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(args=[], returncode=returncode, stdout=stdout)

    successful = (
        git_result(f"{TARGET_SHA}\n2025-07-21T15:59:00-04:00\n"),
        git_result(f"{TARGET_SHA}\n"),
        git_result('#define FIRMWARE_VERSION "1.0.6"\n'),
        git_result(""),
    )
    with mock.patch.object(qualification.subprocess, "run", side_effect=successful):
        state, errors = qualification.read_repository_state(TARGET_SHA, TARGET_SHA)
    assert_true(errors == [] and state is not None, "Git state resolves")
    assert state is not None
    assert_true(state.firmware_version == "1.0.6", "version comes from target tree")
    assert_true(
        state.target_commit_utc == datetime(2025, 7, 21, 19, 59, tzinfo=timezone.utc),
        "commit time normalizes to UTC",
    )

    dirty = (*successful[:3], git_result(" M include/config.h\n"))
    with mock.patch.object(qualification.subprocess, "run", side_effect=dirty):
        state, errors = qualification.read_repository_state(TARGET_SHA, TARGET_SHA)
    assert_true(state is None and has_error(errors, "worktree must be clean"), "live dirty")

    missing_version = (
        successful[0],
        successful[1],
        git_result("#define SOMETHING_ELSE 1\n"),
        successful[3],
    )
    with mock.patch.object(
        qualification.subprocess,
        "run",
        side_effect=missing_version,
    ):
        state, errors = qualification.read_repository_state(TARGET_SHA, TARGET_SHA)
    assert_true(
        state is None and has_error(errors, "no valid FIRMWARE_VERSION"),
        "version cannot be supplied by the evidence author",
    )


def test_future_or_reversed_time_is_rejected_everywhere(tmpdir: Path) -> None:
    artifact_path, payload, _ = make_valid_artifact(tmpdir)
    candidate = copy.deepcopy(payload)
    candidate["completed_at_utc"] = "2027-01-01T00:00:00Z"
    assert_true(
        has_error(validate(candidate, artifact_path), "must not be in the future"),
        "future",
    )

    candidate = copy.deepcopy(payload)
    candidate["completed_at_utc"] = "2025-07-21T19:59:59Z"
    assert_true(has_error(validate(candidate, artifact_path), "must not precede"), "order")

    candidate = copy.deepcopy(payload)
    rewrite_json_artifact(
        artifact_path,
        candidate,
        "build-manifest",
        lambda manifest: manifest.update({"observed_at_utc": "2027-01-01T00:00:00Z"}),
    )
    assert_true(
        has_error(validate(candidate, artifact_path), "must not be in the future"),
        "build",
    )


def test_build_manifest_binds_command_commit_clean_tree_and_binary(tmpdir: Path) -> None:
    artifact_path, payload, _ = make_valid_artifact(tmpdir)
    mutations = (
        (
            lambda manifest: manifest["builds"][0].update({"environment": "native"}),
            "environment must match",
        ),
        (
            lambda manifest: manifest["builds"][0].update({"build_command": ["pio", "run"]}),
            "build_command must exactly match",
        ),
        (
            lambda manifest: manifest["builds"][0].update({"commit_sha": "f" * 40}),
            "commit_sha must match",
        ),
        (
            lambda manifest: manifest["builds"][0].update({"source_worktree_clean": False}),
            "source_worktree_clean must be boolean true",
        ),
        (
            lambda manifest: manifest["generator"].update({"sha256": "f" * 64}),
            "must match target-tracked bytes",
        ),
        (
            lambda manifest: manifest["builds"][0].update(
                {"firmware_version": "9.9.9"}
            ),
            "must match target Git tree FIRMWARE_VERSION",
        ),
        (
            lambda manifest: manifest["builds"][0].update(
                {"completed_at_utc": "2025-07-21T20:06:00Z"}
            ),
            "observed_at_utc must not precede a completed build",
        ),
        (
            lambda manifest: manifest["builds"][0].update(
                {"log_artifact_id": "firmware-production"}
            ),
            "qualification/build-log/text",
        ),
        (
            lambda manifest: manifest["builds"].pop(),
            "missing required kind",
        ),
    )
    for index, (mutation, expected) in enumerate(mutations):
        case_dir = tmpdir / f"build-{index}"
        case_dir.mkdir()
        path, candidate, _ = make_valid_artifact(case_dir)
        rewrite_json_artifact(path, candidate, "build-manifest", mutation)
        assert_true(has_error(validate(candidate, path), expected), expected)

    candidate = copy.deepcopy(payload)
    rewrite_json_artifact(
        artifact_path,
        candidate,
        "build-manifest",
        lambda manifest: manifest["builds"][1].update(
            {"binary_sha256": manifest["builds"][0]["binary_sha256"]}
        ),
    )
    assert_true(has_error(validate(candidate, artifact_path), "distinct firmware"), "distinct")


def test_recomputed_self_declared_build_digests_cannot_forge_provenance(
    tmpdir: Path,
) -> None:
    mutations = (
        (
            lambda manifest: manifest.update({"target_git_sha": "f" * 40}),
            "target_git_sha must match the qualification target",
        ),
        (
            lambda manifest: manifest.update({"target_tree_sha256": "f" * 64}),
            "must match the exact target Git tree",
        ),
        (
            lambda manifest: manifest.update({"build_contracts_sha256": "f" * 64}),
            "must match the pinned contracts",
        ),
        (
            lambda manifest: manifest["generator"].update({"sha256": "f" * 64}),
            "must match target-tracked bytes",
        ),
        (
            lambda manifest: manifest["tools"]["platformio"].update(
                {"sha256": "f" * 64}
            ),
            "must match the independently hashed tool identity",
        ),
    )
    for index, (mutation, expected) in enumerate(mutations):
        case_dir = tmpdir / f"self-declared-{index}"
        case_dir.mkdir()
        path, candidate, profile = make_valid_artifact(case_dir)

        def forge(manifest: dict[str, Any]) -> None:
            mutation(manifest)
            if index == 4:
                tools_without_identity = {
                    key: value
                    for key, value in manifest["tools"].items()
                    if key != "identity_sha256"
                }
                manifest["tools"]["identity_sha256"] = qualification.canonical_commitment(
                    "v1simple.hil.build-tools.v1",
                    tools_without_identity,
                )
            recompute_self_declared_build_commitments(manifest, profile)

        rewrite_json_artifact(path, candidate, "build-manifest", forge)
        assert_true(
            has_error(validate(candidate, path), expected),
            f"independent input survives recomputed digest attack: {expected}",
        )


def test_each_build_commitment_layer_fails_closed(tmpdir: Path) -> None:
    mutations = (
        (
            lambda manifest: manifest["builds"][0].update(
                {"build_contract_sha256": "f" * 64}
            ),
            "build_contract_sha256 must match the pinned contract",
        ),
        (
            lambda manifest: manifest["builds"][0].update(
                {"input_commitment_sha256": "f" * 64}
            ),
            "input_commitment_sha256 is not input-bound",
        ),
        (
            lambda manifest: manifest["builds"][0].update(
                {"output_commitment_sha256": "f" * 64}
            ),
            "output_commitment_sha256 is not output-bound",
        ),
        (
            lambda manifest: manifest["builds"][0].update(
                {"provenance_sha256": "f" * 64}
            ),
            "provenance_sha256 does not bind the build record",
        ),
        (
            lambda manifest: manifest.update({"provenance_sha256": "f" * 64}),
            "provenance_sha256 does not bind the complete manifest",
        ),
    )
    for index, (mutation, expected) in enumerate(mutations):
        case_dir = tmpdir / f"commitment-layer-{index}"
        case_dir.mkdir()
        path, candidate, _ = make_valid_artifact(case_dir)
        rewrite_json_artifact(path, candidate, "build-manifest", mutation)
        assert_true(has_error(validate(candidate, path), expected), expected)


def test_firmware_image_validation_requires_complete_esp32s3_image(tmpdir: Path) -> None:
    image = tmpdir / "firmware.bin"
    image.write_bytes(b"not-an-image")
    failed = subprocess.CompletedProcess(
        args=[],
        returncode=2,
        stdout="fatal image error\n",
    )
    with mock.patch.object(qualification.subprocess, "run", return_value=failed):
        errors: list[str] = []
        qualification.validate_firmware_image(image, "build", errors)
    assert_true(has_error(errors, "esptool-valid ESP32-S3"), "reject invalid image")

    valid_output = "\n".join(
        (
            "Detected image type: ESP32-S3",
            "Chip ID: 9 (ESP32-S3)",
            "Checksum: 0xff (valid)",
            "Validation hash: abcdef (valid)",
            "Application Information",
        )
    )
    succeeded = subprocess.CompletedProcess(args=[], returncode=0, stdout=valid_output)
    with mock.patch.object(qualification.subprocess, "run", return_value=succeeded):
        errors = []
        image.write_bytes(b"prefix-v1.0.6-middle-Firmware: 1.0.6-suffix")
        qualification.validate_firmware_image(image, "build", errors, "1.0.6")
    assert_true(errors == [], "accept complete ESP32-S3 image report")

    with mock.patch.object(qualification.subprocess, "run", return_value=succeeded):
        errors = []
        qualification.validate_firmware_image(image, "build", errors, "9.9.9")
    assert_true(has_error(errors, "firmware version identity"), "version is binary-bound")


def test_resolver_attestation_is_sanitized_hashed_and_alias_bound(tmpdir: Path) -> None:
    mutations = (
        (
            lambda attestation: attestation.update({"alias": "dut-other"}),
            "must match the approved board alias",
        ),
        (
            lambda attestation: attestation["capabilities"].pop(),
            "must exactly match approved board capabilities",
        ),
        (
            lambda attestation: attestation.update({"resolution_sha256": "0" * 64}),
            "must not be the all-zero",
        ),
        (
            lambda attestation: attestation.update({"endpoints": {"serial_port": "redacted"}}),
            "endpoints is not allowed",
        ),
    )
    for index, (mutation, expected) in enumerate(mutations):
        case_dir = tmpdir / f"resolver-{index}"
        case_dir.mkdir()
        path, candidate, _ = make_valid_artifact(case_dir)
        rewrite_json_artifact(path, candidate, "resolver-attestation", mutation)
        assert_true(has_error(validate(candidate, path), expected), expected)

    path, candidate, _ = make_valid_artifact(tmpdir / "alias")
    candidate["duts"][0]["alias"] = "192.0.2.1"
    assert_true(has_error(validate(candidate, path), "sanitized dut-* alias"), "IP alias")
    candidate = copy.deepcopy(candidate)
    candidate["duts"][0]["alias"] = "dut-aabbccddeeff"
    assert_true(has_error(validate(candidate, path), "sanitized dut-* alias"), "serial alias")

    path, candidate, _ = make_valid_artifact(tmpdir / "raw-tamper")
    raw_path = path.parent / candidate["duts"][0]["local_resolution_path"]
    raw = json.loads(raw_path.read_text(encoding="utf-8"))
    raw["resolution"]["endpoints"]["serial_port"] = "different-synthetic-endpoint"
    write_json(raw_path, raw)
    assert_true(
        has_error(validate(candidate, path), "recomputed local resolution attestation"),
        "raw resolver resolution must remain digest-bound",
    )

    path, candidate, _ = make_valid_artifact(tmpdir / "rig-tamper")
    rewrite_json_artifact(
        path,
        candidate,
        "rig-resolver-attestation",
        lambda attestation: attestation.update({"alias": "rig-other"}),
    )
    assert_true(has_error(validate(candidate, path), "approved board alias"), "rig alias bound")


def test_board_inventory_commitment_is_private_and_exact(tmpdir: Path) -> None:
    path, candidate, _ = make_valid_artifact(tmpdir / "privacy")
    attestation_entry = artifact_entry(candidate, "resolver-attestation")
    attestation_text = (path.parent / attestation_entry["path"]).read_text(encoding="utf-8")
    for forbidden in (
        "synthetic-local-identity",
        "synthetic-endpoint",
        "commitment_salt_hex",
        "connection",
        "endpoints",
        "usb_serial",
        "lan_base_url",
    ):
        assert_true(forbidden not in attestation_text, f"sanitized attestation hides {forbidden}")
    attestation = json.loads(attestation_text)
    assert_true(
        attestation["inventory_commitment_sha256"]
        and attestation["commitment_algorithm"]
        == qualification.BOARD_COMMITMENT_ALGORITHM,
        "sanitized attestation retains only the inventory commitment",
    )

    for index, mutation in enumerate(
        (
            lambda binding: binding["inventory_record"]["connection"].update(
                {"usb_serial": "different-private-identity"}
            ),
            lambda binding: binding.update({"commitment_salt_hex": "c3" * 32}),
            lambda binding: binding["inventory_record"]["capabilities"].append(
                "unselected-private-capability"
            ),
        )
    ):
        case_dir = tmpdir / f"binding-{index}"
        case_dir.mkdir()
        local_path, local_candidate, _ = make_valid_artifact(case_dir)
        binding_path = (
            local_path.parent / local_candidate["duts"][0]["local_resolution_path"]
        )
        binding = json.loads(binding_path.read_text(encoding="utf-8"))
        mutation(binding)
        if index == 2:
            binding["inventory_record"]["capabilities"].sort()
        write_json(binding_path, binding)
        assert_true(
            has_error(
                validate(local_candidate, local_path),
                "recomputed local resolution attestation",
            ),
            "inventory bytes and salt are commitment-bound",
        )


def test_execution_and_case_source_bind_target_tracked_contracts(tmpdir: Path) -> None:
    for index, field in enumerate(
        (
            "target_git_sha",
            "case_definitions_sha256",
            "case_driver_contract_sha256",
            "fault_control_contract_sha256",
        )
    ):
        case_dir = tmpdir / f"execution-{index}"
        case_dir.mkdir()
        path, candidate, _ = make_valid_artifact(case_dir)

        def forge_execution(provenance: dict[str, Any]) -> None:
            provenance[field] = "f" * (40 if field == "target_git_sha" else 64)
            unsigned = {
                key: value
                for key, value in provenance.items()
                if key != "provenance_sha256"
            }
            provenance["provenance_sha256"] = qualification.canonical_commitment(
                "v1simple.hil.execution-provenance.v1",
                unsigned,
            )

        rewrite_json_artifact(path, candidate, "execution-provenance", forge_execution)
        assert_true(
            has_error(validate(candidate, path), "must exactly match target-tracked"),
            f"execution field is independently authenticated: {field}",
        )

    source_mutations = (
        ("case_definition_sha256", "case_definition_sha256 must bind the pinned case"),
        ("driver_contract_sha256", "driver_contract_sha256 must bind tracked"),
        (
            "execution_provenance_sha256",
            "must bind authenticated target-tracked execution provenance",
        ),
    )
    for index, (field, expected) in enumerate(source_mutations):
        case_dir = tmpdir / f"source-contract-{index}"
        case_dir.mkdir()
        path, candidate, _ = make_valid_artifact(case_dir)
        artifact_id = source_artifact_id(
            path,
            candidate,
            "BSC-02",
            "maintenance-recovery-fault",
        )

        def forge_source(source: dict[str, Any]) -> None:
            source[field] = "f" * 64
            unsigned = {
                key: value
                for key, value in source.items()
                if key != "source_commitment_sha256"
            }
            source["source_commitment_sha256"] = qualification.canonical_commitment(
                "v1simple.hil.case-source.v1",
                unsigned,
            )

        rewrite_json_artifact(path, candidate, artifact_id, forge_source)
        assert_true(has_error(validate(candidate, path), expected), expected)


def test_case_runs_roles_and_artifacts_are_exact_and_nonreusable(tmpdir: Path) -> None:
    artifact_path, payload, _ = make_valid_artifact(tmpdir)
    candidate = copy.deepcopy(payload)
    removed = candidate["cases"].pop()
    assert_true(
        has_error(
            validate(candidate, artifact_path),
            f"missing required case id: {removed['id']}",
        ),
        "missing case",
    )

    candidate = copy.deepcopy(payload)
    candidate["cases"].append(copy.deepcopy(candidate["cases"][0]))
    assert_true(has_error(validate(candidate, artifact_path), "duplicate case id"), "duplicate")

    candidate = copy.deepcopy(payload)
    run = case_result(candidate, "BSC-02")["runs"][0]
    removed_role = run["roles"].pop()
    assert_true(has_error(validate(candidate, artifact_path), "missing required role_id"), "role")

    candidate = copy.deepcopy(payload)
    bsc02 = case_result(candidate, "BSC-02")["runs"][0]
    bsc03 = case_result(candidate, "BSC-03")["runs"][0]
    bsc03["roles"][0]["artifact_id"] = bsc02["roles"][0]["artifact_id"]
    errors = validate(candidate, artifact_path)
    assert_true(has_error(errors, "must reference BSC-03"), "cross-case role")
    assert_true(has_error(errors, "referenced exactly once"), "reuse")

    candidate = copy.deepcopy(payload)
    case_result(candidate, "BSC-02")["result"] = "ACCEPTED_RISK"
    assert_true(has_error(validate(candidate, artifact_path), "accepted-risk"), "accepted risk")


def test_case_specific_facts_stimuli_faults_and_barriers_are_parsed(tmpdir: Path) -> None:
    role_id = "maintenance-recovery-fault"
    mutations = (
        (
            lambda observation: observation["facts"].update(
                {"failed-ap-never-published-active": False}
            ),
            "must be boolean true",
        ),
        (
            lambda observation: observation["facts"].update(
                {"first-retry-delay-ms": 1}
            ),
            "must be between",
        ),
        (
            lambda observation: observation["stimuli"].pop(),
            "stimuli is missing required id",
        ),
        (
            lambda observation: observation["faults"].clear(),
            "faults is missing required id",
        ),
        (
            lambda observation: observation["barriers"][0].update({"timed_out": True}),
            "timed_out must be boolean false",
        ),
    )
    for index, (mutation, expected) in enumerate(mutations):
        case_dir = tmpdir / f"typed-{index}"
        case_dir.mkdir()
        path, candidate, _ = make_valid_artifact(case_dir)
        artifact_id = role_artifact_id(candidate, "BSC-02", role_id)
        rewrite_json_artifact(path, candidate, artifact_id, mutation)
        assert_true(has_error(validate(candidate, path), expected), expected)


def test_case_source_is_independent_exact_and_causally_ordered(tmpdir: Path) -> None:
    case_id = "BSC-02"
    role_id = "maintenance-recovery-fault"
    mutations = (
        (
            lambda source: source["records"].pop(),
            "missing source record",
        ),
        (
            lambda source: source["records"].reverse(),
            "causally ordered by timestamp",
        ),
        (
            lambda source: source["records"][0].update({"value": False}),
            "must match the observed source value",
        ),
    )
    for index, (mutation, expected) in enumerate(mutations):
        case_dir = tmpdir / f"source-{index}"
        case_dir.mkdir()
        path, candidate, _ = make_valid_artifact(case_dir)
        artifact_id = source_artifact_id(path, candidate, case_id, role_id)
        rewrite_json_artifact(path, candidate, artifact_id, mutation)
        assert_true(has_error(validate(candidate, path), expected), expected)


def test_qualification_cannot_predate_target_commit(tmpdir: Path) -> None:
    artifact_path, payload, _ = make_valid_artifact(tmpdir)
    candidate = copy.deepcopy(payload)
    candidate["started_at_utc"] = "2025-07-21T19:58:59Z"
    assert_true(
        has_error(validate(candidate, artifact_path), "must not precede"),
        "qualification starts after target commit",
    )


def test_reset_vbus_instrumentation_and_safety_cannot_be_weakened(tmpdir: Path) -> None:
    mutations = (
        (
            "BSC-03",
            "connected-persistence-hard-cut",
            lambda observation: observation.update({"vbus_isolated": False}),
            "vbus_isolated must be true",
        ),
        (
            "BSC-03",
            "connected-persistence-hard-cut",
            lambda observation: observation["resets"].update({"observed": 0}),
            "resets.observed must be integer 1",
        ),
        (
            "BSC-02",
            "maintenance-recovery-fault",
            lambda observation: observation["resets"].update({"unexpected": 1}),
            "resets.unexpected must be integer 0",
        ),
        (
            "BSC-02",
            "maintenance-recovery-fault",
            lambda observation: observation.update({"instrumentation_mode": "unobserved"}),
            "instrumentation_mode is not allowed",
        ),
    )
    for index, (case_id, role_id, mutation, expected) in enumerate(mutations):
        case_dir = tmpdir / f"safety-{index}"
        case_dir.mkdir()
        path, candidate, _ = make_valid_artifact(case_dir)
        artifact_id = role_artifact_id(candidate, case_id, role_id)
        rewrite_json_artifact(path, candidate, artifact_id, mutation)
        assert_true(has_error(validate(candidate, path), expected), expected)

    path, candidate, _ = make_valid_artifact(tmpdir / "global")
    candidate["safety"]["unexpected_watchdog_resets"] = False
    assert_true(has_error(validate(candidate, path), "must be integer 0"), "bool WDT")


def test_paths_lstat_every_component_and_reject_operational_shapes(tmpdir: Path) -> None:
    invalid_paths = (
        ("/tmp/absolute.json", "canonical relative POSIX"),
        ("../outside.json", "unsafe or operationally identifying"),
        ("evidence/192.0.2.1.json", "operationally identifying"),
        ("evidence/aabbccddeeff.json", "operationally identifying"),
        ("evidence/serial-capture.json", "operationally identifying"),
        ("evidence/bad segment.json", "operationally identifying"),
        ("evidence/with\x00nul.json", "nonempty canonical"),
    )
    for index, (raw_path, expected) in enumerate(invalid_paths):
        case_dir = tmpdir / f"path-{index}"
        case_dir.mkdir()
        path, candidate, _ = make_valid_artifact(case_dir)
        artifact_entry(candidate, "build-manifest")["path"] = raw_path
        assert_true(has_error(validate(candidate, path), expected), raw_path)

    path, candidate, _ = make_valid_artifact(tmpdir / "symlink")
    target = path.parent / "manifests"
    link = path.parent / "linked"
    link.symlink_to(target, target_is_directory=True)
    artifact_entry(candidate, "build-manifest")["path"] = "linked/build-manifest.json"
    assert_true(has_error(validate(candidate, path), "symlink path components"), "parent symlink")

    path, candidate, _ = make_valid_artifact(tmpdir / "file-symlink")
    original = path.parent / "manifests" / "build-manifest.json"
    file_link = path.parent / "manifests" / "build-link.json"
    file_link.symlink_to(original)
    artifact_entry(candidate, "build-manifest")["path"] = "manifests/build-link.json"
    assert_true(has_error(validate(candidate, path), "symlink path components"), "file symlink")

    path, candidate, _ = make_valid_artifact(tmpdir / "missing")
    artifact_entry(candidate, "build-manifest")["path"] = "manifests/missing.json"
    assert_true(has_error(validate(candidate, path), "could not lstat"), "missing path")

    path, candidate, _ = make_valid_artifact(tmpdir / "empty")
    entry = artifact_entry(candidate, "build-manifest")
    empty = path.parent / "manifests" / "empty.json"
    empty.write_bytes(b"")
    entry["path"] = "manifests/empty.json"
    assert_true(has_error(validate(candidate, path), "nonempty file"), "empty")


def test_hashes_duplicate_json_and_unknown_metadata_fail_closed(tmpdir: Path) -> None:
    artifact_path, payload, _ = make_valid_artifact(tmpdir)
    candidate = copy.deepcopy(payload)
    artifact_entry(candidate, "build-manifest")["sha256"] = "0" * 64
    assert_true(has_error(validate(candidate, artifact_path), "all-zero SHA-256"), "zero hash")

    candidate = copy.deepcopy(payload)
    artifact_entry(candidate, "build-manifest")["sha256"] = "f" * 64
    assert_true(
        has_error(validate(candidate, artifact_path), "does not match file content"),
        "hash",
    )

    candidate = copy.deepcopy(payload)
    candidate["operator_serial"] = "not-accepted"
    assert_true(
        has_error(validate(candidate, artifact_path), "operator_serial is not allowed"),
        "closed",
    )

    candidate = copy.deepcopy(payload)
    entry = artifact_entry(candidate, "resolver-attestation")
    evidence_path = artifact_path.parent / entry["path"]
    evidence_path.write_text(
        '{"schema_version":1,"schema_version":1}\n',
        encoding="utf-8",
    )
    entry["sha256"] = sha256(evidence_path)
    assert_true(
        has_error(validate(candidate, artifact_path), "duplicate JSON key"),
        "duplicate key",
    )


def test_filesystem_and_type_confusion_become_errors_not_exceptions(tmpdir: Path) -> None:
    artifact_path, payload, _ = make_valid_artifact(tmpdir)
    for field in (
        "evidence_artifacts",
        "duts",
        "rigs",
        "cases",
    ):
        candidate = copy.deepcopy(payload)
        candidate[field] = {"not": "an array"}
        errors = validate(candidate, artifact_path)
        assert_true(bool(errors), f"{field} type confusion")

    candidate = copy.deepcopy(payload)
    case_result(candidate, "BSC-02")["runs"][0]["roles"] = {"bad": True}
    assert_true(bool(validate(candidate, artifact_path)), "role type confusion")

    candidate = copy.deepcopy(payload)
    artifact_entry(candidate, "build-manifest")["path"] = []
    assert_true(bool(validate(candidate, artifact_path)), "path type confusion")

    artifact_link = tmpdir / "qualification-link.json"
    artifact_link.symlink_to(artifact_path)
    assert_true(
        has_error(
            qualification.validate_artifact_file(
                artifact_link,
                TARGET_SHA,
                now=VALIDATION_NOW,
            ),
            "must not be a symlink",
        ),
        "artifact symlink",
    )


def main() -> int:
    tests = (
        test_pinned_profile_has_exact_case_specific_contracts,
        test_incomplete_profile_rejects_synthetic_pass_pack,
        test_self_authored_build_and_board_integrity_cannot_activate,
        test_profile_ready_mutation_cannot_authenticate_forged_provenance,
        test_profile_scope_digest_and_case_set_cannot_be_overridden,
        test_target_sha_must_be_nonzero_existing_head,
        test_repository_state_comes_from_git_tree_and_live_cleanliness,
        test_future_or_reversed_time_is_rejected_everywhere,
        test_build_manifest_binds_command_commit_clean_tree_and_binary,
        test_recomputed_self_declared_build_digests_cannot_forge_provenance,
        test_each_build_commitment_layer_fails_closed,
        test_firmware_image_validation_requires_complete_esp32s3_image,
        test_resolver_attestation_is_sanitized_hashed_and_alias_bound,
        test_board_inventory_commitment_is_private_and_exact,
        test_execution_and_case_source_bind_target_tracked_contracts,
        test_case_runs_roles_and_artifacts_are_exact_and_nonreusable,
        test_case_specific_facts_stimuli_faults_and_barriers_are_parsed,
        test_case_source_is_independent_exact_and_causally_ordered,
        test_qualification_cannot_predate_target_commit,
        test_reset_vbus_instrumentation_and_safety_cannot_be_weakened,
        test_paths_lstat_every_component_and_reject_operational_shapes,
        test_hashes_duplicate_json_and_unknown_metadata_fail_closed,
        test_filesystem_and_type_confusion_become_errors_not_exceptions,
    )
    with tempfile.TemporaryDirectory(prefix="bug_squash_hil_qualification_") as tmp:
        root = Path(tmp)
        for index, test in enumerate(tests):
            case_dir = root / f"case-{index}"
            case_dir.mkdir()
            test(case_dir)
    print(
        f"[bug-squash-hil-qualification] {len(tests)} adversarial regression groups passed"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
