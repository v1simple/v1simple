#!/usr/bin/env python3
"""Run fail-closed bug-squash hardware jobs through an explicit board alias.

The final device-suite mode wraps the legacy device runner with exact resolver
selection, a full target SHA, fail-closed transport handling, production-image
restoration, and a sanitized result. Registered but blocked scenario drivers
fail before hardware mutation until their typed physical orchestrators exist.
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
from typing import Callable, Mapping, Sequence
import xml.etree.ElementTree as ET

import bug_squash_hil_case_drivers as case_drivers
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
CASE_IDS = case_drivers.CASE_IDS
BSC02_CASE_ID = "BSC-02"
BSC02_HIL_ENVIRONMENT = "waveshare-349-hil"
BSC02_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC02_REQUIRED_RUNS = 1
BSC02_ADAPTER_TIMEOUT_SECONDS = 600
BSC02_FIRST_RETRY_MIN_MS = 2_500
BSC02_FIRST_RETRY_MAX_MS = 5_000
BSC02_LATER_RETRY_MIN_MS = 25_000
BSC02_LATER_RETRY_MAX_MS = 35_000
BSC02_PRESSURE_CAP_BYTES = 64 * 1024
BSC02_PRESSURE_TASK_OVERHEAD_CAP_BYTES = 8 * 1024
BSC02_AUTO_RELEASE_MAX_MS = 5_000
BSC02_LOW_HEAP_PERSIST_MIN_MS = 1_500
BSC02_FREE_FLOOR_BYTES = 16 * 1024
BSC02_LARGEST_BLOCK_FLOOR_BYTES = 8 * 1024
BSC02_SAFETY_FREE_BYTES = (14 * 1024) + 512
BSC02_SAFETY_LARGEST_BLOCK_BYTES = (6 * 1024) + 512
BSC02_ABSOLUTE_MINIMUM_FREE_BYTES = 14 * 1024
BSC02_ABSOLUTE_MINIMUM_LARGEST_BLOCK_BYTES = 6 * 1024
BSC02_DUT_CAPABILITIES = (
    "firmware-execution",
    "maintenance-mode",
    "serial",
)
BSC02_RIG_CAPABILITIES = (
    "artifact-capture",
    "lan-client",
    "sram-pressure-control",
    "utc-time-source",
)
BSC02_CAPTURE_COMMITMENTS = (
    "build_evidence_sha256",
    "heap_timeline_sha256",
    "http_timeline_sha256",
    "lifecycle_timeline_sha256",
    "serial_log_sha256",
)
BSC03_CASE_ID = "BSC-03"
BSC03_REQUIRED_RUNS = 3
BSC03_ADMISSION_DEADLINE_MS = 10_000
BSC03_CUT_NOT_BEFORE_MS = 10_000
BSC03_LOOP_SLO_US = 250_000
BSC03_ADAPTER_TIMEOUT_SECONDS = 600
BSC03_DUT_CAPABILITIES = ("firmware-execution", "persistence", "serial")
BSC03_RIG_CAPABILITIES = (
    "artifact-capture",
    "bond-peer",
    "obd-peer",
    "power-control",
    "sd-media",
    "utc-time-source",
    "v1-peer",
    "vbus-isolation",
)
BSC03_EVENT_IDS = (
    "mutate-four-persistence-classes",
    "wait-for-persistence-admission",
    "persistence-admitted",
    "isolated-ignition-cut",
    "ignition-restore",
)
BSC03_STATE_CLASSES = ("settings", "bond", "obd", "v1-device")
BSC03_FACTS = (
    "settings-state-survived",
    "bond-state-survived",
    "obd-state-survived",
    "v1-device-state-survived",
    "peers-reconnected-without-pairing",
    "loop-slo-preserved",
    "early-cut-durability-not-claimed",
)
BSC11_CASE_ID = "BSC-11"
BSC11_PRODUCTION_ENVIRONMENT = "esp32-s3-car-install"
BSC11_REQUIRED_RUNS = 1
BSC11_MINIMUM_OBSERVATION_MS = 60_000
BSC11_MINIMUM_LONG_PRESS_MS = 5_000
BSC11_SERVICE_MAX_GAP_MS = 5_000
BSC11_POWER_DOWN_MAX_DELAY_MS = 30_000
BSC11_ADAPTER_TIMEOUT_SECONDS = 7_200
BSC11_DUT_CAPABILITIES = (
    "car-mode",
    "firmware-execution",
    "serial",
    "v1-connectivity",
)
BSC11_RIG_CAPABILITIES = (
    "artifact-capture",
    "ignition-control",
    "power-button",
    "utc-time-source",
    "vbus-isolation",
)
BSC11_EVENT_IDS = (
    "car-ignition-established",
    "real-v1-received",
    "real-v1-disconnected",
    "auto-power-window-exceeded",
    "long-pwr-hold-completed",
    "ignition-removed",
    "ignition-power-down",
)
BSC11_FORBIDDEN_ACTIVITY = (
    "auto-power-timer-fired",
    "shutdown-preparation-entered",
    "goodbye-frame-presented",
    "clean-shutdown-marker-written",
    "power-latch-action",
    "deep-sleep-entered",
)
BSC11_CONTINUOUS_SERVICES = ("alp", "ble", "display", "logging", "wifi")
BSC11_CAPTURE_COMMITMENTS = (
    "display_video_sha256",
    "ignition_timeline_sha256",
    "serial_log_sha256",
    "service_timeline_sha256",
    "v1_exchange_sha256",
)
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
UTC_TIMESTAMP_PATTERN = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$"
)


class RunnerError(Exception):
    """Expected fail-closed runner error safe to print without local values."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


class CaseDriverUnavailable(RunnerError):
    """The case is registered but its typed physical driver is not implemented."""


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


def write_json_atomic(path: Path, payload: Mapping[str, object]) -> None:
    temporary = path.with_name(f".{path.name}.{secrets.token_hex(8)}.tmp")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        temporary.replace(path)
    except OSError as exc:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise RunnerError("artifact_write_failed", "runner checkpoint could not be written") from exc


def require_no_symlink_components(path: Path, *, boundary: Path) -> None:
    absolute = Path(os.path.abspath(path))
    absolute_boundary = Path(os.path.abspath(boundary))
    try:
        relative = absolute.relative_to(absolute_boundary)
    except ValueError as exc:
        raise RunnerError("unsafe_output", "runner output is outside its allowed boundary") from exc
    current = absolute_boundary
    for component in relative.parts:
        current /= component
        if current.is_symlink():
            raise RunnerError("unsafe_output", "runner artifacts must not use symlink paths")


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


def require_exact_object(
    value: object,
    expected_keys: set[str],
    *,
    code: str,
    label: str,
) -> dict[str, object]:
    if not isinstance(value, dict) or set(value) != expected_keys:
        raise RunnerError(code, f"{label} does not match the typed case contract")
    return value


def require_sha256(value: object, *, code: str, label: str) -> str:
    if not isinstance(value, str) or SHA256_PATTERN.fullmatch(value) is None:
        raise RunnerError(code, f"{label} is not a valid SHA-256 commitment")
    if value == "0" * 64:
        raise RunnerError(code, f"{label} must not use the all-zero commitment")
    return value


def parse_runner_utc(value: object, *, code: str, label: str) -> datetime:
    if not isinstance(value, str) or UTC_TIMESTAMP_PATTERN.fullmatch(value) is None:
        raise RunnerError(code, f"{label} is not an RFC3339 UTC timestamp")
    try:
        parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise RunnerError(code, f"{label} is not a valid UTC timestamp") from exc
    if parsed.tzinfo is None or parsed.utcoffset() != timezone.utc.utcoffset(parsed):
        raise RunnerError(code, f"{label} is not UTC")
    return parsed


