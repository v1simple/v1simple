#!/usr/bin/env python3
"""Regression tests for the typed BSC-09 production qualification collector."""

from __future__ import annotations

import copy
from dataclasses import replace
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import bug_squash_hil_adapter_protocol as adapter_protocol
import bug_squash_hil_rig_adapters as rig_adapters
import hil_board_inventory_test_support as inventory_test_support
import run_bug_squash_hil as runner


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run_bug_squash_hil.py"
BOARD_SIGNER_TEMP = tempfile.TemporaryDirectory(
    prefix="bug-squash-hil-bsc09-board-signer-"
)
BOARD_SIGNING_KEY, BOARD_TRUST_ROOT = inventory_test_support.create_test_signer(
    Path(BOARD_SIGNER_TEMP.name)
)


def digest(label: str) -> str:
    return hashlib.sha256(label.encode("utf-8")).hexdigest()


def valid_expected(
    run_index: int,
    *,
    session_id: str = "bsc09-session-0123456789abcdef",
    attempt_id: str | None = None,
    target_sha: str = "a" * 40,
) -> dict[str, object]:
    descriptor = runner.load_bsc09_case_descriptor()
    expected: dict[str, object] = {
        "case_id": runner.BSC09_CASE_ID,
        "role_id": "wifi-dual-consumer-scan",
        "run_index": run_index,
        "session_id": session_id,
        "attempt_id": attempt_id or f"attempt-bsc09-{run_index:02d}-0123456789abcdef",
        "target_sha": target_sha,
        "dut_alias": "release",
        "rig_alias": "rig",
        "execution_mode": "simulated",
        "hardware_observed": False,
        "qualification_profile": {
            "profile_id": runner.BSC09_PROFILE_ID,
            "profile_version": runner.BSC09_PROFILE_VERSION,
            "profile_sha256": runner.qualification.PINNED_PROFILE_SHA256,
        },
        "case_descriptor": descriptor,
        "case_descriptor_sha256": runner.bsc09_descriptor_commitment(descriptor),
        "dut_capabilities": list(runner.BSC09_DUT_CAPABILITIES),
        "rig_capabilities": list(runner.BSC09_RIG_CAPABILITIES),
    }
    expected["adapter_request_sha256"] = runner.bsc09_adapter_request_commitment(expected)
    return expected


def rebind_manifest(record: dict[str, object]) -> None:
    manifest = record["raw_artifact_manifest"]
    assert isinstance(manifest, dict)
    manifest["manifest_commitment_sha256"] = runner.bsc09_raw_manifest_commitment(manifest)


def rebind_record(record: dict[str, object]) -> None:
    record["evidence_binding_sha256"] = runner.bsc09_record_commitment(record)


