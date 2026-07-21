#!/usr/bin/env python3
"""Run fail-closed bug-squash hardware jobs through an explicit board alias.

The final device-suite mode wraps the legacy device runner with exact resolver
selection, a full target SHA, fail-closed transport handling, production-image
restoration, and a sanitized result. Scenario case drivers intentionally fail
until their typed physical orchestrators are implemented.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
from typing import Mapping, Sequence
import xml.etree.ElementTree as ET

import resolve_hil_board
import check_bug_squash_hil_qualification as qualification


ROOT = Path(__file__).resolve().parents[1]
PRODUCTION_ENVIRONMENT = "waveshare-349"
AUTHORITATIVE_GIT = Path("/usr/bin/git")
EXPECTED_DEVICE_SUITES = (
    "test_device_boot",
    "test_device_heap",
    "test_device_psram",
    "test_device_freertos",
    "test_device_event_bus",
    "test_device_nvs",
    "test_device_battery",
    "test_device_coexistence",
)
CASE_IDS = tuple(f"BSC-{index:02d}" for index in range(2, 15)) + ("BSC-16",)


class RunnerError(Exception):
    """Expected fail-closed runner error safe to print without local values."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def reject_duplicate_json_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    payload: dict[str, object] = {}
    for key, value in pairs:
        if key in payload:
            raise ValueError("duplicate JSON key")
        payload[key] = value
    return payload


@dataclass(frozen=True)
class GitState:
    head_sha: str
    tracked_clean: bool


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_optional_file(path: Path) -> str | None:
    try:
        return sha256_file(path) if path.is_file() else None
    except OSError:
        return None


def read_git_state(repository: Path) -> GitState:
    git_environment = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_")
    }
    git_environment.update(
        {
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_COUNT": "0",
        }
    )
    repository_args = [
        str(AUTHORITATIVE_GIT),
        "--git-dir",
        str(repository / ".git"),
        "--work-tree",
        str(repository),
        "-c",
        "core.fsmonitor=false",
    ]
    head = subprocess.run(
        [*repository_args, "rev-parse", "HEAD"],
        cwd=repository,
        env=git_environment,
        capture_output=True,
        text=True,
        check=False,
    )
    if head.returncode != 0 or len(head.stdout.strip()) != 40:
        raise RunnerError("git_state_unavailable", "target Git commit could not be resolved")
    status = subprocess.run(
        [*repository_args, "status", "--porcelain", "--untracked-files=all"],
        cwd=repository,
        env=git_environment,
        capture_output=True,
        text=True,
        check=False,
    )
    if status.returncode != 0:
        raise RunnerError("git_state_unavailable", "target Git cleanliness could not be resolved")
    return GitState(head_sha=head.stdout.strip(), tracked_clean=not status.stdout.strip())


def require_unchanged_git_state(repository: Path, expected: GitState) -> None:
    observed = read_git_state(repository)
    if (
        observed.head_sha != expected.head_sha
        or not observed.tracked_clean
        or not expected.tracked_clean
    ):
        raise RunnerError("target_mutated", "target Git state changed during hardware execution")


def read_json(path: Path, label: str) -> object:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_json_keys,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("artifact_invalid", f"{label} could not be read as JSON") from exc


def write_json(path: Path, payload: Mapping[str, object]) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except OSError as exc:
        raise RunnerError("artifact_write_failed", "runner artifact could not be written") from exc


def write_bytes(path: Path, data: bytes) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    except OSError as exc:
        raise RunnerError("artifact_write_failed", "runner artifact could not be written") from exc