def bsc03_board_attestation(
    *,
    inventory: resolve_hil_board.Inventory,
    alias: str,
    required_capabilities: Sequence[str],
    port_records: Sequence[Mapping[str, object]],
) -> tuple[dict[str, object], dict[str, object]]:
    try:
        resolution = resolve_hil_board.resolve_board(
            inventory,
            alias,
            required_capabilities,
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
    except (resolve_hil_board.ResolverError, KeyError, ValueError) as exc:
        message = getattr(exc, "message", "case board resolution failed")
        raise RunnerError("case_board_resolution_failed", str(message)) from exc
    return resolution, attestation


def resolve_bsc03_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-03 requires the ignored local hardware inventory",
        )
    try:
        inventory = resolve_hil_board.load_inventory(args.template, inventory_path)
        port_records = (
            resolve_hil_board.parse_port_records(
                resolve_hil_board._read_json(args.ports_json, "serial port inventory")
            )
            if args.ports_json is not None
            else resolve_hil_board.enumerate_serial_ports(str(pio_executable))
        )
    except resolve_hil_board.ResolverError as exc:
        raise RunnerError("case_board_resolution_failed", exc.message) from exc

    dut_resolution, dut_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.board,
        required_capabilities=BSC03_DUT_CAPABILITIES,
        port_records=port_records,
    )
    rig_resolution, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=BSC03_RIG_CAPABILITIES,
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-03 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def validate_bsc03_adapter_record(
    payload: object,
    *,
    expected: Mapping[str, object],
    command_started: datetime | None = None,
    command_completed: datetime | None = None,
) -> dict[str, object]:
    code = "case_record_invalid"
    record = require_exact_object(
        payload,
        {
            "schema_version",
            "case_id",
            "session_id",
            "attempt_id",
            "run_index",
            "target_sha",
            "dut_alias",
            "rig_alias",
            "execution_mode",
            "hardware_observed",
            "started_at_utc",
            "completed_at_utc",
            "preconditions",
            "events",
            "admissions",
            "state_commitments",
            "mutation_commitment_sha256",
            "hard_cut_commitment_sha256",
            "boot_commitments",
            "resets",
            "performance",
            "facts",
        },
        code=code,
        label="case adapter record",
    )
    for field in (
        "case_id",
        "session_id",
        "attempt_id",
        "run_index",
        "target_sha",
        "dut_alias",
        "rig_alias",
        "execution_mode",
        "hardware_observed",
    ):
        if record.get(field) != expected.get(field):
            raise RunnerError(code, f"case adapter {field} is not runner-bound")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1:
        raise RunnerError(code, "case adapter schema version is invalid")
    for field in (
        "case_id",
        "session_id",
        "attempt_id",
        "target_sha",
        "dut_alias",
        "rig_alias",
        "execution_mode",
    ):
        if not isinstance(record.get(field), str):
            raise RunnerError(code, f"case adapter {field} type is invalid")
    if type(record.get("run_index")) is not int or type(record.get("hardware_observed")) is not bool:
        raise RunnerError(code, "case adapter run identity types are invalid")

    preconditions = require_exact_object(
        record.get("preconditions"),
        {
            "vbus_isolated",
            "power_rig_qualified",
            "power_rig_evidence_sha256",
            "sd_media_present",
            "v1_peer_ready",
            "obd_peer_ready",
            "bond_peer_ready",
            "production_firmware_target_sha",
        },
        code=code,
        label="case preconditions",
    )
    for field in (
        "vbus_isolated",
        "power_rig_qualified",
        "sd_media_present",
        "v1_peer_ready",
        "obd_peer_ready",
        "bond_peer_ready",
    ):
        if preconditions.get(field) is not True:
            raise RunnerError(code, f"case precondition {field} was not verified")
    require_sha256(
        preconditions.get("power_rig_evidence_sha256"),
        code=code,
        label="power-rig evidence",
    )
    if preconditions.get("production_firmware_target_sha") != expected["target_sha"]:
        raise RunnerError(code, "production firmware does not match the target commit")

    started = parse_runner_utc(
        record.get("started_at_utc"), code=code, label="case start"
    )
    completed = parse_runner_utc(
        record.get("completed_at_utc"), code=code, label="case completion"
    )
    if completed < started:
        raise RunnerError(code, "case completion precedes case start")
    now = datetime.now(timezone.utc)
    if completed > now.replace(microsecond=now.microsecond) and (
        completed - now
    ).total_seconds() > 2:
        raise RunnerError(code, "case completion is in the future")
    if command_started is not None and started < command_started.replace(
        microsecond=0
    ):
        raise RunnerError(code, "physical case start predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(
        microsecond=999999
    ):
        raise RunnerError(code, "physical case completion follows adapter execution")

    events = record.get("events")
    if not isinstance(events, list) or len(events) != len(BSC03_EVENT_IDS):
        raise RunnerError(code, "case events are incomplete")
    elapsed_values: list[int] = []
    for sequence, (event, event_id) in enumerate(
        zip(events, BSC03_EVENT_IDS, strict=True), start=1
    ):
        row = require_exact_object(
            event,
            {"id", "sequence", "elapsed_ms"},
            code=code,
            label="case event",
        )
        elapsed = row.get("elapsed_ms")
        if (
            row.get("id") != event_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or isinstance(elapsed, bool)
            or not isinstance(elapsed, int)
            or elapsed < 0
        ):
            raise RunnerError(code, "case event order or timing is invalid")
        elapsed_values.append(elapsed)
    if any(later <= earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, "case event times must increase strictly")
    cut_after_mutation_ms = elapsed_values[3] - elapsed_values[0]
    if cut_after_mutation_ms < BSC03_CUT_NOT_BEFORE_MS:
        raise RunnerError(code, "ignition was cut before the ten-second durability boundary")
    if elapsed_values[3] <= elapsed_values[2]:
        raise RunnerError(code, "ignition was cut before persistence admission")
    duration_ms = int((completed - started).total_seconds() * 1000)
    if elapsed_values[-1] > duration_ms + 1000:
        raise RunnerError(code, "case events exceed the recorded run duration")

    commitments = require_exact_object(
        record.get("state_commitments"),
        set(BSC03_STATE_CLASSES),
        code=code,
        label="state commitments",
    )
    for state_class in BSC03_STATE_CLASSES:
        pair = require_exact_object(
            commitments[state_class],
            {"before_sha256", "after_sha256"},
            code=code,
            label="state commitment pair",
        )
        before = require_sha256(
            pair.get("before_sha256"), code=code, label="pre-cut state"
        )
        after = require_sha256(
            pair.get("after_sha256"), code=code, label="post-cut state"
        )
        if before != after:
            raise RunnerError(code, f"{state_class} state did not survive the hard cut")

    admissions = require_exact_object(
        record.get("admissions"),
        set(BSC03_STATE_CLASSES),
        code=code,
        label="persistence admissions",
    )
    admission_elapsed_values: list[int] = []
    for state_class in BSC03_STATE_CLASSES:
        admission = require_exact_object(
            admissions[state_class],
            {"admitted_elapsed_ms", "state_commitment_sha256"},
            code=code,
            label="persistence admission",
        )
        admitted_elapsed = admission.get("admitted_elapsed_ms")
        if (
            isinstance(admitted_elapsed, bool)
            or not isinstance(admitted_elapsed, int)
            or admitted_elapsed <= elapsed_values[0]
            or admitted_elapsed - elapsed_values[0] > BSC03_ADMISSION_DEADLINE_MS
        ):
            raise RunnerError(
                code,
                f"{state_class} persistence admission exceeded the ten-second window",
            )
        commitment_pair = commitments[state_class]
        assert isinstance(commitment_pair, dict)
        if admission.get("state_commitment_sha256") != commitment_pair.get("before_sha256"):
            raise RunnerError(
                code,
                f"{state_class} admission is not bound to its mutated state",
            )
        admission_elapsed_values.append(admitted_elapsed)
    aggregate_admission_elapsed = max(admission_elapsed_values)
    if elapsed_values[2] != aggregate_admission_elapsed:
        raise RunnerError(
            code,
            "aggregate persistence admission must equal the last per-class admission",
        )
    admission_ms = aggregate_admission_elapsed - elapsed_values[0]
    require_sha256(
        record.get("mutation_commitment_sha256"),
        code=code,
        label="mutation identity",
    )
    require_sha256(
        record.get("hard_cut_commitment_sha256"),
        code=code,
        label="hard-cut identity",
    )

    boot = require_exact_object(
        record.get("boot_commitments"),
        {"before_sha256", "after_sha256"},
        code=code,
        label="boot commitments",
    )
    boot_before = require_sha256(
        boot.get("before_sha256"), code=code, label="pre-cut boot identity"
    )
    boot_after = require_sha256(
        boot.get("after_sha256"), code=code, label="post-cut boot identity"
    )
    if boot_before == boot_after:
        raise RunnerError(code, "hard cut did not produce a distinct boot identity")

    resets = require_exact_object(
        record.get("resets"),
        {"expected_kind", "planned", "observed", "unexpected"},
        code=code,
        label="reset record",
    )
    if (
        resets.get("expected_kind") != "ignition-hard-cut"
        or any(type(resets.get(field)) is not int for field in ("planned", "observed", "unexpected"))
        or resets.get("planned") != 1
        or resets.get("observed") != 1
        or resets.get("unexpected") != 0
    ):
        raise RunnerError(code, "case reset evidence does not match one clean hard cut")

    performance = require_exact_object(
        record.get("performance"),
        {"loop_max_us", "sample_count"},
        code=code,
        label="performance evidence",
    )
    loop_max = performance.get("loop_max_us")
    sample_count = performance.get("sample_count")
    if (
        isinstance(loop_max, bool)
        or not isinstance(loop_max, int)
        or loop_max < 0
        or loop_max > BSC03_LOOP_SLO_US
        or isinstance(sample_count, bool)
        or not isinstance(sample_count, int)
        or sample_count < 1
    ):
        raise RunnerError(code, "loop-latency evidence does not satisfy the pinned SLO")

    facts = require_exact_object(
        record.get("facts"),
        {"persistence-admission-ms", *BSC03_FACTS},
        code=code,
        label="case facts",
    )
    if (
        type(facts.get("persistence-admission-ms")) is not int
        or facts.get("persistence-admission-ms") != admission_ms
    ):
        raise RunnerError(code, "persistence admission fact does not match event timing")
    if any(facts.get(fact) is not True for fact in BSC03_FACTS):
        raise RunnerError(code, "case acceptance facts are incomplete")
    return record