def valid_record(expected: dict[str, object]) -> dict[str, object]:
    run_index = int(expected["run_index"])
    generation = 100 + run_index
    captures = {
        field: digest(f"bsc09-run-{run_index}-{field}")
        for field in runner.BSC09_CAPTURE_COMMITMENTS
    }
    artifact_contract = rig_adapters.get_rig_adapter("BSC-09").roles[0].raw_artifacts
    artifacts = [
        {
            "role": contract.role,
            "filename": contract.filename,
            "size_bytes": 100 + index,
            "sha256": captures[runner.BSC09_RAW_CAPTURE_FIELDS[contract.role]],
        }
        for index, contract in enumerate(artifact_contract, start=1)
    ]
    manifest: dict[str, object] = {
        "schema_version": 1,
        "protocol_version": rig_adapters.ADAPTER_PROTOCOL_VERSION,
        "request_commitment_sha256": expected["adapter_request_sha256"],
        "artifacts": artifacts,
    }
    manifest["manifest_commitment_sha256"] = runner.bsc09_raw_manifest_commitment(manifest)
    lifecycle_times = (1000, 2000, 4000, 6000, 7000, 7500, 8000)
    lifecycle = []
    for sequence, (event_contract, elapsed_ms) in enumerate(
        zip(runner.BSC09_LIFECYCLE_EVENTS, lifecycle_times, strict=True), start=1
    ):
        event, consumer, pending, snapshots, running, released, aborted = event_contract
        lifecycle.append(
            {
                "event": event,
                "consumer": consumer,
                "generation": generation,
                "network_count": 3 if sequence >= 5 else -1,
                "pending_consumer_mask": pending,
                "snapshot_consumer_mask": snapshots,
                "running": running,
                "released": released,
                "aborted": aborted,
                "sequence": sequence,
                "elapsed_ms": elapsed_ms,
            }
        )
    browser_times = (2000, 3000, 4000, 5000, 5500, 6000, 6500, 8000)
    browser_run_ids = ((1, 1), (1, 2), (3, 3), (3, 4), (3, 4), (5, 5), (5, 5), (5, 5))
    browser_steps = []
    for sequence, (contract, elapsed_ms, run_ids) in enumerate(
        zip(runner.BSC09_BROWSER_ACTIONS, browser_times, browser_run_ids, strict=True), start=1
    ):
        action, method, path, status, scanning, modal, spinner, error, accepted = contract
        browser_steps.append(
            {
                "action": action,
                "sequence": sequence,
                "elapsed_ms": elapsed_ms,
                "request_run_id": run_ids[0],
                "current_run_id": run_ids[1],
                "http_method": method,
                "path": path,
                "response_status": status,
                "response_scanning": scanning,
                "modal_open": modal,
                "spinner_active": spinner,
                "error_visible": error,
                "response_accepted": accepted,
            }
        )
    started = datetime(2026, 7, 22, 12, run_index, 0, tzinfo=timezone.utc)
    completed = started + timedelta(seconds=20)
    record: dict[str, object] = {
        "schema_version": 1,
        "case_id": expected["case_id"],
        "role_id": expected["role_id"],
        "run_index": expected["run_index"],
        "session_id": expected["session_id"],
        "attempt_id": expected["attempt_id"],
        "target_sha": expected["target_sha"],
        "dut_alias": expected["dut_alias"],
        "rig_alias": expected["rig_alias"],
        "execution_mode": expected["execution_mode"],
        "hardware_observed": expected["hardware_observed"],
        "qualification_profile": copy.deepcopy(expected["qualification_profile"]),
        "case_descriptor": copy.deepcopy(expected["case_descriptor"]),
        "case_descriptor_sha256": expected["case_descriptor_sha256"],
        "adapter_request_sha256": expected["adapter_request_sha256"],
        "started_at_utc": started.isoformat(timespec="seconds").replace("+00:00", "Z"),
        "completed_at_utc": completed.isoformat(timespec="seconds").replace("+00:00", "Z"),
        "duration_ms": 20_000,
        "dut_capabilities": list(runner.BSC09_DUT_CAPABILITIES),
        "rig_capabilities": list(runner.BSC09_RIG_CAPABILITIES),
        "firmware": {
            "environment": runner.BSC09_PRODUCTION_ENVIRONMENT,
            "build_kind": "production",
            "target_sha": expected["target_sha"],
            "binary_sha256": digest(f"bsc09-run-{run_index}-firmware-binary"),
            "hil_fault_control_active": False,
            "capture_sha256": captures["firmware_build_sha256"],
        },
        "stimuli": [
            {"id": stimulus_id, "sequence": sequence, "elapsed_ms": elapsed_ms, "result": "pass"}
            for sequence, (stimulus_id, elapsed_ms) in enumerate(
                zip(runner.BSC09_STIMULUS_IDS, (1000, 2000, 3000, 4000, 5000, 6000), strict=True),
                start=1,
            )
        ],
        "scan_lifecycle": {
            "source": "WiFiScanTrace",
            "capture_sha256": captures["wifi_scan_projection_sha256"],
            "events": lifecycle,
        },
        "browser_trace": {
            "capture_sha256": captures["browser_projection_sha256"],
            "steps": browser_steps,
        },
        "barriers": [
            {
                "id": "scan-overlap-observed",
                "sequence": 1,
                "generation": generation,
                "ready_elapsed_ms": 2000,
                "released_elapsed_ms": 7000,
                "source_event_sequences": [1, 2, 5],
                "timed_out": False,
            }
        ],
        "heap_trace": {
            "capture_sha256": captures["heap_projection_sha256"],
            "integrity_ok": True,
            "samples": [
                {
                    "phase": "before-scan",
                    "sequence": 1,
                    "elapsed_ms": 500,
                    "free_heap_bytes": 500_000 + run_index * 100,
                    "largest_block_bytes": 300_000 + run_index * 100,
                },
                {
                    "phase": "overlapped-scan",
                    "sequence": 2,
                    "elapsed_ms": 4500,
                    "free_heap_bytes": 480_000 + run_index * 100,
                    "largest_block_bytes": 280_000 + run_index * 100,
                },
                {
                    "phase": "released",
                    "sequence": 3,
                    "elapsed_ms": 8500,
                    "free_heap_bytes": 500_000 + run_index * 100,
                    "largest_block_bytes": 300_000 + run_index * 100,
                },
            ],
        },
        "wifi_mode_trace": {
            "capture_sha256": captures["wifi_mode_projection_sha256"],
            "samples": [
                {"phase": "maintenance-start", "sequence": 1, "elapsed_ms": 1000, "mode": "AP_STA", "settled": False},
                {"phase": "scan-overlap", "sequence": 2, "elapsed_ms": 4500, "mode": "AP_STA", "settled": False},
                {"phase": "settled", "sequence": 3, "elapsed_ms": 9000, "mode": "AP_STA", "settled": True},
            ],
        },
        "health": {
            "expected_kind": "none",
            "planned_count": 0,
            "observed_count": 0,
            "unexpected_count": 0,
            "panic_observed": False,
            "watchdog_reset_observed": False,
            "heap_corruption_observed": False,
            "capture_sha256": captures["health_projection_sha256"],
        },
        "facts": {
            "maintenance-snapshot-stable": True,
            "ui-snapshot-stable": True,
            "consumer-generations-isolated": True,
            "retry-succeeded": True,
            "spinner-terminated": True,
            "wifi-mode-settled": True,
            "accumulating-scan-heap-loss-observed": False,
        },
        "capture_commitments": captures,
        "raw_artifact_manifest": manifest,
    }
    rebind_record(record)
    return record