def resolve_device_board(
    *,
    alias: str,
    template: Path,
    inventory_path: Path,
    ports_json: Path | None,
    pio_command: str,
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    try:
        inventory = resolve_hil_board.load_inventory(template, inventory_path)
        port_records = (
            resolve_hil_board.parse_port_records(
                resolve_hil_board._read_json(ports_json, "serial port inventory")
            )
            if ports_json is not None
            else resolve_hil_board.enumerate_serial_ports(pio_command)
        )
        resolution = resolve_hil_board.resolve_board(
            inventory,
            alias,
            ("device-tests", "serial"),
            port_records=port_records,
        )
        board = inventory.boards[alias]
        binding: dict[str, object] = {
            "schema_version": 1,
            "commitment_salt_hex": secrets.token_hex(32),
            "inventory_record": {
                "alias": board.alias,
                "capabilities": list(board.capabilities),
                "connection": {
                    "lan_base_url": board.lan_base_url,
                    "usb_serial": board.usb_serial,
                },
            },
            "resolution": resolution,
        }
        attestation = qualification.build_board_inventory_attestation(binding)
    except resolve_hil_board.ResolverError as exc:
        raise RunnerError("board_resolution_failed", exc.message) from exc
    return resolution, binding, attestation


def validate_device_manifest(
    manifest_path: Path,
    *,
    target_sha: str,
    board_alias: str,
) -> None:
    payload = read_json(manifest_path, "device manifest")
    if not isinstance(payload, dict):
        raise RunnerError("device_manifest_invalid", "device manifest must be an object")
    if payload.get("schema_version") != 1:
        raise RunnerError("device_manifest_invalid", "device manifest schema is invalid")
    if payload.get("git_sha") != target_sha:
        raise RunnerError("device_manifest_stale", "device manifest does not match the full target SHA")
    if payload.get("board_id") != board_alias:
        raise RunnerError("device_manifest_unbound", "device manifest does not match the board alias")
    if not isinstance(payload.get("run_id"), str) or not payload["run_id"]:
        raise RunnerError("device_manifest_invalid", "device manifest run identity is invalid")
    if payload.get("metrics_file") != "metrics.ndjson" or payload.get("scoring_file") != "scoring.json":
        raise RunnerError("device_manifest_invalid", "device manifest artifact names are invalid")
    if payload.get("base_result") != "PASS" or payload.get("result") not in {
        "PASS",
        "NO_BASELINE",
    }:
        raise RunnerError("device_suite_failed", "device manifest did not record a passing run")

    rows = payload.get("suite_results")
    if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
        raise RunnerError("device_manifest_invalid", "device suite results are missing")
    if len(rows) != len(EXPECTED_DEVICE_SUITES):
        raise RunnerError("device_manifest_incomplete", "device manifest suite count is invalid")
    if any(
        not isinstance(row.get("suite"), str)
        or not isinstance(row.get("status"), str)
        for row in rows
    ):
        raise RunnerError("device_manifest_invalid", "device suite result fields are invalid")
    by_suite = {row.get("suite"): row for row in rows}
    if len(by_suite) != len(rows) or tuple(by_suite) != EXPECTED_DEVICE_SUITES:
        raise RunnerError("device_manifest_incomplete", "device manifest does not contain the exact suite set")
    if any(by_suite[suite].get("status") != "PASS" for suite in EXPECTED_DEVICE_SUITES):
        raise RunnerError("device_transport_failed", "a device suite did not finish with clean transport")
    tracks = payload.get("tracks")
    if not isinstance(tracks, list) or tuple(tracks) != EXPECTED_DEVICE_SUITES:
        raise RunnerError("device_manifest_incomplete", "device manifest tracks are incomplete")


def hash_device_artifacts(device_root: Path) -> dict[str, str]:
    expected = {
        "device.log",
        "manifest.json",
        "metrics.ndjson",
        "scoring.json",
        "suite_index.tsv",
        "summary.md",
    }
    for suite in EXPECTED_DEVICE_SUITES:
        expected.update({f"{suite}.json", f"{suite}.log", f"{suite}.xml"})
    try:
        if not device_root.exists():
            return {}
        if device_root.is_symlink() or not device_root.is_dir():
            raise RunnerError("device_artifacts_invalid", "device artifact directory is invalid")
        artifacts: dict[str, str] = {}
        for path in sorted(device_root.rglob("*")):
            if path.is_symlink():
                raise RunnerError("device_artifacts_invalid", "device artifacts must not use symlinks")
            if path.is_dir():
                continue
            if not path.is_file():
                raise RunnerError("device_artifacts_invalid", "device artifact type is invalid")
            relative = path.relative_to(device_root).as_posix()
            if relative not in expected:
                raise RunnerError("device_artifacts_invalid", "device artifact name is not allowed")
            artifacts[relative] = sha256_file(path)
    except OSError as exc:
        raise RunnerError("device_artifacts_invalid", "device artifacts could not be hashed") from exc
    return artifacts


def require_complete_device_artifacts(artifacts: Mapping[str, str]) -> None:
    expected = {
        "device.log",
        "manifest.json",
        "metrics.ndjson",
        "scoring.json",
        "suite_index.tsv",
        "summary.md",
    }
    for suite in EXPECTED_DEVICE_SUITES:
        expected.update({f"{suite}.json", f"{suite}.log", f"{suite}.xml"})
    if set(artifacts) != expected:
        raise RunnerError("device_artifacts_incomplete", "device artifact set is incomplete")


def validate_underlying_device_artifacts(
    device_root: Path,
    *,
    target_sha: str,
    manifest_result: str,
    manifest: Mapping[str, object],
) -> None:
    for suite in EXPECTED_DEVICE_SUITES:
        payload = read_json(device_root / f"{suite}.json", f"{suite} JSON report")
        if not isinstance(payload, dict) or not isinstance(payload.get("test_suites"), list):
            raise RunnerError("device_report_invalid", "device JSON report is invalid")
        rows = [
            row
            for row in payload["test_suites"]
            if isinstance(row, dict)
            and row.get("env_name") == "device"
            and row.get("status") != "SKIPPED"
        ]
        if len(rows) != 1:
            raise RunnerError("device_report_invalid", "device JSON report must have one executed suite")
        if any(row.get("test_suite_name") != suite for row in rows):
            raise RunnerError("device_report_invalid", "device JSON suite identity is invalid")
        numeric_fields = ("testcase_nums", "pass_nums", "failure_nums", "error_nums")
        if any(
            isinstance(row.get(field), bool) or not isinstance(row.get(field), int)
            for row in rows
            for field in numeric_fields
        ):
            raise RunnerError("device_report_invalid", "device JSON counters are invalid")
        if any(
            row.get("status") != "PASSED"
            or any(row[field] < 0 for field in numeric_fields)
            for row in rows
        ):
            raise RunnerError("device_report_invalid", "device JSON result fields are invalid")
        tests = sum(row["testcase_nums"] for row in rows)
        passes = sum(row["pass_nums"] for row in rows)
        failures = sum(row["failure_nums"] for row in rows)
        errors = sum(row["error_nums"] for row in rows)
        if tests <= 0 or passes != tests or failures != 0 or errors != 0:
            raise RunnerError("device_report_failed", "device JSON report is not clean")
        try:
            xml_root = ET.parse(device_root / f"{suite}.xml").getroot()
        except (OSError, ET.ParseError) as exc:
            raise RunnerError("device_report_invalid", "device XML report is invalid") from exc
        xml_suites = [xml_root] if xml_root.tag == "testsuite" else list(xml_root.findall("testsuite"))
        if len(xml_suites) != 1:
            raise RunnerError("device_report_invalid", "device XML report must have one suite")
        if any(row.attrib.get("name") != f"device:{suite}" for row in xml_suites):
            raise RunnerError("device_report_invalid", "device XML suite identity is invalid")
        try:
            xml_counters = [
                (
                    int(row.attrib.get("tests", "0")),
                    int(row.attrib.get("failures", "0")),
                    int(row.attrib.get("errors", "0")),
                )
                for row in xml_suites
            ]
        except ValueError as exc:
            raise RunnerError("device_report_invalid", "device XML counters are invalid") from exc
        if any(value < 0 for counters in xml_counters for value in counters):
            raise RunnerError("device_report_invalid", "device XML counters must be nonnegative")
        xml_tests = sum(counters[0] for counters in xml_counters)
        xml_failures = sum(counters[1] for counters in xml_counters)
        xml_errors = sum(counters[2] for counters in xml_counters)
        if (
            xml_tests != tests
            or xml_failures != failures
            or xml_errors != errors
            or xml_tests <= 0
            or xml_failures != 0
            or xml_errors != 0
        ):
            raise RunnerError("device_report_failed", "device XML report is not clean")
        try:
            if not (device_root / f"{suite}.log").read_bytes():
                raise RunnerError("device_report_invalid", "device suite log is empty")
        except OSError as exc:
            raise RunnerError("device_report_invalid", "device suite log is unreadable") from exc

    scoring = read_json(device_root / "scoring.json", "device scoring report")
    if not isinstance(scoring, dict) or scoring.get("result") != manifest_result:
        raise RunnerError("device_report_invalid", "device scoring result does not match manifest")
    scoring_manifest = scoring.get("manifest")
    if (
        not isinstance(scoring_manifest, dict)
        or scoring_manifest.get("git_sha") != target_sha
        or scoring_manifest.get("run_id") != manifest.get("run_id")
    ):
        raise RunnerError("device_report_invalid", "device scoring report is not target-bound")

    try:
        with (device_root / "suite_index.tsv").open("r", encoding="utf-8", newline="") as handle:
            index_rows = list(csv.DictReader(handle, delimiter="\t"))
    except (OSError, UnicodeError, csv.Error) as exc:
        raise RunnerError("device_report_invalid", "device suite index is invalid") from exc
    if [row.get("suite") for row in index_rows] != list(EXPECTED_DEVICE_SUITES):
        raise RunnerError("device_report_invalid", "device suite index is incomplete")
    if set(index_rows[0].keys()) != {
        "suite",
        "status",
        "json",
        "xml",
        "log",
        "metric_count",
    }:
        raise RunnerError("device_report_invalid", "device suite index columns are invalid")
    if any(row.get("status") != "PASS" for row in index_rows):
        raise RunnerError("device_report_failed", "device suite index contains a non-pass result")
    for row, suite in zip(index_rows, EXPECTED_DEVICE_SUITES, strict=True):
        expected_paths = {
            "json": device_root / f"{suite}.json",
            "xml": device_root / f"{suite}.xml",
            "log": device_root / f"{suite}.log",
        }
        try:
            paths_invalid = any(
                not isinstance(row.get(field), str)
                or Path(row[field]).resolve() != expected.resolve()
                for field, expected in expected_paths.items()
            )
        except (OSError, ValueError):
            paths_invalid = True
        if paths_invalid:
            raise RunnerError("device_report_invalid", "device suite index paths are invalid")
        metric_count = row.get("metric_count")
        if not isinstance(metric_count, str) or not metric_count.isdigit():
            raise RunnerError("device_report_invalid", "device suite metric count is invalid")

    manifest_rows = manifest.get("suite_results")
    assert isinstance(manifest_rows, list)
    if any(manifest_row != index_row for manifest_row, index_row in zip(manifest_rows, index_rows, strict=True)):
        raise RunnerError("device_report_invalid", "manifest and suite index rows do not match")

    metric_counts = {suite: 0 for suite in EXPECTED_DEVICE_SUITES}
    try:
        metric_lines = (device_root / "metrics.ndjson").read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise RunnerError("device_report_invalid", "device metrics are unreadable") from exc
    for line in metric_lines:
        try:
            metric = json.loads(line, object_pairs_hook=reject_duplicate_json_keys)
        except (json.JSONDecodeError, ValueError) as exc:
            raise RunnerError("device_report_invalid", "device metric record is invalid") from exc
        if not isinstance(metric, dict) or set(metric) != {
            "schema_version",
            "run_id",
            "git_sha",
            "run_kind",
            "suite_or_profile",
            "metric",
            "sample",
            "value",
            "unit",
            "tags",
        }:
            raise RunnerError("device_report_invalid", "device metric schema is invalid")
        suite = metric.get("suite_or_profile")
        value = metric.get("value")
        if (
            metric.get("schema_version") != 1
            or metric.get("run_id") != manifest.get("run_id")
            or metric.get("git_sha") != target_sha
            or metric.get("run_kind") != "device_suite"
            or suite not in metric_counts
            or not isinstance(metric.get("metric"), str)
            or not metric["metric"]
            or not isinstance(metric.get("sample"), str)
            or not metric["sample"]
            or isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(float(value))
            or not isinstance(metric.get("unit"), str)
            or not metric["unit"]
            or not isinstance(metric.get("tags"), dict)
        ):
            raise RunnerError("device_report_invalid", "device metric binding is invalid")
        assert isinstance(suite, str)
        metric_counts[suite] += 1
    for row in index_rows:
        suite = row["suite"]
        if metric_counts[suite] != int(row["metric_count"]):
            raise RunnerError("device_report_invalid", "device metric count does not match suite index")


def run_command(
    command: Sequence[str],
    *,
    cwd: Path,
    environment: Mapping[str, str],
    stdout_path: Path,
    stderr_path: Path,
) -> int:
    try:
        completed = subprocess.run(
            list(command),
            cwd=cwd,
            env=dict(environment),
            capture_output=True,
            check=False,
        )
    except OSError as exc:
        write_bytes(stdout_path, b"")
        write_bytes(stderr_path, b"")
        raise RunnerError("command_unavailable", "required hardware command could not start") from exc
    write_bytes(stdout_path, completed.stdout)
    write_bytes(stderr_path, completed.stderr)
    return completed.returncode


def test_hooks_enabled() -> bool:
    return os.environ.get("V1SIMPLE_HIL_TEST_HOOKS") == "1"


def authoritative_child_environment(pio_executable: Path) -> dict[str, str]:
    allowed = {
        "HOME",
        "LANG",
        "LC_ALL",
        "LOGNAME",
        "NO_PROXY",
        "REQUESTS_CA_BUNDLE",
        "SSL_CERT_FILE",
        "SYSTEMROOT",
        "TMPDIR",
        "TZ",
        "USER",
    }
    environment = {
        key: value for key, value in os.environ.items() if key in allowed
    }
    environment["PATH"] = os.pathsep.join(
        (str(pio_executable.parent), "/usr/sbin", "/usr/bin", "/sbin", "/bin")
    )
    return environment


def authoritative_platformio() -> Path:
    suffix = Path("Scripts") / "platformio.exe" if os.name == "nt" else Path("bin") / "pio"
    expected_environment = (Path.home() / ".platformio" / "penv").resolve()
    expected_pio = (expected_environment / suffix).resolve()
    resolved = shutil.which("pio")
    if (
        resolved is None
        or Path(resolved).resolve() != expected_pio
        or not expected_pio.is_file()
        or Path(sys.prefix).resolve() != expected_environment
    ):
        raise RunnerError(
            "untrusted_platformio",
            "authoritative runs require the default PlatformIO isolated environment",
        )
    environment = authoritative_child_environment(expected_pio)
    try:
        version_result = subprocess.run(
            [str(expected_pio), "--version"],
            env=environment,
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RunnerError("untrusted_platformio", "PlatformIO identity check failed") from exc
    match = re.fullmatch(
        r"PlatformIO Core, version (\d+)\.(\d+)\.(\d+)\s*",
        version_result.stdout,
    )
    if (
        version_result.returncode != 0
        or match is None
        or tuple(int(value) for value in match.groups()) < (6, 1, 19)
    ):
        raise RunnerError("untrusted_platformio", "PlatformIO identity check failed")
    return expected_pio


def validate_runtime_arguments(args: argparse.Namespace) -> Path:
    if test_hooks_enabled():
        return Path(args.pio_command)
    expected_paths = {
        "repo_root": ROOT,
        "template": resolve_hil_board.DEFAULT_TEMPLATE,
        "inventory": resolve_hil_board.DEFAULT_LOCAL_INVENTORY,
        "device_runner": ROOT / "scripts" / "run_device_tests.sh",
    }
    for field, expected in expected_paths.items():
        if getattr(args, field).resolve() != expected.resolve():
            raise RunnerError("untrusted_override", "authoritative runs forbid tool path overrides")
    if args.pio_command != "pio":
        raise RunnerError("untrusted_override", "authoritative runs require the pinned PlatformIO command")
    if args.ports_json is not None:
        raise RunnerError("untrusted_override", "authoritative runs require live port enumeration")
    if args.out_dir is not None:
        artifact_root = (ROOT / ".artifacts").resolve()
        output = args.out_dir.resolve()
        if output == artifact_root or artifact_root not in output.parents:
            raise RunnerError("unsafe_output", "authoritative output must be below ignored .artifacts")
    if not AUTHORITATIVE_GIT.is_file():
        raise RunnerError("git_state_unavailable", "authoritative Git executable is unavailable")
    return authoritative_platformio()


def run_device_suite(args: argparse.Namespace) -> int:
    pio_executable = validate_runtime_arguments(args)
    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "final device suite requires a clean worktree")

    resolution, board_binding, attestation = resolve_device_board(
        alias=args.board,
        template=args.template,
        inventory_path=args.inventory,
        ports_json=args.ports_json,
        pio_command=str(pio_executable),
    )
    endpoints = resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("board_resolution_failed", "resolved board has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("board_resolution_failed", "resolved serial endpoint is not present")

    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("final-device-suite-%Y%m%dT%H%M%SZ")
        run_root = (ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / run_id).resolve()
    else:
        run_root = args.out_dir.resolve()
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "output directory must be new or empty")
    run_root.mkdir(parents=True, exist_ok=True)
    raw_root = run_root / "raw"
    raw_root.mkdir(parents=True, exist_ok=True)
    write_json(run_root / "resolver-attestation.json", attestation)
    write_json(raw_root / "board-resolution-binding.json", board_binding)

    device_root = raw_root / "device-suite"
    device_stdout = raw_root / "device-suite.stdout.log"
    device_stderr = raw_root / "device-suite.stderr.log"
    restore_stdout = raw_root / "production-restore.stdout.log"
    restore_stderr = raw_root / "production-restore.stderr.log"
    environment = (
        os.environ.copy()
        if test_hooks_enabled()
        else authoritative_child_environment(pio_executable)
    )
    environment.update(
        {
            "DEVICE_PORT": serial_port,
            "DEVICE_BOARD_ID": args.board,
            "DEVICE_GIT_SHA": git_state.head_sha,
            "DEVICE_FAIL_CLOSED_TRANSPORT": "1",
            "PIO_CMD": str(pio_executable),
        }
    )

    started_at = utc_now()
    device_exit = 1
    restore_exit = 1
    validation_error: RunnerError | None = None
    restore_error: RunnerError | None = None
    device_artifact_hashes: dict[str, str] = {}
    try:
        try:
            device_exit = run_command(
                [str(args.device_runner), "--full", "--out-dir", str(device_root)],
                cwd=repository,
                environment=environment,
                stdout_path=device_stdout,
                stderr_path=device_stderr,
            )
            device_artifact_hashes = hash_device_artifacts(device_root)
            require_unchanged_git_state(repository, git_state)
            if device_exit != 0:
                validation_error = RunnerError(
                    "device_suite_failed", "device suite command did not complete successfully"
                )
            else:
                manifest_path = device_root / "manifest.json"
                validate_device_manifest(
                    manifest_path,
                    target_sha=git_state.head_sha,
                    board_alias=args.board,
                )
                require_complete_device_artifacts(device_artifact_hashes)
                manifest_payload = read_json(manifest_path, "device manifest")
                assert isinstance(manifest_payload, dict)
                validate_underlying_device_artifacts(
                    device_root,
                    target_sha=git_state.head_sha,
                    manifest_result=str(manifest_payload["result"]),
                    manifest=manifest_payload,
                )
        except RunnerError as exc:
            validation_error = exc
    finally:
        try:
            require_unchanged_git_state(repository, git_state)
            restore_exit = run_command(
                [
                    str(pio_executable),
                    "run",
                    "-e",
                    PRODUCTION_ENVIRONMENT,
                    "-t",
                    "upload",
                    "--upload-port",
                    serial_port,
                ],
                cwd=repository,
                environment=environment,
                stdout_path=restore_stdout,
                stderr_path=restore_stderr,
            )
            require_unchanged_git_state(repository, git_state)
        except RunnerError as exc:
            restore_error = exc
            if not restore_stdout.exists():
                write_bytes(restore_stdout, b"")
            if not restore_stderr.exists():
                write_bytes(restore_stderr, b"")

    finished_at = utc_now()
    restored = restore_error is None and restore_exit == 0
    passed = validation_error is None and device_exit == 0 and restored
    authoritative = not test_hooks_enabled()
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-final-device-suite",
        "target_sha": git_state.head_sha,
        "board_alias": args.board,
        "capabilities": ["device-tests", "serial"],
        "started_at_utc": started_at,
        "finished_at_utc": finished_at,
        "device_exit_code": device_exit,
        "production_restore_exit_code": restore_exit,
        "production_restored": restored,
        "authoritative": authoritative,
        "result": ("PASS" if authoritative else "TEST_PASS") if passed else "FAIL",
        "artifact_sha256": {
            "device_manifest": sha256_file(device_root / "manifest.json")
            if (device_root / "manifest.json").is_file()
            else None,
            "device_stdout": sha256_file(device_stdout),
            "device_stderr": sha256_file(device_stderr),
            "production_restore_stdout": sha256_file(restore_stdout),
            "production_restore_stderr": sha256_file(restore_stderr),
            "resolver_attestation": sha256_file(run_root / "resolver-attestation.json"),
            "board_resolution_binding": sha256_file(
                raw_root / "board-resolution-binding.json"
            ),
            "runner": sha256_file(Path(__file__)),
            "device_runner": sha256_optional_file(args.device_runner),
            "resolver": sha256_file(Path(resolve_hil_board.__file__)),
            "platformio": sha256_optional_file(pio_executable),
            "python": sha256_optional_file(Path(sys.executable)),
            "git": sha256_optional_file(AUTHORITATIVE_GIT),
        },
        "device_artifact_sha256": device_artifact_hashes,
    }
    if validation_error is not None:
        result["failure_code"] = validation_error.code
    elif restore_error is not None:
        result["failure_code"] = restore_error.code
    elif not restored:
        result["failure_code"] = "production_restore_failed"
    write_json(run_root / "result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if passed else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--run-device-suite", action="store_true")
    mode.add_argument("--case", choices=CASE_IDS)
    parser.add_argument("--board", required=True, help="opaque local board alias")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--production-replay", action="store_true")
    parser.add_argument("--repo-root", type=Path, default=ROOT)
    parser.add_argument("--template", type=Path, default=resolve_hil_board.DEFAULT_TEMPLATE)
    parser.add_argument("--inventory", type=Path, default=resolve_hil_board.DEFAULT_LOCAL_INVENTORY)
    parser.add_argument("--ports-json", type=Path)
    parser.add_argument("--pio-command", default="pio")
    parser.add_argument("--device-runner", type=Path, default=ROOT / "scripts" / "run_device_tests.sh")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.runs < 1 or args.runs > 10:
        print(json.dumps({"error": {"code": "invalid_runs", "message": "runs must be 1..10"}}))
        return 2
    if args.case is not None:
        error = {
            "error": {
                "code": "case_driver_unavailable",
                "message": "typed physical orchestration for this case is not implemented",
            }
        }
        print(json.dumps(error, sort_keys=True))
        return 3
    try:
        return run_device_suite(args)
    except RunnerError as exc:
        print(json.dumps({"error": {"code": exc.code, "message": exc.message}}, sort_keys=True))
        return 1
    except Exception:
        print(
            json.dumps(
                {
                    "error": {
                        "code": "internal_error",
                        "message": "runner failed closed before producing a verified result",
                    }
                },
                sort_keys=True,
            )
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