def load_bsc03_checkpoint(
    state_path: Path,
    *,
    expected_identity: Mapping[str, object],
) -> dict[str, object]:
    payload = read_json(state_path, "BSC-03 checkpoint")
    state = require_exact_object(
        payload,
        {
            "schema_version",
            "case_id",
            "target_sha",
            "session_id",
            "dut_alias",
            "rig_alias",
            "execution_mode",
            "runs_required",
            "started_at_utc",
            "runner_sha256",
            "adapter_sha256",
            "inventory_sha256",
            "dut_attestation",
            "rig_attestation",
            "completed_attempts",
            "active_attempt",
            "abandoned_attempt_ids",
        },
        code="resume_state_invalid",
        label="BSC-03 checkpoint",
    )
    if (
        type(state.get("schema_version")) is not int
        or state.get("schema_version") != 1
        or state.get("case_id") != BSC03_CASE_ID
    ):
        raise RunnerError("resume_state_invalid", "BSC-03 checkpoint identity is invalid")
    for field, expected in expected_identity.items():
        if state.get(field) != expected:
            raise RunnerError(
                "resume_state_mismatch",
                "BSC-03 checkpoint does not match the requested target and hardware",
            )
    completed = state.get("completed_attempts")
    abandoned = state.get("abandoned_attempt_ids")
    if not isinstance(completed, list) or not isinstance(abandoned, list):
        raise RunnerError("resume_state_invalid", "BSC-03 checkpoint attempt lists are invalid")
    if any(
        not isinstance(value, str) or re.fullmatch(r"attempt-[0-9a-f]{32}", value) is None
        for value in abandoned
    ) or len(abandoned) != len(set(abandoned)):
        raise RunnerError("resume_state_invalid", "BSC-03 abandoned attempt list is invalid")
    return state


def validate_bsc03_completed_attempts(
    state: Mapping[str, object],
    *,
    run_root: Path,
    expected_base: Mapping[str, object],
) -> list[dict[str, object]]:
    rows = state["completed_attempts"]
    assert isinstance(rows, list)
    records: list[dict[str, object]] = []
    seen_attempts: set[str] = set()
    for run_index, raw in enumerate(rows, start=1):
        row = require_exact_object(
            raw,
            {"run_index", "attempt_id", "artifact", "sha256"},
            code="resume_state_invalid",
            label="completed attempt",
        )
        attempt_id = row.get("attempt_id")
        artifact = row.get("artifact")
        if (
            type(row.get("run_index")) is not int
            or row.get("run_index") != run_index
            or not isinstance(attempt_id, str)
            or re.fullmatch(r"attempt-[0-9a-f]{32}", attempt_id) is None
            or attempt_id in seen_attempts
            or artifact != f"attempts/{attempt_id}.json"
        ):
            raise RunnerError("resume_state_invalid", "completed attempt identity is invalid")
        seen_attempts.add(attempt_id)
        artifact_path = run_root / artifact
        expected_hash = require_sha256(
            row.get("sha256"), code="resume_state_invalid", label="attempt artifact"
        )
        if (
            not artifact_path.is_file()
            or artifact_path.is_symlink()
            or sha256_file(artifact_path) != expected_hash
        ):
            raise RunnerError("resume_evidence_changed", "completed attempt evidence changed")
        record = read_json(artifact_path, "completed BSC-03 attempt")
        expected = dict(expected_base)
        expected.update({"run_index": run_index, "attempt_id": attempt_id})
        records.append(validate_bsc03_adapter_record(record, expected=expected))
    if len(records) > BSC03_REQUIRED_RUNS:
        raise RunnerError("resume_state_invalid", "checkpoint contains too many completed runs")
    abandoned = state["abandoned_attempt_ids"]
    assert isinstance(abandoned, list)
    if seen_attempts.intersection(abandoned):
        raise RunnerError(
            "resume_state_invalid",
            "completed and abandoned BSC-03 attempts must be disjoint",
        )
    return records