def valid_raw_projections(expected: dict[str, object]) -> dict[str, dict[str, object]]:
    record = valid_record(expected)
    case_fields = {
        "schema_version",
        "case_id",
        "role_id",
        "run_index",
        "session_id",
        "attempt_id",
        "target_sha",
        "dut_alias",
        "rig_alias",
        "execution_mode",
        "hardware_observed",
        "qualification_profile",
        "case_descriptor_sha256",
        "adapter_request_sha256",
        "started_at_utc",
        "completed_at_utc",
        "duration_ms",
        "dut_capabilities",
        "rig_capabilities",
        "stimuli",
        "barriers",
        "facts",
    }

    def projection(field: str) -> dict[str, object]:
        value = copy.deepcopy(record[field])
        assert isinstance(value, dict)
        value.pop("capture_sha256")
        return {
            "schema_version": 1,
            "request_commitment_sha256": expected["adapter_request_sha256"],
            **value,
        }

    return {
        "browser-projection": projection("browser_trace"),
        "case-observation": {key: copy.deepcopy(record[key]) for key in case_fields},
        "firmware-build": projection("firmware"),
        "health-projection": projection("health"),
        "heap-projection": projection("heap_trace"),
        "wifi-mode-projection": projection("wifi_mode_trace"),
        "wifi-scan-projection": projection("scan_lifecycle"),
    }


def write_executable(path: Path, source: str) -> None:
    path.write_text(source, encoding="utf-8")
    path.chmod(0o755)


def initialize_repository(path: Path) -> str:
    subprocess.run(["git", "init", "-q"], cwd=path, check=True)
    (path / "README.md").write_text("BSC-09 runner fixture\n", encoding="utf-8")
    subprocess.run(["git", "add", "README.md"], cwd=path, check=True)
    subprocess.run(
        [
            "git",
            "-c",
            "user.name=Runner Test",
            "-c",
            "user.email=runner@example.invalid",
            "commit",
            "-q",
            "-m",
            "fixture",
        ],
        cwd=path,
        check=True,
    )
    return subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=path,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def prepare_fixture(root: Path) -> dict[str, Path | str]:
    repository = root / "repository"
    repository.mkdir()
    target_sha = initialize_repository(repository)
    port = root / "tty-private-fixture"
    port.write_text("", encoding="utf-8")
    template = root / "template.json"
    inventory = root / "inventory.json"
    ports = root / "ports.json"
    template.write_text(
        json.dumps({"schema_version": 1, "description": "test", "boards": []}),
        encoding="utf-8",
    )
    inventory.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "boards": [
                    {
                        "alias": "release",
                        "capabilities": list(runner.BSC09_DUT_CAPABILITIES),
                        "usb_serial": "SECRET-BSC09-USB",
                    },
                    {"alias": "rig", "capabilities": list(runner.BSC09_RIG_CAPABILITIES)},
                ],
            }
        ),
        encoding="utf-8",
    )
    inventory_test_support.sign_inventory(inventory, BOARD_SIGNING_KEY)
    ports.write_text(
        json.dumps([{"port": str(port), "serial_number": "SECRET-BSC09-USB"}]),
        encoding="utf-8",
    )
    fake_pio = root / "fake-pio.py"
    write_executable(fake_pio, "#!/usr/bin/env python3\nraise SystemExit(0)\n")
    adapter = root / "fake-bsc09-adapter.py"
    write_executable(
        adapter,
        """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

root = os.environ['V1SIMPLE_BSC09_TEST_ROOT']
sys.path.insert(0, root + '/scripts')
import bug_squash_hil_adapter_protocol as adapter_protocol
import run_bug_squash_hil as runner
import test_bug_squash_hil_bsc09 as tests

def argument(name):
    return sys.argv[sys.argv.index(name) + 1]

run_index = int(argument('--run-index'))
descriptor = runner.load_bsc09_case_descriptor()
expected = {
    'case_id': argument('--case'),
    'role_id': argument('--role-id'),
    'run_index': run_index,
    'session_id': argument('--session-id'),
    'attempt_id': argument('--attempt-id'),
    'target_sha': argument('--target-sha'),
    'dut_alias': argument('--dut-alias'),
    'rig_alias': argument('--rig-alias'),
    'execution_mode': 'simulated',
    'hardware_observed': False,
    'qualification_profile': {
        'profile_id': argument('--profile-id'),
        'profile_version': int(argument('--profile-version')),
        'profile_sha256': argument('--profile-sha256'),
    },
    'case_descriptor': descriptor,
    'case_descriptor_sha256': argument('--case-descriptor-sha256'),
    'adapter_request_sha256': argument('--adapter-request-sha256'),
    'dut_capabilities': list(runner.BSC09_DUT_CAPABILITIES),
    'rig_capabilities': list(runner.BSC09_RIG_CAPABILITIES),
}
raw_directory = Path(argument('--raw-artifact-dir'))
projections = tests.valid_raw_projections(expected)
role = runner.rig_adapters.get_rig_adapter('BSC-09').roles[0]
for contract in role.raw_artifacts:
    (raw_directory / contract.filename).write_bytes(
        adapter_protocol.canonical_json_bytes(projections[contract.role]) + b'\\n'
    )
sys.stdout.write(json.dumps({
    'schema_version': 1,
    'protocol_version': runner.rig_adapters.ADAPTER_PROTOCOL_VERSION,
    'status': 'complete',
    'request_commitment_sha256': expected['adapter_request_sha256'],
    'nonce': expected['attempt_id'],
}))
""",
    )
    return {
        "repository": repository,
        "target_sha": target_sha,
        "port": port,
        "template": template,
        "inventory": inventory,
        "board_signing_key": BOARD_SIGNING_KEY,
        "board_trust_root": BOARD_TRUST_ROOT,
        "ports": ports,
        "pio": fake_pio,
        "adapter": adapter,
    }


def drop_capability(fixture: dict[str, Path | str], alias: str, capability: str) -> None:
    inventory_path = Path(fixture["inventory"])
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    board = next(item for item in inventory["boards"] if item["alias"] == alias)
    board["capabilities"].remove(capability)
    inventory_path.write_text(json.dumps(inventory), encoding="utf-8")
    inventory_test_support.sign_inventory(
        inventory_path,
        Path(fixture["board_signing_key"]),
    )