def run_bsc03_adapter(
    *,
    adapter: Path,
    repository: Path,
    serial_port: str,
    expected: Mapping[str, object],
    environment: Mapping[str, str],
) -> dict[str, object]:
    command = [
        str(adapter),
        "--case",
        BSC03_CASE_ID,
        "--session-id",
        str(expected["session_id"]),
        "--attempt-id",
        str(expected["attempt_id"]),
        "--run-index",
        str(expected["run_index"]),
        "--target-sha",
        str(expected["target_sha"]),
        "--dut-alias",
        str(expected["dut_alias"]),
        "--rig-alias",
        str(expected["rig_alias"]),
        "--serial-port",
        serial_port,
    ]
    command_started = datetime.now(timezone.utc)
    try:
        completed = subprocess.run(
            command,
            cwd=repository,
            env=dict(environment),
            capture_output=True,
            check=False,
            timeout=BSC03_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-03 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-03 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-03 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 64 * 1024:
        raise RunnerError("case_record_invalid", "BSC-03 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"),
            object_pairs_hook=reject_duplicate_json_keys,
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-03 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc03_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def bsc03_result(
    *,
    state: Mapping[str, object],
    run_root: Path,
    records: Sequence[Mapping[str, object]],
) -> dict[str, object]:
    run_rows = state["completed_attempts"]
    assert isinstance(run_rows, list)
    return {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc03-connected-persistence-hard-cut",
        "case_id": BSC03_CASE_ID,
        "target_sha": state["target_sha"],
        "session_id": state["session_id"],
        "dut_alias": state["dut_alias"],
        "rig_alias": state["rig_alias"],
        "execution_mode": state["execution_mode"],
        "hardware_observed": state["execution_mode"] == "physical",
        "authoritative": False,
        "physical_collection_completed": state["execution_mode"] == "physical",
        "non_qualifying": True,
        "qualification_status": "BLOCKED",
        "qualification_blockers": list(
            case_drivers.get_case_driver(BSC03_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "COLLECTION_COMPLETE"
        if state["execution_mode"] == "physical"
        else "TEST_PASS",
        "runs_required": BSC03_REQUIRED_RUNS,
        "runs_completed": len(records),
        "early_cut_durability_claimed": False,
        "started_at_utc": state["started_at_utc"],
        "completed_at_utc": utc_now(),
        "dut_attestation": state["dut_attestation"],
        "rig_attestation": state["rig_attestation"],
        "run_artifacts": run_rows,
        "artifact_sha256": {
            "runner": state["runner_sha256"],
            "adapter": state["adapter_sha256"],
            "checkpoint": sha256_file(run_root / "checkpoint.json"),
        },
    }


def validate_bsc03_distinct_runs(records: Sequence[Mapping[str, object]]) -> None:
    if len(records) != BSC03_REQUIRED_RUNS:
        raise RunnerError("case_runs_incomplete", "BSC-03 requires exactly three completed runs")
    for field in ("attempt_id", "mutation_commitment_sha256", "hard_cut_commitment_sha256"):
        values = [record.get(field) for record in records]
        if len(set(values)) != BSC03_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", "BSC-03 run identities must be distinct")
    commitments_by_class: dict[str, set[object]] = {
        state_class: set() for state_class in BSC03_STATE_CLASSES
    }
    for record in records:
        state_commitments = record["state_commitments"]
        assert isinstance(state_commitments, dict)
        for state_class in BSC03_STATE_CLASSES:
            pair = state_commitments[state_class]
            assert isinstance(pair, dict)
            commitments_by_class[state_class].add(pair["before_sha256"])
    if any(len(values) != BSC03_REQUIRED_RUNS for values in commitments_by_class.values()):
        raise RunnerError(
            "case_runs_reused",
            "each BSC-03 run must use distinct mutated persistence state",
        )


def run_bsc03_case(args: argparse.Namespace) -> int:
    if args.runs != BSC03_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-03 requires exactly three hard-cut runs")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-03 requires an opaque local rig alias")
    if args.production_replay:
        raise RunnerError("unsupported_mode", "BSC-03 has no production-replay role")
    if not (
        args.ack_vbus_isolated
        and args.ack_destructive_hard_cuts
        and args.ack_early_cut_not_qualified
    ):
        raise RunnerError(
            "operator_preconditions_incomplete",
            "BSC-03 requires all destructive-test and durability-boundary acknowledgements",
        )

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-03 requires a clean target worktree")

    if test_hooks_enabled():
        pio_executable = Path(args.pio_command)
    else:
        expected_paths = {
            "repo_root": ROOT,
            "template": resolve_hil_board.DEFAULT_TEMPLATE,
            "inventory": resolve_hil_board.DEFAULT_LOCAL_INVENTORY,
        }
        for field, expected in expected_paths.items():
            if getattr(args, field).resolve() != expected.resolve():
                raise RunnerError("untrusted_override", "authoritative BSC-03 paths are pinned")
        if args.ports_json is not None or args.pio_command != "pio":
            raise RunnerError("untrusted_override", "authoritative BSC-03 discovery is live and pinned")
        if not args.inventory.resolve().is_file():
            raise RunnerError(
                "local_inventory_missing",
                "BSC-03 requires the ignored local hardware inventory",
            )
        pio_executable = authoritative_platformio()

    dut_resolution, dut_attestation, rig_attestation = resolve_bsc03_hardware(
        args, pio_executable
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-03 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-03 serial endpoint is not present")

    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError(
                "untrusted_override",
                "authoritative BSC-03 forbids an untracked rig adapter",
            )
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-03 physical execution remains blocked until a tracked rig protocol exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-03 test execution requires a mocked adapter")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-03 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)

    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc03-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / run_id
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    output_boundary = (
        Path(os.path.abspath(args.repo_root)).parent
        if test_hooks_enabled()
        else ROOT / ".artifacts"
    )
    require_no_symlink_components(run_root, boundary=output_boundary)
    state_path = run_root / "checkpoint.json"
    execution_mode = "simulated" if test_hooks_enabled() else "physical"
    expected_identity: dict[str, object] = {
        "target_sha": git_state.head_sha,
        "dut_alias": args.board,
        "rig_alias": args.rig,
        "execution_mode": execution_mode,
        "runs_required": BSC03_REQUIRED_RUNS,
        "runner_sha256": sha256_file(Path(__file__)),
        "adapter_sha256": adapter_sha,
        "inventory_sha256": sha256_file(args.inventory.resolve()),
    }

    if args.resume:
        if not state_path.is_file() or state_path.is_symlink():
            raise RunnerError("resume_state_missing", "BSC-03 resume checkpoint is missing")
        state = load_bsc03_checkpoint(state_path, expected_identity=expected_identity)
        for field, current in (
            ("dut_attestation", dut_attestation),
            ("rig_attestation", rig_attestation),
        ):
            saved = state.get(field)
            if (
                not isinstance(saved, dict)
                or saved.get("alias") != current.get("alias")
                or saved.get("capabilities") != current.get("capabilities")
                or saved.get("resolution_sha256") != current.get("resolution_sha256")
            ):
                raise RunnerError(
                    "resume_state_mismatch",
                    "BSC-03 resolved hardware changed since the checkpoint",
                )
    else:
        if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
            raise RunnerError("output_not_empty", "BSC-03 output must be new unless --resume is used")
        run_root.mkdir(parents=True, exist_ok=True)
        state = {
            "schema_version": 1,
            "case_id": BSC03_CASE_ID,
            **expected_identity,
            "dut_attestation": dut_attestation,
            "rig_attestation": rig_attestation,
            "session_id": f"bsc03-{secrets.token_hex(16)}",
            "started_at_utc": utc_now(),
            "completed_attempts": [],
            "active_attempt": None,
            "abandoned_attempt_ids": [],
        }
        write_json_atomic(state_path, state)

    active = state.get("active_attempt")
    if active is not None:
        active_row = require_exact_object(
            active,
            {"run_index", "attempt_id", "started_at_utc"},
            code="resume_state_invalid",
            label="active attempt",
        )
        if not args.ack_incomplete_run_recovered:
            raise RunnerError(
                "incomplete_run_recovery_required",
                "an interrupted hard-cut attempt requires explicit rig recovery before retry",
            )
        abandoned = state["abandoned_attempt_ids"]
        completed_attempts = state["completed_attempts"]
        assert isinstance(abandoned, list)
        assert isinstance(completed_attempts, list)
        attempt_id = active_row.get("attempt_id")
        completed_ids = {
            row.get("attempt_id")
            for row in completed_attempts
            if isinstance(row, dict)
        }
        if (
            type(active_row.get("run_index")) is not int
            or active_row.get("run_index") != len(completed_attempts) + 1
            or len(completed_attempts) >= BSC03_REQUIRED_RUNS
            or not isinstance(attempt_id, str)
            or re.fullmatch(r"attempt-[0-9a-f]{32}", attempt_id) is None
            or attempt_id in abandoned
            or attempt_id in completed_ids
        ):
            raise RunnerError("resume_state_invalid", "active attempt identity is invalid")
        parse_runner_utc(
            active_row.get("started_at_utc"),
            code="resume_state_invalid",
            label="active attempt start",
        )
        abandoned.append(attempt_id)
        state["active_attempt"] = None
        write_json_atomic(state_path, state)
    elif args.ack_incomplete_run_recovered:
        raise RunnerError(
            "unexpected_recovery_ack",
            "incomplete-run recovery was acknowledged without an interrupted attempt",
        )

    expected_base: dict[str, object] = {
        "case_id": BSC03_CASE_ID,
        "session_id": state["session_id"],
        "target_sha": git_state.head_sha,
        "dut_alias": args.board,
        "rig_alias": args.rig,
        "execution_mode": execution_mode,
        "hardware_observed": execution_mode == "physical",
    }
    records = validate_bsc03_completed_attempts(
        state,
        run_root=run_root,
        expected_base=expected_base,
    )
    environment = os.environ.copy()

    for run_index in range(len(records) + 1, BSC03_REQUIRED_RUNS + 1):
        require_unchanged_git_state(repository, git_state)
        attempt_id = f"attempt-{secrets.token_hex(16)}"
        state["active_attempt"] = {
            "run_index": run_index,
            "attempt_id": attempt_id,
            "started_at_utc": utc_now(),
        }
        write_json_atomic(state_path, state)
        expected = dict(expected_base)
        expected.update({"run_index": run_index, "attempt_id": attempt_id})
        record = run_bsc03_adapter(
            adapter=adapter,
            repository=repository,
            serial_port=serial_port,
            expected=expected,
            environment=environment,
        )
        require_unchanged_git_state(repository, git_state)
        artifact_relative = f"attempts/{attempt_id}.json"
        artifact_path = run_root / artifact_relative
        write_json_atomic(artifact_path, record)
        completed_attempts = state["completed_attempts"]
        assert isinstance(completed_attempts, list)
        completed_attempts.append(
            {
                "run_index": run_index,
                "attempt_id": attempt_id,
                "artifact": artifact_relative,
                "sha256": sha256_file(artifact_path),
            }
        )
        state["active_attempt"] = None
        write_json_atomic(state_path, state)
        records.append(record)

    validate_bsc03_distinct_runs(records)
    require_unchanged_git_state(repository, git_state)
    result = bsc03_result(state=state, run_root=run_root, records=records)
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def canonical_case_commitment(domain: str, payload: object) -> str:
    canonical = json.dumps(
        payload,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return hashlib.sha256(domain.encode("ascii") + b"\0" + canonical).hexdigest()


def resolve_bsc02_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-02 requires the ignored local hardware inventory",
        )
    try:
        inventory = resolve_hil_board.load_inventory(args.template, inventory_path)
        port_records = (
            resolve_hil_board.parse_port_records(
                resolve_hil_board._read_json(args.ports_json, "serial port inventory")
            )
            if args.ports_json is not None
            else resolve_hil_board.enumerate_serial_ports(str(pio_executable))
        )
    except resolve_hil_board.ResolverError as exc:
        raise RunnerError("case_board_resolution_failed", exc.message) from exc

    dut_resolution, dut_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.board,
        required_capabilities=BSC02_DUT_CAPABILITIES,
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=BSC02_RIG_CAPABILITIES,
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-02 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def bsc02_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc02.case-record.v1", committed)


def validate_bsc02_events(
    value: object,
    expected_ids: Sequence[str],
    *,
    label: str,
    expected_phase: int,
) -> list[int]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, f"{label} event sequence is incomplete")
    elapsed_values: list[int] = []
    lifecycle_identity: tuple[int, int, int, int] | None = None
    for sequence, (event, event_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            event,
            {
                "id",
                "sequence",
                "elapsed_ms",
                "reason",
                "arm_sequence",
                "ready_sequence",
                "generation",
                "phase",
            },
            code=code,
            label=f"{label} event",
        )
        elapsed = row.get("elapsed_ms")
        identity_values = tuple(
            row.get(field)
            for field in ("arm_sequence", "ready_sequence", "generation", "phase")
        )
        if (
            row.get("id") != event_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or isinstance(elapsed, bool)
            or not isinstance(elapsed, int)
            or elapsed < 0
            or not isinstance(row.get("reason"), str)
            or not row["reason"]
            or any(type(identity) is not int or identity <= 0 for identity in identity_values[:3])
            or type(identity_values[3]) is not int
            or identity_values[3] != expected_phase
        ):
            raise RunnerError(code, f"{label} event order or timing is invalid")
        typed_identity = (
            identity_values[0],
            identity_values[1],
            identity_values[2],
            identity_values[3],
        )
        if lifecycle_identity is None:
            lifecycle_identity = typed_identity
        elif typed_identity != lifecycle_identity:
            raise RunnerError(code, f"{label} event identity changed during its lifecycle")
        elapsed_values.append(elapsed)
    if any(later <= earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, f"{label} event times must increase strictly")
    return elapsed_values


def require_bsc02_timing(
    later: object,
    earlier: object,
    *,
    minimum_ms: int,
    maximum_ms: int,
    label: str,
) -> tuple[int, int]:
    if any(isinstance(value, bool) or not isinstance(value, int) for value in (later, earlier)):
        raise RunnerError("case_record_invalid", f"{label} timing is invalid")
    delta = later - earlier
    if delta < minimum_ms or delta > maximum_ms:
        raise RunnerError("case_record_invalid", f"{label} timing is outside its bound")
    return later, earlier


def validate_bsc02_adapter_record(
    payload: object,
    *,
    expected: Mapping[str, object],
    command_started: datetime | None = None,
    command_completed: datetime | None = None,
) -> dict[str, object]:
    code = "case_record_invalid"
    record = require_exact_object(
        payload,
        {
            "schema_version",
            "case_id",
            "role",
            "session_id",
            "attempt_id",
            "target_sha",
            "dut_alias",
            "rig_alias",
            "execution_mode",
            "hardware_observed",
            "started_at_utc",
            "completed_at_utc",
            "firmware",
            "preconditions",
            "fault_collection",
            "production_replay",
            "capture_commitments",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-02 adapter record",
    )
    for field in (
        "case_id",
        "role",
        "session_id",
        "attempt_id",
        "target_sha",
        "dut_alias",
        "rig_alias",
        "execution_mode",
        "hardware_observed",
    ):
        if record.get(field) != expected.get(field):
            raise RunnerError(code, f"BSC-02 adapter {field} is not runner-bound")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1:
        raise RunnerError(code, "BSC-02 adapter schema version is invalid")
    if not isinstance(record.get("session_id"), str) or re.fullmatch(
        r"bsc02-[0-9a-f]{32}", record["session_id"]
    ) is None:
        raise RunnerError(code, "BSC-02 session identity is invalid")
    if not isinstance(record.get("attempt_id"), str) or re.fullmatch(
        r"attempt-[0-9a-f]{32}", record["attempt_id"]
    ) is None:
        raise RunnerError(code, "BSC-02 attempt identity is invalid")

    role = record["role"]
    if role not in ("fault-collection", "production-replay"):
        raise RunnerError(code, "BSC-02 adapter role is invalid")
    firmware = require_exact_object(
        record.get("firmware"),
        {
            "environment",
            "target_sha",
            "binary_sha256",
            "build_manifest_sha256",
            "hil_fault_control_active",
        },
        code=code,
        label="BSC-02 firmware identity",
    )
    expected_environment = (
        BSC02_HIL_ENVIRONMENT
        if role == "fault-collection"
        else BSC02_PRODUCTION_ENVIRONMENT
    )
    expected_hil = role == "fault-collection"
    if (
        firmware.get("environment") != expected_environment
        or firmware.get("target_sha") != expected["target_sha"]
        or firmware.get("hil_fault_control_active") is not expected_hil
    ):
        raise RunnerError(code, "BSC-02 firmware identity or HIL marker is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-02 binary")
    require_sha256(
        firmware.get("build_manifest_sha256"),
        code=code,
        label="BSC-02 build manifest",
    )

    preconditions = require_exact_object(
        record.get("preconditions"),
        {"maintenance_mode", "http_probe_ready", "unexpected_resets_before_start"},
        code=code,
        label="BSC-02 preconditions",
    )
    if (
        preconditions.get("maintenance_mode") is not True
        or preconditions.get("http_probe_ready") is not True
        or type(preconditions.get("unexpected_resets_before_start")) is not int
        or preconditions.get("unexpected_resets_before_start") != 0
    ):
        raise RunnerError(code, "BSC-02 maintenance preconditions were not verified")

    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-02 start")
    completed = parse_runner_utc(
        record.get("completed_at_utc"), code=code, label="BSC-02 completion"
    )
    if completed < started:
        raise RunnerError(code, "BSC-02 completion precedes its start")
    now = datetime.now(timezone.utc)
    if completed > now and (completed - now).total_seconds() > 2:
        raise RunnerError(code, "BSC-02 completion is in the future")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "physical BSC-02 evidence predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(
        microsecond=999999
    ):
        raise RunnerError(code, "physical BSC-02 evidence follows adapter execution")

    if role == "fault-collection":
        if record.get("production_replay") is not None:
            raise RunnerError(code, "BSC-02 fault collection mixed production replay evidence")
        collection = require_exact_object(
            record.get("fault_collection"),
            {"ap_start", "pressure", "recovery", "heap"},
            code=code,
            label="BSC-02 fault collection",
        )
        ap_start = require_exact_object(
            collection.get("ap_start"),
            {
                "fault_id",
                "setup_ap_configured",
                "softap_called",
                "false_ap_active",
                "false_ap_reachable",
                "events",
            },
            code=code,
            label="BSC-02 AP start fault",
        )
        if (
            ap_start.get("fault_id") != "wifi-ap-start-fail-once"
            or ap_start.get("setup_ap_configured") is not True
            or ap_start.get("softap_called") is not False
            or ap_start.get("false_ap_active") is not False
            or ap_start.get("false_ap_reachable") is not False
        ):
            raise RunnerError(code, "BSC-02 failed AP admission published a false active state")
        ap_events = validate_bsc02_events(
            ap_start.get("events"),
            ("ready", "fired", "terminal"),
            label="BSC-02 AP start fault",
            expected_phase=1,
        )
        if [event["reason"] for event in ap_start["events"]] != [
            "fresh_ap_admission",
            "softap_admission_suppressed",
            "released_after_suppression",
        ]:
            raise RunnerError(code, "BSC-02 AP fault lifecycle reason is invalid")

        pressure = require_exact_object(
            collection.get("pressure"),
            {
                "fault_id",
                "allocation_cap_bytes",
                "allocated_bytes",
                "task_overhead_bytes",
                "auto_release_ms",
                "heap_guard_stop_observed",
                "events",
            },
            code=code,
            label="BSC-02 SRAM pressure fault",
        )
        allocation_cap = pressure.get("allocation_cap_bytes")
        allocated = pressure.get("allocated_bytes")
        task_overhead = pressure.get("task_overhead_bytes")
        auto_release = pressure.get("auto_release_ms")
        if (
            pressure.get("fault_id") != "wifi-internal-sram-hold"
            or type(allocation_cap) is not int
            or allocation_cap != BSC02_PRESSURE_CAP_BYTES
            or type(allocated) is not int
            or allocated <= 0
            or allocated > allocation_cap
            or type(task_overhead) is not int
            or task_overhead <= 0
            or task_overhead > BSC02_PRESSURE_TASK_OVERHEAD_CAP_BYTES
            or type(auto_release) is not int
            or auto_release <= 0
            or auto_release > BSC02_AUTO_RELEASE_MAX_MS
            or pressure.get("heap_guard_stop_observed") is not True
        ):
            raise RunnerError(code, "BSC-02 SRAM pressure was not bounded or product-observed")
        pressure_events = validate_bsc02_events(
            pressure.get("events"),
            ("ready", "fired", "competing_observed", "terminal"),
            label="BSC-02 SRAM pressure fault",
            expected_phase=2,
        )
        if [event["reason"] for event in pressure["events"]] != [
            "pressure_task_admission",
            "pressure_task_start",
            "wifi_heap_guard_stop",
            "released",
        ]:
            raise RunnerError(code, "BSC-02 SRAM fault lifecycle reason is invalid")

        recovery = require_exact_object(
            collection.get("recovery"),
            {
                "initial_failure_elapsed_ms",
                "first_retry_elapsed_ms",
                "first_http_success_elapsed_ms",
                "pressure_low_heap_started_elapsed_ms",
                "pressure_stop_elapsed_ms",
                "pressure_first_retry_elapsed_ms",
                "pressure_first_retry_outcome",
                "pressure_later_retry_elapsed_ms",
                "pressure_http_success_elapsed_ms",
                "maintenance_mode_continuous",
                "unexpected_resets",
            },
            code=code,
            label="BSC-02 recovery timeline",
        )
        require_bsc02_timing(
            recovery.get("first_retry_elapsed_ms"),
            recovery.get("initial_failure_elapsed_ms"),
            minimum_ms=BSC02_FIRST_RETRY_MIN_MS,
            maximum_ms=BSC02_FIRST_RETRY_MAX_MS,
            label="BSC-02 initial recovery retry",
        )
        require_bsc02_timing(
            recovery.get("first_http_success_elapsed_ms"),
            recovery.get("first_retry_elapsed_ms"),
            minimum_ms=0,
            maximum_ms=5_000,
            label="BSC-02 initial HTTP recovery",
        )
        require_bsc02_timing(
            recovery.get("pressure_stop_elapsed_ms"),
            recovery.get("pressure_low_heap_started_elapsed_ms"),
            minimum_ms=BSC02_LOW_HEAP_PERSIST_MIN_MS,
            maximum_ms=BSC02_AUTO_RELEASE_MAX_MS,
            label="BSC-02 pressure heap-guard persistence",
        )
        require_bsc02_timing(
            recovery.get("pressure_first_retry_elapsed_ms"),
            recovery.get("pressure_stop_elapsed_ms"),
            minimum_ms=BSC02_FIRST_RETRY_MIN_MS,
            maximum_ms=BSC02_FIRST_RETRY_MAX_MS,
            label="BSC-02 pressure first retry",
        )
        require_bsc02_timing(
            recovery.get("pressure_later_retry_elapsed_ms"),
            recovery.get("pressure_first_retry_elapsed_ms"),
            minimum_ms=BSC02_LATER_RETRY_MIN_MS,
            maximum_ms=BSC02_LATER_RETRY_MAX_MS,
            label="BSC-02 pressure cadence retry",
        )
        require_bsc02_timing(
            recovery.get("pressure_http_success_elapsed_ms"),
            recovery.get("pressure_later_retry_elapsed_ms"),
            minimum_ms=0,
            maximum_ms=5_000,
            label="BSC-02 pressure HTTP recovery",
        )
        if (
            recovery.get("pressure_first_retry_outcome") != "cooldown-rejected"
            or recovery.get("maintenance_mode_continuous") is not True
            or type(recovery.get("unexpected_resets")) is not int
            or recovery.get("unexpected_resets") != 0
            or recovery["initial_failure_elapsed_ms"] != ap_events[1]
            or recovery["first_retry_elapsed_ms"] <= ap_events[2]
            or recovery["first_http_success_elapsed_ms"] >= pressure_events[0]
            or recovery["pressure_low_heap_started_elapsed_ms"]
            < pressure_events[1]
            or recovery["pressure_stop_elapsed_ms"] != pressure_events[2]
            or recovery["pressure_first_retry_elapsed_ms"] <= pressure_events[3]
        ):
            raise RunnerError(code, "BSC-02 recovery sequence or continuity is invalid")
        duration_ms = int((completed - started).total_seconds() * 1000)
        if max(
            *ap_events,
            *pressure_events,
            recovery["first_http_success_elapsed_ms"],
            recovery["pressure_http_success_elapsed_ms"],
        ) > duration_ms + 1_000:
            raise RunnerError(code, "BSC-02 evidence exceeds the recorded run duration")

        heap = require_exact_object(
            collection.get("heap"),
            {
                "configured_free_floor_bytes",
                "configured_largest_block_floor_bytes",
                "safety_free_floor_bytes",
                "safety_largest_block_floor_bytes",
                "absolute_minimum_free_bytes",
                "absolute_minimum_largest_block_bytes",
                "minimum_free_bytes",
                "minimum_largest_block_bytes",
                "samples",
            },
            code=code,
            label="BSC-02 heap evidence",
        )
        if (
            heap.get("configured_free_floor_bytes") != BSC02_FREE_FLOOR_BYTES
            or heap.get("configured_largest_block_floor_bytes")
            != BSC02_LARGEST_BLOCK_FLOOR_BYTES
            or heap.get("safety_free_floor_bytes") != BSC02_SAFETY_FREE_BYTES
            or heap.get("safety_largest_block_floor_bytes")
            != BSC02_SAFETY_LARGEST_BLOCK_BYTES
            or heap.get("absolute_minimum_free_bytes")
            != BSC02_ABSOLUTE_MINIMUM_FREE_BYTES
            or heap.get("absolute_minimum_largest_block_bytes")
            != BSC02_ABSOLUTE_MINIMUM_LARGEST_BLOCK_BYTES
        ):
            raise RunnerError(code, "BSC-02 heap floors do not match the product guard")
        samples = heap.get("samples")
        if not isinstance(samples, list) or len(samples) != 3:
            raise RunnerError(code, "BSC-02 heap evidence is incomplete")
        free_values: list[int] = []
        largest_values: list[int] = []
        for sample, phase in zip(samples, ("before", "pressured", "released"), strict=True):
            row = require_exact_object(
                sample,
                {"phase", "free_bytes", "largest_block_bytes"},
                code=code,
                label="BSC-02 heap sample",
            )
            free_bytes = row.get("free_bytes")
            largest_bytes = row.get("largest_block_bytes")
            if (
                row.get("phase") != phase
                or type(free_bytes) is not int
                or type(largest_bytes) is not int
                or free_bytes < BSC02_SAFETY_FREE_BYTES
                or largest_bytes < BSC02_SAFETY_LARGEST_BLOCK_BYTES
            ):
                raise RunnerError(code, "BSC-02 heap floor or phase evidence is invalid")
            free_values.append(free_bytes)
            largest_values.append(largest_bytes)
        if (
            heap.get("minimum_free_bytes") != min(free_values)
            or heap.get("minimum_largest_block_bytes") != min(largest_values)
            or free_values[1] >= free_values[0]
            or largest_values[1] > largest_values[0]
            or not (
                free_values[1] < BSC02_FREE_FLOOR_BYTES
                or largest_values[1] < BSC02_LARGEST_BLOCK_FLOOR_BYTES
            )
            or free_values[2] < free_values[1]
            or largest_values[2] < largest_values[1]
        ):
            raise RunnerError(code, "BSC-02 observed heap evidence is inconsistent")
    else:
        if record.get("fault_collection") is not None:
            raise RunnerError(code, "BSC-02 production replay mixed fault evidence")
        replay = require_exact_object(
            record.get("production_replay"),
            {
                "maintenance_mode_continuous",
                "http_status_recovered",
                "fault_events_seen",
                "unexpected_resets",
            },
            code=code,
            label="BSC-02 production replay",
        )
        if (
            replay.get("maintenance_mode_continuous") is not True
            or replay.get("http_status_recovered") is not True
            or type(replay.get("fault_events_seen")) is not int
            or replay.get("fault_events_seen") != 0
            or type(replay.get("unexpected_resets")) is not int
            or replay.get("unexpected_resets") != 0
        ):
            raise RunnerError(code, "BSC-02 production replay was not fault-free and reachable")

    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC02_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-02 capture commitments",
    )
    commitment_values = [
        require_sha256(commitments[field], code=code, label=field)
        for field in BSC02_CAPTURE_COMMITMENTS
    ]
    if len(set(commitment_values)) != len(commitment_values):
        raise RunnerError(code, "BSC-02 evidence roles reused the same capture")
    binding = require_sha256(
        record.get("evidence_binding_sha256"),
        code=code,
        label="BSC-02 evidence binding",
    )
    if not secrets.compare_digest(binding, bsc02_record_commitment(record)):
        raise RunnerError(code, "BSC-02 evidence binding does not match the record")
    return record


def run_bsc02_adapter(
    *,
    adapter: Path,
    repository: Path,
    serial_port: str,
    expected: Mapping[str, object],
    environment: Mapping[str, str],
) -> dict[str, object]:
    command = [
        str(adapter),
        "--case",
        BSC02_CASE_ID,
        "--role",
        str(expected["role"]),
        "--session-id",
        str(expected["session_id"]),
        "--attempt-id",
        str(expected["attempt_id"]),
        "--target-sha",
        str(expected["target_sha"]),
        "--dut-alias",
        str(expected["dut_alias"]),
        "--rig-alias",
        str(expected["rig_alias"]),
        "--serial-port",
        serial_port,
    ]
    command_started = datetime.now(timezone.utc)
    try:
        completed = subprocess.run(
            command,
            cwd=repository,
            env=dict(environment),
            capture_output=True,
            check=False,
            timeout=BSC02_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-02 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-02 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-02 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 64 * 1024:
        raise RunnerError("case_record_invalid", "BSC-02 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"),
            object_pairs_hook=reject_duplicate_json_keys,
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-02 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc02_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def run_bsc02_case(args: argparse.Namespace) -> int:
    if args.runs != BSC02_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-02 requires exactly one run per collection role")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-02 requires an opaque local rig alias")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-02 collection roles are atomic")
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError(
                "untrusted_override",
                "authoritative BSC-02 forbids an untracked rig adapter",
            )
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-02 physical execution remains blocked until a tracked rig adapter exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-02 test execution requires a mocked adapter")

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-02 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-02 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc02_hardware(
        args, Path(args.pio_command)
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-02 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-02 serial endpoint is not present")

    role = "production-replay" if args.production_replay else "fault-collection"
    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc02-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / f"{run_id}-{role}"
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(
        run_root,
        boundary=Path(os.path.abspath(args.repo_root)).parent,
    )
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-02 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc02-{secrets.token_hex(16)}"
    attempt_id = f"attempt-{secrets.token_hex(16)}"
    expected: dict[str, object] = {
        "case_id": BSC02_CASE_ID,
        "role": role,
        "session_id": session_id,
        "attempt_id": attempt_id,
        "target_sha": git_state.head_sha,
        "dut_alias": args.board,
        "rig_alias": args.rig,
        "execution_mode": "simulated",
        "hardware_observed": False,
    }
    require_unchanged_git_state(repository, git_state)
    record = run_bsc02_adapter(
        adapter=adapter,
        repository=repository,
        serial_port=serial_port,
        expected=expected,
        environment=os.environ.copy(),
    )
    require_unchanged_git_state(repository, git_state)
    attempt_path = run_root / "attempt.json"
    write_json_atomic(attempt_path, record)
    firmware = record["firmware"]
    assert isinstance(firmware, dict)
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc02-maintenance-recovery",
        "case_id": BSC02_CASE_ID,
        "collection_role": role,
        "target_sha": git_state.head_sha,
        "session_sha256": hashlib.sha256(session_id.encode("ascii")).hexdigest(),
        "attempt_sha256": hashlib.sha256(attempt_id.encode("ascii")).hexdigest(),
        "execution_mode": "simulated",
        "hardware_observed": False,
        "authoritative": False,
        "physical_collection_completed": False,
        "non_qualifying": True,
        "qualification_status": "BLOCKED",
        "qualification_blockers": list(
            case_drivers.get_case_driver(BSC02_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC02_REQUIRED_RUNS,
        "runs_completed": 1,
        "production_replay_required": role == "fault-collection",
        "firmware_target": {
            "environment": firmware["environment"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
        },
        "configured_heap_floors": {
            "trigger": {
                "free_bytes": BSC02_FREE_FLOOR_BYTES,
                "largest_block_bytes": BSC02_LARGEST_BLOCK_FLOOR_BYTES,
            },
            "safety": {
                "free_bytes": BSC02_SAFETY_FREE_BYTES,
                "largest_block_bytes": BSC02_SAFETY_LARGEST_BLOCK_BYTES,
            },
            "absolute_minimum": {
                "free_bytes": BSC02_ABSOLUTE_MINIMUM_FREE_BYTES,
                "largest_block_bytes": BSC02_ABSOLUTE_MINIMUM_LARGEST_BLOCK_BYTES,
            },
        },
        "evidence_binding_sha256": record["evidence_binding_sha256"],
        "artifact_sha256": {
            "adapter_record": sha256_file(attempt_path),
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc02.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc02.rig-attestation.v1", rig_attestation
            ),
        },
    }
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def resolve_bsc11_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-11 requires the ignored local hardware inventory",
        )
    try:
        inventory = resolve_hil_board.load_inventory(args.template, inventory_path)
        port_records = (
            resolve_hil_board.parse_port_records(
                resolve_hil_board._read_json(args.ports_json, "serial port inventory")
            )
            if args.ports_json is not None
            else resolve_hil_board.enumerate_serial_ports(str(pio_executable))
        )
    except resolve_hil_board.ResolverError as exc:
        raise RunnerError("case_board_resolution_failed", exc.message) from exc

    dut_resolution, dut_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.board,
        required_capabilities=BSC11_DUT_CAPABILITIES,
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=BSC11_RIG_CAPABILITIES,
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-11 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def bsc11_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc11.case-record.v1", committed)


def validate_bsc11_adapter_record(
    payload: object,
    *,
    expected: Mapping[str, object],
    command_started: datetime | None = None,
    command_completed: datetime | None = None,
) -> dict[str, object]:
    code = "case_record_invalid"
    record = require_exact_object(
        payload,
        {
            "schema_version",
            "case_id",
            "session_id",
            "attempt_id",
            "target_sha",
            "dut_alias",
            "rig_alias",
            "execution_mode",
            "hardware_observed",
            "started_at_utc",
            "completed_at_utc",
            "firmware",
            "preconditions",
            "events",
            "long_press",
            "forbidden_activity",
            "services",
            "power",
            "capture_commitments",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-11 adapter record",
    )
    for field in (
        "case_id",
        "session_id",
        "attempt_id",
        "target_sha",
        "dut_alias",
        "rig_alias",
        "execution_mode",
        "hardware_observed",
    ):
        if record.get(field) != expected.get(field):
            raise RunnerError(code, f"BSC-11 adapter {field} is not runner-bound")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1:
        raise RunnerError(code, "BSC-11 adapter schema version is invalid")
    if not isinstance(record.get("session_id"), str) or re.fullmatch(
        r"bsc11-[0-9a-f]{32}", record["session_id"]
    ) is None:
        raise RunnerError(code, "BSC-11 session identity is invalid")
    if not isinstance(record.get("attempt_id"), str) or re.fullmatch(
        r"attempt-[0-9a-f]{32}", record["attempt_id"]
    ) is None:
        raise RunnerError(code, "BSC-11 attempt identity is invalid")

    firmware = require_exact_object(
        record.get("firmware"),
        {"environment", "target_sha", "binary_sha256", "car_mode_define"},
        code=code,
        label="BSC-11 firmware identity",
    )
    if (
        firmware.get("environment") != BSC11_PRODUCTION_ENVIRONMENT
        or firmware.get("target_sha") != expected["target_sha"]
        or firmware.get("car_mode_define") is not True
    ):
        raise RunnerError(code, "BSC-11 did not execute the exact car production target")
    require_sha256(
        firmware.get("binary_sha256"), code=code, label="car production binary"
    )

    preconditions = require_exact_object(
        record.get("preconditions"),
        {
            "vbus_isolated",
            "ignition_present",
            "real_v1_peer",
            "auto_power_off_minutes",
            "ignition_rig_evidence_sha256",
        },
        code=code,
        label="BSC-11 preconditions",
    )
    if (
        preconditions.get("vbus_isolated") is not True
        or preconditions.get("ignition_present") is not True
        or preconditions.get("real_v1_peer") is not True
    ):
        raise RunnerError(code, "BSC-11 power and V1 preconditions were not verified")
    auto_power_minutes = preconditions.get("auto_power_off_minutes")
    if (
        isinstance(auto_power_minutes, bool)
        or not isinstance(auto_power_minutes, int)
        or auto_power_minutes < 1
        or auto_power_minutes > 60
    ):
        raise RunnerError(code, "BSC-11 auto-power timeout is outside the product range")
    require_sha256(
        preconditions.get("ignition_rig_evidence_sha256"),
        code=code,
        label="ignition rig evidence",
    )

    started = parse_runner_utc(
        record.get("started_at_utc"), code=code, label="BSC-11 start"
    )
    completed = parse_runner_utc(
        record.get("completed_at_utc"), code=code, label="BSC-11 completion"
    )
    if completed < started:
        raise RunnerError(code, "BSC-11 completion precedes its start")
    now = datetime.now(timezone.utc)
    if completed > now and (completed - now).total_seconds() > 2:
        raise RunnerError(code, "BSC-11 completion is in the future")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "physical BSC-11 evidence predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(
        microsecond=999999
    ):
        raise RunnerError(code, "physical BSC-11 evidence follows adapter execution")

    events = record.get("events")
    if not isinstance(events, list) or len(events) != len(BSC11_EVENT_IDS):
        raise RunnerError(code, "BSC-11 event sequence is incomplete")
    elapsed_values: list[int] = []
    for sequence, (event, event_id) in enumerate(
        zip(events, BSC11_EVENT_IDS, strict=True), start=1
    ):
        row = require_exact_object(
            event,
            {"id", "sequence", "elapsed_ms"},
            code=code,
            label="BSC-11 event",
        )
        elapsed = row.get("elapsed_ms")
        if (
            row.get("id") != event_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or isinstance(elapsed, bool)
            or not isinstance(elapsed, int)
            or elapsed < 0
        ):
            raise RunnerError(code, "BSC-11 event order or timing is invalid")
        elapsed_values.append(elapsed)
    if any(later <= earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, "BSC-11 event times must increase strictly")
    duration_ms = int((completed - started).total_seconds() * 1000)
    if elapsed_values[-1] > duration_ms + 1000:
        raise RunnerError(code, "BSC-11 events exceed the recorded run duration")
    wait_elapsed_ms = elapsed_values[3] - elapsed_values[2]
    required_wait_ms = max(
        BSC11_MINIMUM_OBSERVATION_MS,
        auto_power_minutes * 60_000,
    )
    if wait_elapsed_ms <= required_wait_ms:
        raise RunnerError(
            code,
            "BSC-11 did not observe beyond the configured auto-power window",
        )

    long_press = require_exact_object(
        record.get("long_press"),
        {
            "started_elapsed_ms",
            "completed_elapsed_ms",
            "duration_ms",
            "inert",
            "evidence_sha256",
        },
        code=code,
        label="BSC-11 long-press evidence",
    )
    press_start = long_press.get("started_elapsed_ms")
    press_completed = long_press.get("completed_elapsed_ms")
    press_duration = long_press.get("duration_ms")
    if (
        any(
            isinstance(value, bool) or not isinstance(value, int)
            for value in (press_start, press_completed, press_duration)
        )
        or press_start < elapsed_values[3]
        or press_completed != elapsed_values[4]
        or press_duration != press_completed - press_start
        or press_duration < BSC11_MINIMUM_LONG_PRESS_MS
        or long_press.get("inert") is not True
    ):
        raise RunnerError(code, "BSC-11 long PWR hold was not proven inert")
    require_sha256(
        long_press.get("evidence_sha256"), code=code, label="long-press evidence"
    )

    forbidden = require_exact_object(
        record.get("forbidden_activity"),
        set(BSC11_FORBIDDEN_ACTIVITY),
        code=code,
        label="BSC-11 forbidden shutdown activity",
    )
    if any(type(forbidden.get(field)) is not int or forbidden[field] != 0 for field in forbidden):
        raise RunnerError(code, "BSC-11 observed a forbidden portable-shutdown action")

    services = require_exact_object(
        record.get("services"),
        set(BSC11_CONTINUOUS_SERVICES),
        code=code,
        label="BSC-11 service continuity",
    )
    for service in BSC11_CONTINUOUS_SERVICES:
        row = require_exact_object(
            services[service],
            {
                "continuous",
                "sample_count",
                "first_observed_elapsed_ms",
                "during_hold_elapsed_ms",
                "last_observed_elapsed_ms",
                "maximum_gap_ms",
                "evidence_sha256",
            },
            code=code,
            label=f"BSC-11 {service} continuity",
        )
        sample_count = row.get("sample_count")
        first_observed = row.get("first_observed_elapsed_ms")
        during_hold = row.get("during_hold_elapsed_ms")
        last_observed = row.get("last_observed_elapsed_ms")
        maximum_gap = row.get("maximum_gap_ms")
        if (
            row.get("continuous") is not True
            or any(
                isinstance(value, bool) or not isinstance(value, int)
                for value in (
                    sample_count,
                    first_observed,
                    during_hold,
                    last_observed,
                    maximum_gap,
                )
            )
            or first_observed < elapsed_values[0]
            or first_observed > elapsed_values[2]
            or during_hold <= press_start
            or during_hold >= press_completed
            or last_observed <= elapsed_values[4]
            or last_observed >= elapsed_values[5]
            or maximum_gap < 1
            or maximum_gap > BSC11_SERVICE_MAX_GAP_MS
            or sample_count < math.ceil(
                (last_observed - first_observed) / maximum_gap
            ) + 1
        ):
            raise RunnerError(
                code,
                f"BSC-11 {service} evidence does not span the shutdown-isolation window",
            )
        require_sha256(
            row.get("evidence_sha256"), code=code, label=f"{service} continuity evidence"
        )

    power = require_exact_object(
        record.get("power"),
        {
            "ignition_present_through_observation",
            "expected_power_event_kind",
            "observed_power_events",
            "unexpected_resets_before_removal",
            "power_downs_before_removal",
            "ignition_removal_elapsed_ms",
            "power_down_elapsed_ms",
            "power_down_source",
            "vbus_present_at_power_down",
            "evidence_sha256",
        },
        code=code,
        label="BSC-11 power transition",
    )
    if (
        power.get("ignition_present_through_observation") is not True
        or power.get("expected_power_event_kind") != "ignition-removal"
        or type(power.get("observed_power_events")) is not int
        or power.get("observed_power_events") != 1
        or type(power.get("unexpected_resets_before_removal")) is not int
        or power.get("unexpected_resets_before_removal") != 0
        or type(power.get("power_downs_before_removal")) is not int
        or power.get("power_downs_before_removal") != 0
        or power.get("ignition_removal_elapsed_ms") != elapsed_values[5]
        or power.get("power_down_elapsed_ms") != elapsed_values[6]
        or power.get("power_down_source") != "ignition-removal"
        or power.get("vbus_present_at_power_down") is not False
        or elapsed_values[6] - elapsed_values[5] > BSC11_POWER_DOWN_MAX_DELAY_MS
    ):
        raise RunnerError(code, "BSC-11 power-down was not isolated to ignition removal")
    require_sha256(power.get("evidence_sha256"), code=code, label="power transition evidence")

    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC11_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-11 capture commitments",
    )
    commitment_values = [
        require_sha256(commitments[field], code=code, label=field)
        for field in BSC11_CAPTURE_COMMITMENTS
    ]
    if len(set(commitment_values)) != len(commitment_values):
        raise RunnerError(code, "BSC-11 evidence roles reused the same capture")
    binding = require_sha256(
        record.get("evidence_binding_sha256"),
        code=code,
        label="BSC-11 evidence binding",
    )
    if not secrets.compare_digest(binding, bsc11_record_commitment(record)):
        raise RunnerError(code, "BSC-11 evidence binding does not match the record")
    return record


def run_bsc11_adapter(
    *,
    adapter: Path,
    repository: Path,
    serial_port: str,
    expected: Mapping[str, object],
    environment: Mapping[str, str],
) -> dict[str, object]:
    command = [
        str(adapter),
        "--case",
        BSC11_CASE_ID,
        "--session-id",
        str(expected["session_id"]),
        "--attempt-id",
        str(expected["attempt_id"]),
        "--target-sha",
        str(expected["target_sha"]),
        "--dut-alias",
        str(expected["dut_alias"]),
        "--rig-alias",
        str(expected["rig_alias"]),
        "--serial-port",
        serial_port,
    ]
    command_started = datetime.now(timezone.utc)
    try:
        completed = subprocess.run(
            command,
            cwd=repository,
            env=dict(environment),
            capture_output=True,
            check=False,
            timeout=BSC11_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-11 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-11 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-11 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 64 * 1024:
        raise RunnerError("case_record_invalid", "BSC-11 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"),
            object_pairs_hook=reject_duplicate_json_keys,
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-11 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc11_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def run_bsc11_case(args: argparse.Namespace) -> int:
    if args.runs != BSC11_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-11 requires exactly one isolated car-power run")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-11 requires an opaque local rig alias")
    if args.production_replay:
        raise RunnerError("unsupported_mode", "BSC-11 has no production-replay role")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-11 is an atomic one-run collection")
    if not args.ack_vbus_isolated:
        raise RunnerError(
            "operator_preconditions_incomplete",
            "BSC-11 requires explicit VBUS-isolation acknowledgement",
        )
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError(
                "untrusted_override",
                "authoritative BSC-11 forbids an untracked rig adapter",
            )
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-11 physical execution remains blocked until a tracked rig adapter exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-11 test execution requires a mocked adapter")

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-11 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-11 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    pio_executable = Path(args.pio_command)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc11_hardware(
        args, pio_executable
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-11 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-11 serial endpoint is not present")

    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc11-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / run_id
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(
        run_root,
        boundary=Path(os.path.abspath(args.repo_root)).parent,
    )
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-11 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc11-{secrets.token_hex(16)}"
    attempt_id = f"attempt-{secrets.token_hex(16)}"
    expected: dict[str, object] = {
        "case_id": BSC11_CASE_ID,
        "session_id": session_id,
        "attempt_id": attempt_id,
        "target_sha": git_state.head_sha,
        "dut_alias": args.board,
        "rig_alias": args.rig,
        "execution_mode": "simulated",
        "hardware_observed": False,
    }
    require_unchanged_git_state(repository, git_state)
    record = run_bsc11_adapter(
        adapter=adapter,
        repository=repository,
        serial_port=serial_port,
        expected=expected,
        environment=os.environ.copy(),
    )
    require_unchanged_git_state(repository, git_state)
    attempt_path = run_root / "attempt.json"
    write_json_atomic(attempt_path, record)
    firmware = record["firmware"]
    preconditions = record["preconditions"]
    assert isinstance(firmware, dict) and isinstance(preconditions, dict)
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc11-car-shutdown-isolation",
        "case_id": BSC11_CASE_ID,
        "target_sha": git_state.head_sha,
        "session_sha256": hashlib.sha256(session_id.encode("ascii")).hexdigest(),
        "attempt_sha256": hashlib.sha256(attempt_id.encode("ascii")).hexdigest(),
        "execution_mode": "simulated",
        "hardware_observed": False,
        "authoritative": False,
        "physical_collection_completed": False,
        "non_qualifying": True,
        "qualification_status": "BLOCKED",
        "qualification_blockers": list(
            case_drivers.get_case_driver(BSC11_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC11_REQUIRED_RUNS,
        "runs_completed": 1,
        "production_target": {
            "environment": firmware["environment"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
        },
        "configured_auto_power_off_minutes": preconditions["auto_power_off_minutes"],
        "minimum_observation_ms": BSC11_MINIMUM_OBSERVATION_MS,
        "evidence_binding_sha256": record["evidence_binding_sha256"],
        "artifact_sha256": {
            "adapter_record": sha256_file(attempt_path),
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc11.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc11.rig-attestation.v1", rig_attestation
            ),
        },
    }
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--run-device-suite", action="store_true")
    mode.add_argument("--case", choices=CASE_IDS)
    parser.add_argument("--board", required=True, help="opaque local board alias")
    parser.add_argument("--rig", help="opaque local rig alias for typed case execution")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--production-replay", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--ack-vbus-isolated", action="store_true")
    parser.add_argument("--ack-destructive-hard-cuts", action="store_true")
    parser.add_argument("--ack-early-cut-not-qualified", action="store_true")
    parser.add_argument("--ack-incomplete-run-recovered", action="store_true")
    parser.add_argument("--repo-root", type=Path, default=ROOT)
    parser.add_argument("--template", type=Path, default=resolve_hil_board.DEFAULT_TEMPLATE)
    parser.add_argument("--inventory", type=Path, default=resolve_hil_board.DEFAULT_LOCAL_INVENTORY)
    parser.add_argument("--ports-json", type=Path)
    parser.add_argument("--pio-command", default="pio")
    parser.add_argument("--device-runner", type=Path, default=ROOT / "scripts" / "run_device_tests.sh")
    parser.add_argument(
        "--case-adapter",
        type=Path,
        help="mocked typed-case rig boundary; authoritative overrides are forbidden",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
    )
    return parser


def case_handler_map() -> Mapping[str, Callable[[argparse.Namespace], int]]:
    """Return the exact implemented entrypoint map for registry validation."""

    return {
        "run_bsc02_case": run_bsc02_case,
        "run_bsc03_case": run_bsc03_case,
        "run_bsc11_case": run_bsc11_case,
    }


def resolve_case_handler(
    driver: case_drivers.CaseDriver,
) -> Callable[[argparse.Namespace], int]:
    """Resolve a registry entry before any case-owned hardware mutation."""

    if not driver.implemented:
        raise CaseDriverUnavailable(
            "case_driver_unavailable",
            "typed physical orchestration for this case is not implemented",
        )
    handlers = case_handler_map()
    if tuple(handlers) != case_drivers.implemented_entrypoints():
        raise RunnerError(
            "case_driver_contract_invalid",
            "tracked case-driver registry does not match runner entrypoints",
        )
    handler = handlers.get(driver.entrypoint)
    if handler is None:
        raise RunnerError(
            "case_driver_contract_invalid",
            "tracked case-driver entrypoint is unavailable",
        )
    return handler


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.runs < 1 or args.runs > 10:
        print(json.dumps({"error": {"code": "invalid_runs", "message": "runs must be 1..10"}}))
        return 2
    if args.case is not None:
        try:
            driver = case_drivers.get_case_driver(args.case)
            return resolve_case_handler(driver)(args)
        except CaseDriverUnavailable as exc:
            print(
                json.dumps(
                    {"error": {"code": exc.code, "message": exc.message}},
                    sort_keys=True,
                )
            )
            return 3
        except (case_drivers.CaseDriverContractError, RunnerError) as exc:
            code = exc.code if isinstance(exc, RunnerError) else "case_driver_contract_invalid"
            message = (
                exc.message
                if isinstance(exc, RunnerError)
                else "tracked case-driver registry is invalid"
            )
            print(json.dumps({"error": {"code": code, "message": message}}, sort_keys=True))
            return 1
        except Exception:
            print(
                json.dumps(
                    {
                        "error": {
                            "code": "internal_error",
                            "message": "typed case failed closed without qualifying evidence",
                        }
                    },
                    sort_keys=True,
                )
            )
            return 1
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