def run_fixture(
    fixture: dict[str, Path | str],
    root: Path,
    *,
    runs: int = 3,
    production_replay: bool = False,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    out_dir = root / "bsc09-out"
    command = [
        sys.executable,
        "-B",
        str(RUNNER),
        "--case",
        "BSC-09",
        "--board",
        "release",
        "--rig",
        "rig",
        "--runs",
        str(runs),
        "--repo-root",
        str(fixture["repository"]),
        "--template",
        str(fixture["template"]),
        "--inventory",
        str(fixture["inventory"]),
        "--ports-json",
        str(fixture["ports"]),
        "--pio-command",
        str(fixture["pio"]),
        "--case-adapter",
        str(fixture["adapter"]),
        "--out-dir",
        str(out_dir),
    ]
    if production_replay:
        command.append("--production-replay")
    environment = inventory_test_support.test_environment(
        Path(fixture["board_trust_root"])
    )
    environment.update(
        {
            "PYTHONDONTWRITEBYTECODE": "1",
            "V1SIMPLE_HIL_TEST_HOOKS": "1",
            "V1SIMPLE_BSC09_TEST_ROOT": str(ROOT),
        }
    )
    return (
        subprocess.run(
            command,
            cwd=ROOT,
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        ),
        out_dir,
    )


class Bsc09CollectorTests(unittest.TestCase):
    def test_valid_three_run_collection_is_bound_and_nonqualifying(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            fixture = prepare_fixture(root)
            completed, out_dir = run_fixture(fixture, root)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            result = json.loads((out_dir / "collection_result.json").read_text(encoding="utf-8"))
            self.assertEqual(result["result"], "TEST_PASS")
            self.assertEqual(result["runs_required"], 3)
            self.assertEqual(result["runs_completed"], 3)
            self.assertEqual(result["execution_mode"], "simulated")
            self.assertFalse(result["hardware_observed"])
            self.assertFalse(result["authoritative"])
            self.assertFalse(result["physical_collection_completed"])
            self.assertTrue(result["non_qualifying"])
            self.assertEqual(result["qualification_status"], "BLOCKED")
            self.assertEqual(
                result["qualification_blockers"],
                [
                    "tracked-rig-adapter-not-implemented",
                ],
            )
            records = [
                json.loads((out_dir / f"attempt-{run_index}.json").read_text(encoding="utf-8"))
                for run_index in range(1, 4)
            ]
            runner.validate_bsc09_runs(records)
            self.assertEqual(len({row["scan_lifecycle"]["events"][0]["generation"] for row in records}), 3)
            self.assertFalse((out_dir / "qualification_result.json").exists())
            public_output = completed.stdout + completed.stderr + json.dumps(result)
            self.assertNotIn("SECRET-BSC09-USB", public_output)
            self.assertNotIn(str(fixture["port"]), public_output)

    def test_semantic_mutations_fail_after_rebinding(self) -> None:
        mutations = {
            "bool-run-index": lambda row: row.__setitem__("run_index", True),
            "bool-generation": lambda row: row["scan_lifecycle"]["events"][0].__setitem__("generation", True),
            "fact-int": lambda row: row["facts"].__setitem__("retry-succeeded", 1),
            "missing-event": lambda row: row["scan_lifecycle"]["events"].pop(),
            "extra-event": lambda row: row["scan_lifecycle"]["events"].append(copy.deepcopy(row["scan_lifecycle"]["events"][-1])),
            "reordered-events": lambda row: row["scan_lifecycle"]["events"].__setitem__(slice(1, 3), list(reversed(row["scan_lifecycle"]["events"][1:3]))),
            "generation-mismatch": lambda row: row["scan_lifecycle"]["events"][3].__setitem__("generation", 999),
            "ui-cancelled-shared-scan": lambda row: row["scan_lifecycle"]["events"][2].update({"event": "consumer-cancelled", "aborted": True}),
            "snapshot-mask": lambda row: row["scan_lifecycle"]["events"][4].__setitem__("snapshot_consumer_mask", 2),
            "snapshot-cardinality": lambda row: row["scan_lifecycle"]["events"][6].__setitem__("network_count", 2),
            "release-count": lambda row: row["scan_lifecycle"]["events"][5].__setitem__("released", True),
            "stale-response-accepted": lambda row: row["browser_trace"]["steps"][4].__setitem__("response_accepted", True),
            "drop-error-hidden": lambda row: row["browser_trace"]["steps"][3].__setitem__("error_visible", False),
            "retry-not-post": lambda row: row["browser_trace"]["steps"][5].__setitem__("http_method", "GET"),
            "resumed-poll-missing": lambda row: row["browser_trace"]["steps"].pop(6),
            "spinner-false-early": lambda row: row["browser_trace"]["steps"][6].__setitem__("spinner_active", False),
            "spinner-left-active": lambda row: row["browser_trace"]["steps"][7].__setitem__("spinner_active", True),
            "wifi-mode": lambda row: row["wifi_mode_trace"]["samples"][2].__setitem__("mode", "STA"),
            "wifi-not-settled": lambda row: row["wifi_mode_trace"]["samples"][2].__setitem__("settled", False),
            "heap-order": lambda row: row["heap_trace"]["samples"][1].__setitem__("elapsed_ms", 100),
            "heap-loss": lambda row: row["heap_trace"]["samples"][2].__setitem__("free_heap_bytes", 400_000),
            "heap-integrity": lambda row: row["heap_trace"].__setitem__("integrity_ok", False),
            "unexpected-reset": lambda row: row["health"].__setitem__("unexpected_count", 1),
            "panic": lambda row: row["health"].__setitem__("panic_observed", True),
            "watchdog": lambda row: row["health"].__setitem__("watchdog_reset_observed", True),
            "utc-duration": lambda row: row.__setitem__("duration_ms", 19_999),
            "dut-capability": lambda row: row["dut_capabilities"].pop(),
            "rig-capability": lambda row: row["rig_capabilities"].pop(),
            "capture-extra": lambda row: row["capture_commitments"].__setitem__("unrelated_sha256", digest("unrelated")),
            "capture-unrelated": lambda row: row["capture_commitments"].__setitem__("browser_projection_sha256", digest("unrelated")),
            "capture-reuse": lambda row: row["capture_commitments"].__setitem__("health_projection_sha256", row["capture_commitments"]["heap_projection_sha256"]),
            "manifest-order": lambda row: row["raw_artifact_manifest"]["artifacts"].__setitem__(slice(0, 2), list(reversed(row["raw_artifact_manifest"]["artifacts"][0:2]))),
            "manifest-reuse": lambda row: row["raw_artifact_manifest"]["artifacts"][1].__setitem__("sha256", row["raw_artifact_manifest"]["artifacts"][0]["sha256"]),
            "session-replay": lambda row: row.__setitem__("session_id", "bsc09-replayed-session"),
            "attempt-replay": lambda row: row.__setitem__("attempt_id", "attempt-replayed"),
        }
        for name, mutate in mutations.items():
            expected = valid_expected(1)
            record = valid_record(expected)
            mutate(record)
            if name.startswith("manifest-"):
                rebind_manifest(record)
            rebind_record(record)
            with self.subTest(name=name), self.assertRaises(runner.RunnerError):
                runner.validate_bsc09_adapter_record(record, expected=expected)

    def test_cross_run_generation_capture_attempt_and_session_replay_fail(self) -> None:
        expected = [valid_expected(index) for index in range(1, 4)]
        records = [valid_record(item) for item in expected]
        runner.validate_bsc09_runs(records)

        generation_reuse = copy.deepcopy(records)
        reused_generation = generation_reuse[1]["scan_lifecycle"]["events"][0]["generation"]
        for event in generation_reuse[2]["scan_lifecycle"]["events"]:
            event["generation"] = reused_generation
        generation_reuse[2]["barriers"][0]["generation"] = reused_generation
        rebind_record(generation_reuse[2])
        with self.assertRaises(runner.RunnerError):
            runner.validate_bsc09_runs(generation_reuse)

        for field in ("attempt_id", "session_id"):
            replay = copy.deepcopy(records)
            replay[2][field] = replay[1][field] if field == "attempt_id" else "escaped-session"
            rebind_record(replay[2])
            with self.subTest(field=field), self.assertRaises(runner.RunnerError):
                runner.validate_bsc09_runs(replay)

        capture_reuse = copy.deepcopy(records)
        capture_reuse[2]["capture_commitments"]["browser_projection_sha256"] = capture_reuse[1]["capture_commitments"]["browser_projection_sha256"]
        with self.assertRaises(runner.RunnerError):
            runner.validate_bsc09_runs(capture_reuse)

        accumulating_heap_loss = copy.deepcopy(records)
        for index, record in enumerate(accumulating_heap_loss):
            samples = record["heap_trace"]["samples"]
            samples[0]["free_heap_bytes"] = 500_000 - index * 10_000
            samples[2]["free_heap_bytes"] = 500_000 - index * 10_000
        with self.assertRaises(runner.RunnerError):
            runner.validate_bsc09_runs(accumulating_heap_loss)

    def test_hardware_capabilities_are_all_required(self) -> None:
        cases = [("release", item) for item in runner.BSC09_DUT_CAPABILITIES]
        cases += [("rig", item) for item in runner.BSC09_RIG_CAPABILITIES]
        for alias, capability in cases:
            with self.subTest(alias=alias, capability=capability), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                fixture = prepare_fixture(root)
                drop_capability(fixture, alias, capability)
                completed, out_dir = run_fixture(fixture, root)
                self.assertNotEqual(completed.returncode, 0)
                payload = json.loads(completed.stdout)
                self.assertEqual(payload["error"]["code"], "case_board_resolution_failed")
                self.assertFalse((out_dir / "collection_result.json").exists())

    def test_authoritative_admission_blocks_before_git_output_or_discovery(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            out_dir = root / "must-not-exist"
            inventory = root / "missing-inventory.json"
            args = runner.build_parser().parse_args(
                [
                    "--case",
                    "BSC-09",
                    "--board",
                    "opaque-dut",
                    "--rig",
                    "opaque-rig",
                    "--runs",
                    "3",
                    "--inventory",
                    str(inventory),
                    "--out-dir",
                    str(out_dir),
                ]
            )
            with mock.patch.object(runner, "test_hooks_enabled", return_value=False), mock.patch.object(
                runner, "read_git_state"
            ) as read_git, mock.patch.object(runner, "resolve_bsc09_hardware") as resolve_hardware:
                with self.assertRaises(runner.RunnerError) as raised:
                    runner.run_bsc09_case(args)
            self.assertEqual(raised.exception.code, "case_rig_adapter_unavailable")
            read_git.assert_not_called()
            resolve_hardware.assert_not_called()
            self.assertFalse(out_dir.exists())
            self.assertFalse(inventory.exists())

    def test_adapter_hash_claims_without_exact_raw_files_fail(self) -> None:
        adapters = {
            "no-files": (
                """#!/usr/bin/env python3
import json
import sys
def argument(name):
    return sys.argv[sys.argv.index(name) + 1]
print(json.dumps({
    'schema_version': 1,
    'protocol_version': 1,
    'status': 'complete',
    'request_commitment_sha256': argument('--adapter-request-sha256'),
    'nonce': argument('--attempt-id'),
}))
""",
                "raw_artifact_set_invalid",
            ),
            "adapter-hashes": (
                """#!/usr/bin/env python3
import json
import sys
def argument(name):
    return sys.argv[sys.argv.index(name) + 1]
print(json.dumps({
    'schema_version': 1,
    'protocol_version': 1,
    'status': 'complete',
    'request_commitment_sha256': argument('--adapter-request-sha256'),
    'nonce': argument('--attempt-id'),
    'artifact_sha256': {'wifi-scan-projection': 'f' * 64},
}))
""",
                "protocol_invalid",
            ),
        }
        for name, (source, code) in adapters.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                fixture = prepare_fixture(root)
                write_executable(Path(fixture["adapter"]), source)
                completed, out_dir = run_fixture(fixture, root)
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(json.loads(completed.stdout)["error"]["code"], code)
                self.assertFalse((out_dir / "collection_result.json").exists())

    def test_bsc09_verified_read_rejects_projection_changed_after_manifest(self) -> None:
        expected = valid_expected(1)
        projections = valid_raw_projections(expected)
        role = rig_adapters.get_rig_adapter("BSC-09").roles[0]
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            raw_directory = root / "raw"
            raw_directory.mkdir()
            for contract in role.raw_artifacts:
                (raw_directory / contract.filename).write_bytes(
                    adapter_protocol.canonical_json_bytes(projections[contract.role]) + b"\n"
                )
            manifest = adapter_protocol.collect_raw_artifacts(
                raw_directory=raw_directory,
                role=role,
                request_commitment_sha256=str(expected["adapter_request_sha256"]),
                manifest_path=root / "manifest.json",
            )
            browser = raw_directory / "browser-projection.json"
            browser.write_bytes(browser.read_bytes() + b" ")
            with self.assertRaises(adapter_protocol.AdapterProtocolError) as raised:
                adapter_protocol.read_collected_raw_artifacts(
                    raw_directory=raw_directory,
                    role=role,
                    manifest=manifest,
                )
            self.assertEqual(raised.exception.code, "raw_artifact_changed")

    def test_tracked_activation_uses_admitted_source_and_forbids_override(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            repository = Path(raw)
            source_path = Path("scripts/bug_squash_hil_bsc09_rig.py")
            tracked_source = repository / source_path
            tracked_source.parent.mkdir()
            tracked_source.write_text("def main():\n    return 0\n", encoding="utf-8")
            descriptor = replace(
                rig_adapters.get_rig_adapter("BSC-09"),
                status="implemented",
                source_path=source_path.as_posix(),
                entrypoint="main",
            )
            source_sha256 = hashlib.sha256(tracked_source.read_bytes()).hexdigest()
            admission = runner.RigAdapterAdmission(
                adapter=descriptor,
                simulated=False,
                git_state=runner.GitState("a" * 40, True),
                source_sha256=source_sha256,
            )
            args = runner.build_parser().parse_args(
                ["--case", "BSC-09", "--board", "release", "--rig", "rig", "--runs", "3"]
            )
            resolved = runner.resolve_bsc09_adapter_execution(
                args,
                admission=admission,
                repository=repository,
            )
            self.assertEqual(resolved, (tracked_source, "main", source_sha256, True))

            args.case_adapter = repository / "untracked.py"
            with self.assertRaises(runner.RunnerError) as raised:
                runner.resolve_bsc09_adapter_execution(
                    args,
                    admission=admission,
                    repository=repository,
                )
            self.assertEqual(raised.exception.code, "untrusted_override")

    def test_private_network_identifiers_are_rejected_before_projection_commitment(self) -> None:
        private_payloads = (
            {"schema_version": 1, "steps": [], "ssid": "PrivateNetwork"},
            {"schema_version": 1, "steps": [{"detail": "password=secret"}]},
            {"schema_version": 1, "steps": [{"detail": "bssid=aa:bb:cc:dd:ee:ff"}]},
            {"schema_version": 1, "steps": [{"detail": "192.168.4.1"}]},
            {"schema_version": 1, "steps": [], "network_sha256": "f" * 64},
        )
        for payload in private_payloads:
            with self.subTest(payload=payload), self.assertRaises(runner.RunnerError) as raised:
                runner.parse_bsc09_projection(
                    json.dumps(payload).encode("utf-8"),
                    role="privacy regression",
                    fields={"steps"},
                )
            self.assertEqual(raised.exception.code, "case_projection_private")

    def test_invalid_run_count_and_production_replay_fail_without_output(self) -> None:
        for options, code in (({"runs": 2}, "invalid_runs"), ({"production_replay": True}, "unsupported_mode")):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                fixture = prepare_fixture(root)
                completed, out_dir = run_fixture(fixture, root, **options)
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(json.loads(completed.stdout)["error"]["code"], code)
                self.assertFalse((out_dir / "collection_result.json").exists())


if __name__ == "__main__":
    unittest.main()
