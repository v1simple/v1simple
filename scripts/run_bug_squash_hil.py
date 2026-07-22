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
BSC04_CASE_ID = "BSC-04"
BSC04_HIL_ENVIRONMENT = "waveshare-349-hil"
BSC04_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC04_REQUIRED_RUNS = 1
BSC04_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC04_LATE_CONNECTION_MIN_MS = 30_000
BSC04_SETTLE_DEADLINE_MIN_MS = 10_000
BSC04_SETTLE_DEADLINE_MAX_MS = 10_500
BSC04_DUT_CAPABILITIES = (
    "firmware-execution",
    "serial",
    "v1-connectivity",
)
BSC04_RIG_CAPABILITIES = (
    "artifact-capture",
    "obd-peer",
    "power-control",
    "utc-time-source",
    "v1-peer",
)
BSC04_FAULT_EVENT_IDS = (
    "late-v1-power-enabled",
    "late-v1-connected",
    "v1-settling-entered",
    "verify-push-suppressed",
    "hard-deadline-exit",
    "obd-sequencing-started",
    "proxy-sequencing-started",
)
BSC04_PRODUCTION_EVENT_IDS = (
    "late-v1-power-enabled",
    "late-v1-connected",
    "v1-settling-entered",
    "verify-push-accepted",
    "obd-sequencing-started",
    "proxy-sequencing-started",
)
BSC04_FACTS = {
    "late_connection_delay_ms",
    "entry_state",
    "same_loop_reentry",
    "settle_exit_elapsed_ms",
    "verify_push_match_observed",
    "verify_push_suppressed",
    "hard_deadline_used",
    "v1_connected_through_exit",
    "obd_started_without_v1_power_cycle",
    "proxy_started_without_v1_power_cycle",
    "unexpected_v1_disconnects",
    "unexpected_resets",
    "hil_fault_control_active",
}
BSC04_CAPTURE_COMMITMENTS = (
    "build_evidence_sha256",
    "coordinator_timeline_sha256",
    "perf_csv_sha256",
    "serial_log_sha256",
    "v1_exchange_sha256",
)
BSC05_CASE_ID = "BSC-05"
BSC05_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC05_REQUIRED_RUNS = 3
BSC05_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC05_DUT_CAPABILITIES = (
    "display",
    "firmware-execution",
    "serial",
    "v1-connectivity",
)
BSC05_RIG_CAPABILITIES = (
    "artifact-capture",
    "display-capture",
    "programmable-v1-peer",
    "utc-time-source",
)
BSC05_FAULT_STIMULUS_IDS = (
    "fragment-alert",
    "disconnect-mid-packet",
    "release-old-callback",
    "send-display-only-packet",
    "send-fresh-alert",
)
BSC05_PRODUCTION_STIMULUS_IDS = (
    "fragment-alert",
    "disconnect-mid-packet",
    "send-fresh-alert",
)
BSC05_FAULT_FACTS = {
    "old-queue-state-cleared",
    "old-partial-state-cleared",
    "old-persisted-state-cleared",
    "logical-display-idle-after-reconnect",
    "physical-display-idle-after-reconnect",
    "old-callback-rejected",
    "fresh-alert-rendered",
    "fresh-alert-persisted",
    "fresh-alert-faded",
}
BSC05_PRODUCTION_FACTS = {
    "old-generation-state-absent",
    "fresh-alert-normal",
}
BSC05_CAPTURE_COMMITMENTS = (
    "build_evidence_sha256",
    "display_video_sha256",
    "framebuffer_sha256",
    "packet_generation_sha256",
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
BSC10_CASE_ID = "BSC-10"
BSC10_HIL_ENVIRONMENT = "waveshare-349-hil"
BSC10_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC10_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC10_CAPTURE_COMMITMENTS = (
    "browser_trace_sha256",
    "build_evidence_sha256",
    "http_sequence_sha256",
    "nvs_runtime_state_sha256",
    "serial_log_sha256",
)
BSC13_CASE_ID = "BSC-13"
BSC13_HIL_ENVIRONMENT = "waveshare-349-hil"
BSC13_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC13_REQUIRED_RUNS = 3
BSC13_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC13_IDLE_DEADLINE_MS = 1_000
BSC13_DUT_CAPABILITIES = (
    "firmware-execution",
    "obd-connectivity",
    "proxy-connectivity",
    "serial",
)
BSC13_RIG_CAPABILITIES = (
    "artifact-capture",
    "obd-peer",
    "proxy-client",
    "utc-time-source",
)
BSC13_STIMULUS_IDS = (
    "begin-obd-connect",
    "preempt-with-proxy",
    "preempt-with-disable",
    "remove-preemption",
)
BSC13_FAULT_IDS = ("obd-connect-edge-barrier",)
BSC13_BARRIER_IDS = ("physical-link-before-session", "preemption-release")
BSC13_CRITICAL_WINDOW_ROLES = ("proxy-takeover", "qualification-disable")
BSC13_FAULT_FACTS = {
    "unowned-link-disconnected",
    "callback-confirmed-link-down",
    "coordinator-reached-idle",
    "phantom-connected-status-observed",
    "resume-scan-count",
    "successful-reconnect-count",
    "barrier-generation-matched",
}
BSC13_PRODUCTION_FACTS = {
    "orphan-link-observed",
    "phantom-connected-status-observed",
    "single-reconnect-succeeded",
    "hil-fault-control-active",
}
BSC13_CAPTURE_COMMITMENTS = (
    "build_evidence_sha256",
    "coordinator_timeline_sha256",
    "obd_exchange_sha256",
    "proxy_exchange_sha256",
    "serial_log_sha256",
)
BSC14_CASE_ID = "BSC-14"
BSC14_HIL_ENVIRONMENT = "waveshare-349-hil"
BSC14_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC14_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC14_CAPTURE_COMMITMENTS = (
    "build_evidence_sha256",
    "reset_timeline_sha256",
    "sd_backup_sha256",
    "serial_log_sha256",
    "touch_timeline_sha256",
)
BSC16_CASE_ID = "BSC-16"
BSC16_HIL_ENVIRONMENT = "waveshare-349-hil"
BSC16_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC16_REQUIRED_RUNS = 1
BSC16_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC16_DUT_CAPABILITIES = (
    "battery-monitor",
    "firmware-execution",
    "power-button",
    "serial",
)
BSC16_RIG_CAPABILITIES = (
    "artifact-capture",
    "battery-source",
    "logic-analyzer",
    "power-control",
    "usb-source",
    "utc-time-source",
    "vbus-isolation",
)
BSC16_FAULT_STIMULUS_IDS = (
    "pwr-wake-on-battery",
    "usb-cold-boot",
    "force-adc-init-failure",
    "hold-power-button",
    "transition-battery-to-usb",
    "transition-usb-to-battery",
)
BSC16_PRODUCTION_STIMULUS_IDS = (
    "pwr-wake-on-battery",
    "usb-cold-boot",
    "hold-power-button",
    "transition-battery-to-usb",
    "transition-usb-to-battery",
)
BSC16_FAULT_FACTS = {
    "pwr-wake-transient-usb-observed",
    "usb-confirmation-delay-ms",
    "adc-failure-voltage-degraded",
    "adc-failure-power-button-operational",
    "long-hold-classified-as-usb",
    "long-hold-shutdown-succeeded",
    "source-flapping-observed",
    "gpio16-bounce-ms",
}
BSC16_PRODUCTION_FACTS = {
    "battery-classification-correct",
    "usb-classification-correct",
    "power-button-operational",
    "source-flapping-observed",
    "hil-fault-control-active",
}
BSC16_CAPTURE_COMMITMENTS = (
    "build_evidence_sha256",
    "logic_analyzer_sha256",
    "poweroff_log_sha256",
    "serial_log_sha256",
    "source_transitions_sha256",
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


def resolve_bsc04_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-04 requires the ignored local hardware inventory",
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
        required_capabilities=BSC04_DUT_CAPABILITIES,
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=BSC04_RIG_CAPABILITIES,
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-04 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def bsc04_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc04.case-record.v1", committed)


def validate_bsc04_events(value: object, expected_ids: Sequence[str]) -> None:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, "BSC-04 event sequence is incomplete")
    elapsed_values: list[int] = []
    loop_values: list[int] = []
    for sequence, (event, expected_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            event,
            {"id", "sequence", "elapsed_ms", "loop_sequence", "result"},
            code=code,
            label="BSC-04 event",
        )
        elapsed = row.get("elapsed_ms")
        loop_sequence = row.get("loop_sequence")
        if (
            row.get("id") != expected_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed < 0
            or type(loop_sequence) is not int
            or loop_sequence <= 0
            or row.get("result") != "pass"
        ):
            raise RunnerError(code, "BSC-04 event order or result is invalid")
        elapsed_values.append(elapsed)
        loop_values.append(loop_sequence)
    if any(later < earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, "BSC-04 event times moved backwards")
    if any(later < earlier for earlier, later in zip(loop_values, loop_values[1:])):
        raise RunnerError(code, "BSC-04 event loop sequence moved backwards")
    if loop_values[1] != loop_values[2] or elapsed_values[1] != elapsed_values[2]:
        raise RunnerError(code, "BSC-04 late V1 connection did not enter settling in the same loop")


def validate_bsc04_fault_lifecycle(value: object, *, role: str) -> None:
    code = "case_record_invalid"
    if role == "production-replay":
        if value != []:
            raise RunnerError(code, "BSC-04 production replay contains HIL fault events")
        return
    if not isinstance(value, list) or len(value) != 3:
        raise RunnerError(code, "BSC-04 VerifyPush fault lifecycle is incomplete")
    identity: tuple[int, int, int, int] | None = None
    elapsed_values: list[int] = []
    for sequence, (event, event_id, forwarded) in enumerate(
        zip(value, ("ready", "fired", "released"), (True, False, False), strict=True),
        start=1,
    ):
        row = require_exact_object(
            event,
            {
                "id",
                "sequence",
                "elapsed_ms",
                "arm_sequence",
                "ready_sequence",
                "generation",
                "phase",
                "coordinator_state",
                "v1_connected",
                "raw_verify_push_edge",
                "forwarded_verify_push_edge",
            },
            code=code,
            label="BSC-04 VerifyPush fault event",
        )
        elapsed = row.get("elapsed_ms")
        current_identity = tuple(
            row.get(field)
            for field in ("arm_sequence", "ready_sequence", "generation", "phase")
        )
        if (
            row.get("id") != event_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed < 0
            or any(type(item) is not int or item <= 0 for item in current_identity[:3])
            or type(current_identity[3]) is not int
            or current_identity[3] != 1
            or row.get("coordinator_state") != "V1_SETTLING"
            or row.get("v1_connected") is not True
            or row.get("raw_verify_push_edge") is not True
            or row.get("forwarded_verify_push_edge") is not forwarded
        ):
            raise RunnerError(code, "BSC-04 VerifyPush fault lifecycle evidence is invalid")
        typed_identity = (
            current_identity[0],
            current_identity[1],
            current_identity[2],
            current_identity[3],
        )
        if identity is None:
            identity = typed_identity
        elif typed_identity != identity:
            raise RunnerError(code, "BSC-04 VerifyPush fault identity changed during execution")
        elapsed_values.append(elapsed)
    if any(later < earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, "BSC-04 VerifyPush fault times moved backwards")


def validate_bsc04_facts(value: object, *, role: str) -> None:
    code = "case_record_invalid"
    facts = require_exact_object(value, BSC04_FACTS, code=code, label="BSC-04 facts")
    delay = facts.get("late_connection_delay_ms")
    settle_exit = facts.get("settle_exit_elapsed_ms")
    expected_fault = role == "fault-collection"
    if (
        type(delay) is not int
        or delay < BSC04_LATE_CONNECTION_MIN_MS
        or facts.get("entry_state") not in {"STEADY", "WIFI_OPEN"}
        or facts.get("same_loop_reentry") is not True
        or type(settle_exit) is not int
        or settle_exit <= 0
        or facts.get("verify_push_match_observed") is not True
        or facts.get("verify_push_suppressed") is not expected_fault
        or facts.get("hard_deadline_used") is not expected_fault
        or facts.get("v1_connected_through_exit") is not True
        or facts.get("obd_started_without_v1_power_cycle") is not True
        or facts.get("proxy_started_without_v1_power_cycle") is not True
        or type(facts.get("unexpected_v1_disconnects")) is not int
        or facts.get("unexpected_v1_disconnects") != 0
        or type(facts.get("unexpected_resets")) is not int
        or facts.get("unexpected_resets") != 0
        or facts.get("hil_fault_control_active") is not expected_fault
    ):
        raise RunnerError(code, "BSC-04 facts do not satisfy the connection-cycle policy")
    if expected_fault:
        if settle_exit < BSC04_SETTLE_DEADLINE_MIN_MS or settle_exit > BSC04_SETTLE_DEADLINE_MAX_MS:
            raise RunnerError(code, "BSC-04 hard-deadline exit is outside the bounded tolerance")
    elif settle_exit >= BSC04_SETTLE_DEADLINE_MIN_MS:
        raise RunnerError(code, "BSC-04 production replay did not progress through VerifyPush before the deadline")


def validate_bsc04_adapter_record(
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
            "events",
            "facts",
            "fault_lifecycle",
            "capture_commitments",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-04 adapter record",
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
            raise RunnerError(code, f"BSC-04 {field} does not match the runner invocation")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1:
        raise RunnerError(code, "BSC-04 adapter schema is unsupported")
    if type(record.get("hardware_observed")) is not bool:
        raise RunnerError(code, "BSC-04 hardware observation flag is invalid")
    role = record.get("role")
    if role not in {"fault-collection", "production-replay"}:
        raise RunnerError(code, "BSC-04 collection role is invalid")
    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-04 start")
    completed = parse_runner_utc(record.get("completed_at_utc"), code=code, label="BSC-04 completion")
    if completed < started:
        raise RunnerError(code, "BSC-04 completion predates its start")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "BSC-04 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=0):
        raise RunnerError(code, "BSC-04 physical record postdates adapter execution")

    firmware = require_exact_object(
        record.get("firmware"),
        {"environment", "target_sha", "binary_sha256", "hil_fault_control_active"},
        code=code,
        label="BSC-04 firmware",
    )
    expected_environment = BSC04_HIL_ENVIRONMENT if role == "fault-collection" else BSC04_PRODUCTION_ENVIRONMENT
    expected_hil = role == "fault-collection"
    if (
        firmware.get("environment") != expected_environment
        or firmware.get("target_sha") != expected.get("target_sha")
        or firmware.get("hil_fault_control_active") is not expected_hil
    ):
        raise RunnerError(code, "BSC-04 firmware role or target is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-04 firmware binary")

    validate_bsc04_events(
        record.get("events"),
        BSC04_FAULT_EVENT_IDS if role == "fault-collection" else BSC04_PRODUCTION_EVENT_IDS,
    )
    validate_bsc04_facts(record.get("facts"), role=role)
    validate_bsc04_fault_lifecycle(record.get("fault_lifecycle"), role=role)

    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC04_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-04 capture commitments",
    )
    commitment_values = [
        require_sha256(commitments[field], code=code, label=field)
        for field in BSC04_CAPTURE_COMMITMENTS
    ]
    if len(set(commitment_values)) != len(commitment_values):
        raise RunnerError(code, "BSC-04 evidence roles reused the same capture")
    binding = require_sha256(
        record.get("evidence_binding_sha256"), code=code, label="BSC-04 evidence binding"
    )
    if not secrets.compare_digest(binding, bsc04_record_commitment(record)):
        raise RunnerError(code, "BSC-04 evidence binding does not match the record")
    return record


def run_bsc04_adapter(
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
        BSC04_CASE_ID,
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
            timeout=BSC04_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-04 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-04 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-04 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 64 * 1024:
        raise RunnerError("case_record_invalid", "BSC-04 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-04 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc04_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def run_bsc04_case(args: argparse.Namespace) -> int:
    if args.runs != BSC04_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-04 requires exactly one run per collection role")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-04 requires an opaque local rig alias")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-04 collection roles are atomic")
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError("untrusted_override", "authoritative BSC-04 forbids an untracked rig adapter")
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-04 physical execution remains blocked until a tracked rig adapter exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-04 test execution requires a mocked adapter")

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-04 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-04 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc04_hardware(args, Path(args.pio_command))
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-04 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-04 serial endpoint is not present")

    role = "production-replay" if args.production_replay else "fault-collection"
    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc04-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / f"{run_id}-{role}"
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-04 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc04-{secrets.token_hex(16)}"
    attempt_id = f"attempt-{secrets.token_hex(16)}"
    expected: dict[str, object] = {
        "case_id": BSC04_CASE_ID,
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
    record = run_bsc04_adapter(
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
        "run_kind": "bug-squash-bsc04-connection-cycle-progress",
        "case_id": BSC04_CASE_ID,
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
            case_drivers.get_case_driver(BSC04_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC04_REQUIRED_RUNS,
        "runs_completed": 1,
        "production_replay_required": role == "fault-collection",
        "firmware_target": {
            "environment": firmware["environment"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
        },
        "evidence_binding_sha256": record["evidence_binding_sha256"],
        "artifact_sha256": {
            "adapter_record": sha256_file(attempt_path),
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc04.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc04.rig-attestation.v1", rig_attestation
            ),
        },
    }
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def load_bsc05_case_descriptor() -> dict[str, object]:
    code = "case_driver_contract_invalid"
    profile, errors = qualification.load_pinned_profile()
    if profile is None or errors:
        raise RunnerError(code, "BSC-05 pinned qualification profile is invalid")
    descriptor = next(
        (candidate for candidate in profile["required_cases"] if candidate["id"] == BSC05_CASE_ID),
        None,
    )
    row = require_exact_object(
        descriptor,
        {
            "id",
            "minimum_runs",
            "fault_build_required",
            "production_replay_required",
            "required_dut_capabilities",
            "required_rig_capabilities",
            "scenario",
            "production_replay",
        },
        code=code,
        label="BSC-05 case descriptor",
    )
    if (
        row.get("id") != BSC05_CASE_ID
        or type(row.get("minimum_runs")) is not int
        or row.get("minimum_runs") != BSC05_REQUIRED_RUNS
        or row.get("fault_build_required") is not False
        or row.get("production_replay_required") is not True
        or row.get("required_dut_capabilities") != list(BSC05_DUT_CAPABILITIES)
        or row.get("required_rig_capabilities") != list(BSC05_RIG_CAPABILITIES)
    ):
        raise RunnerError(code, "BSC-05 pinned case descriptor does not match the typed driver")
    return row


def bsc05_role_descriptor(
    case_descriptor: Mapping[str, object], *, production_replay: bool
) -> dict[str, object]:
    code = "case_driver_contract_invalid"
    role = case_descriptor.get("production_replay" if production_replay else "scenario")
    descriptor = require_exact_object(
        role,
        {
            "role_id",
            "schema",
            "build_kind",
            "stimulus_ids",
            "fault_ids",
            "barrier_ids",
            "vbus_isolation_required",
            "reset_contract",
            "facts",
        },
        code=code,
        label="BSC-05 role descriptor",
    )
    expected_role_id = (
        "alert-generation-production-replay"
        if production_replay
        else "alert-generation-fence"
    )
    expected_stimuli = (
        BSC05_PRODUCTION_STIMULUS_IDS
        if production_replay
        else BSC05_FAULT_STIMULUS_IDS
    )
    expected_barriers = [] if production_replay else ["old-callback-held"]
    expected_facts = BSC05_PRODUCTION_FACTS if production_replay else BSC05_FAULT_FACTS
    reset = require_exact_object(
        descriptor.get("reset_contract"),
        {"expected_kind", "expected_count", "unexpected_count"},
        code=code,
        label="BSC-05 reset descriptor",
    )
    facts = descriptor.get("facts")
    if not isinstance(facts, list):
        raise RunnerError(code, "BSC-05 fact descriptor is invalid")
    fact_ids = [fact.get("id") for fact in facts if isinstance(fact, dict)]
    if (
        descriptor.get("role_id") != expected_role_id
        or descriptor.get("schema") != "case-observation-v1"
        or descriptor.get("build_kind") != "production"
        or descriptor.get("stimulus_ids") != list(expected_stimuli)
        or descriptor.get("fault_ids") != []
        or descriptor.get("barrier_ids") != expected_barriers
        or descriptor.get("vbus_isolation_required") is not False
        or reset != {"expected_kind": "none", "expected_count": 0, "unexpected_count": 0}
        or len(fact_ids) != len(facts)
        or set(fact_ids) != expected_facts
        or any(
            set(fact) != {"id", "type", "expected"}
            or fact.get("type") != "boolean"
            or fact.get("expected") is not True
            for fact in facts
            if isinstance(fact, dict)
        )
    ):
        raise RunnerError(code, "BSC-05 pinned role descriptor does not match the typed driver")
    return descriptor


def bsc05_descriptor_commitment(case_descriptor: Mapping[str, object]) -> str:
    return canonical_case_commitment(
        "v1simple.bsc05.case-descriptor.v1", case_descriptor
    )


def bsc05_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc05.case-record.v1", committed)


def validate_bsc05_stimuli(
    value: object, expected_ids: Sequence[str]
) -> dict[str, int]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, "BSC-05 stimulus sequence is incomplete")
    elapsed_by_id: dict[str, int] = {}
    prior_elapsed = -1
    for sequence, (stimulus, expected_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            stimulus,
            {"id", "sequence", "elapsed_ms"},
            code=code,
            label="BSC-05 stimulus",
        )
        elapsed = row.get("elapsed_ms")
        if (
            row.get("id") != expected_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed < prior_elapsed
        ):
            raise RunnerError(code, "BSC-05 stimulus order or timing is invalid")
        elapsed_by_id[expected_id] = elapsed
        prior_elapsed = elapsed
    return elapsed_by_id


def validate_bsc05_generation_timeline(
    value: object,
    *,
    production_replay: bool,
    stimuli: Mapping[str, int],
) -> dict[str, object]:
    code = "case_record_invalid"
    common_keys = {
        "old_generation",
        "new_generation",
        "fragment_started_elapsed_ms",
        "disconnect_elapsed_ms",
        "old_session_closed_elapsed_ms",
        "new_session_opened_elapsed_ms",
        "fresh_alert_elapsed_ms",
        "fresh_alert_rendered_elapsed_ms",
        "fresh_alert_persisted_elapsed_ms",
        "fresh_alert_faded_elapsed_ms",
        "unexpected_generation_admissions",
    }
    fault_keys = {
        "old_callback_release_elapsed_ms",
        "old_callback_rejected_elapsed_ms",
        "display_only_packet_elapsed_ms",
        "logical_display_idle_elapsed_ms",
        "physical_display_idle_elapsed_ms",
    }
    timeline = require_exact_object(
        value,
        common_keys if production_replay else common_keys | fault_keys,
        code=code,
        label="BSC-05 generation timeline",
    )
    old_generation = timeline.get("old_generation")
    new_generation = timeline.get("new_generation")
    if (
        type(old_generation) is not int
        or old_generation <= 0
        or type(new_generation) is not int
        or new_generation <= 0
        or new_generation == old_generation
        or type(timeline.get("unexpected_generation_admissions")) is not int
        or timeline.get("unexpected_generation_admissions") != 0
    ):
        raise RunnerError(code, "BSC-05 generation identity is invalid")

    integer_fields = common_keys - {
        "old_generation",
        "new_generation",
        "unexpected_generation_admissions",
    }
    if not production_replay:
        integer_fields |= fault_keys
    if any(type(timeline.get(field)) is not int or timeline[field] < 0 for field in integer_fields):
        raise RunnerError(code, "BSC-05 generation timeline contains an invalid timestamp")

    fragment = timeline["fragment_started_elapsed_ms"]
    disconnect = timeline["disconnect_elapsed_ms"]
    old_closed = timeline["old_session_closed_elapsed_ms"]
    new_opened = timeline["new_session_opened_elapsed_ms"]
    fresh = timeline["fresh_alert_elapsed_ms"]
    rendered = timeline["fresh_alert_rendered_elapsed_ms"]
    persisted = timeline["fresh_alert_persisted_elapsed_ms"]
    faded = timeline["fresh_alert_faded_elapsed_ms"]
    if (
        fragment != stimuli["fragment-alert"]
        or disconnect != stimuli["disconnect-mid-packet"]
        or fresh != stimuli["send-fresh-alert"]
        or not fragment <= disconnect <= old_closed <= new_opened <= fresh
        or rendered < fresh
        or persisted < fresh
        or faded < max(rendered, persisted)
    ):
        raise RunnerError(code, "BSC-05 generation timeline ordering is invalid")
    if not production_replay:
        released = timeline["old_callback_release_elapsed_ms"]
        rejected = timeline["old_callback_rejected_elapsed_ms"]
        display_only = timeline["display_only_packet_elapsed_ms"]
        logical_idle = timeline["logical_display_idle_elapsed_ms"]
        physical_idle = timeline["physical_display_idle_elapsed_ms"]
        if (
            released != stimuli["release-old-callback"]
            or display_only != stimuli["send-display-only-packet"]
            or not new_opened <= released <= rejected <= display_only
            or logical_idle < display_only
            or physical_idle < display_only
            or max(logical_idle, physical_idle) > fresh
        ):
            raise RunnerError(code, "BSC-05 fault timeline ordering is invalid")
    return timeline


def validate_bsc05_barriers(
    value: object,
    *,
    production_replay: bool,
    timeline: Mapping[str, object],
) -> None:
    code = "case_record_invalid"
    if production_replay:
        if value != []:
            raise RunnerError(code, "BSC-05 production replay contains barrier evidence")
        return
    if not isinstance(value, list) or len(value) != 1:
        raise RunnerError(code, "BSC-05 callback barrier evidence is incomplete")
    barrier = require_exact_object(
        value[0],
        {
            "id",
            "sequence",
            "ready_elapsed_ms",
            "released_elapsed_ms",
            "old_generation",
            "new_generation",
            "timed_out",
        },
        code=code,
        label="BSC-05 callback barrier",
    )
    ready = barrier.get("ready_elapsed_ms")
    released = barrier.get("released_elapsed_ms")
    if (
        barrier.get("id") != "old-callback-held"
        or type(barrier.get("sequence")) is not int
        or barrier.get("sequence") != 1
        or type(ready) is not int
        or type(released) is not int
        or ready < timeline["fragment_started_elapsed_ms"]
        or ready > timeline["disconnect_elapsed_ms"]
        or released < timeline["old_callback_release_elapsed_ms"]
        or released > timeline["old_callback_rejected_elapsed_ms"]
        or barrier.get("old_generation") != timeline["old_generation"]
        or barrier.get("new_generation") != timeline["new_generation"]
        or barrier.get("timed_out") is not False
    ):
        raise RunnerError(code, "BSC-05 callback barrier identity or timing is invalid")


def validate_bsc05_facts(
    value: object, contracts: object, *, production_replay: bool
) -> None:
    code = "case_record_invalid"
    if not isinstance(contracts, list):
        raise RunnerError(code, "BSC-05 fact descriptor is invalid")
    expected_ids = BSC05_PRODUCTION_FACTS if production_replay else BSC05_FAULT_FACTS
    contract_by_id = {
        contract.get("id"): contract for contract in contracts if isinstance(contract, dict)
    }
    if set(contract_by_id) != expected_ids or len(contract_by_id) != len(contracts):
        raise RunnerError(code, "BSC-05 fact descriptor is invalid")
    facts = require_exact_object(
        value, expected_ids, code=code, label="BSC-05 facts"
    )
    for fact_id, contract in contract_by_id.items():
        if (
            contract.get("type") != "boolean"
            or contract.get("expected") is not True
            or facts.get(fact_id) is not True
        ):
            raise RunnerError(code, f"BSC-05 fact {fact_id} is invalid")


def validate_bsc05_adapter_record(
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
            "role_id",
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
            "case_descriptor",
            "case_descriptor_sha256",
            "firmware",
            "stimuli",
            "generation_timeline",
            "barriers",
            "facts",
            "capture_commitments",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-05 adapter record",
    )
    for field in (
        "case_id",
        "role_id",
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
            raise RunnerError(code, f"BSC-05 {field} does not match the runner invocation")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1:
        raise RunnerError(code, "BSC-05 adapter schema is unsupported")
    if type(record.get("run_index")) is not int or not 1 <= record["run_index"] <= BSC05_REQUIRED_RUNS:
        raise RunnerError(code, "BSC-05 run index is invalid")
    if type(record.get("hardware_observed")) is not bool:
        raise RunnerError(code, "BSC-05 hardware observation flag is invalid")

    case_descriptor = expected.get("case_descriptor")
    role_descriptor = expected.get("role_descriptor")
    if not isinstance(case_descriptor, dict) or not isinstance(role_descriptor, dict):
        raise RunnerError(code, "BSC-05 pinned descriptor binding is invalid")
    if record.get("case_descriptor") != case_descriptor:
        raise RunnerError(code, "BSC-05 case descriptor does not match the pinned profile")
    descriptor_sha = bsc05_descriptor_commitment(case_descriptor)
    if (
        expected.get("case_descriptor_sha256") != descriptor_sha
        or record.get("case_descriptor_sha256") != descriptor_sha
    ):
        raise RunnerError(code, "BSC-05 case descriptor digest is invalid")

    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-05 start")
    completed = parse_runner_utc(record.get("completed_at_utc"), code=code, label="BSC-05 completion")
    if completed < started:
        raise RunnerError(code, "BSC-05 completion predates its start")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "BSC-05 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=0):
        raise RunnerError(code, "BSC-05 physical record postdates adapter execution")

    firmware = require_exact_object(
        record.get("firmware"),
        {
            "environment",
            "target_sha",
            "binary_sha256",
            "hil_fault_control_active",
            "build_kind",
        },
        code=code,
        label="BSC-05 firmware",
    )
    if (
        firmware.get("environment") != BSC05_PRODUCTION_ENVIRONMENT
        or firmware.get("target_sha") != expected.get("target_sha")
        or firmware.get("hil_fault_control_active") is not False
        or firmware.get("build_kind") != role_descriptor.get("build_kind")
    ):
        raise RunnerError(code, "BSC-05 firmware role or target is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-05 firmware binary")

    production_replay = role_descriptor.get("role_id") == "alert-generation-production-replay"
    stimuli = validate_bsc05_stimuli(record.get("stimuli"), role_descriptor["stimulus_ids"])
    timeline = validate_bsc05_generation_timeline(
        record.get("generation_timeline"),
        production_replay=production_replay,
        stimuli=stimuli,
    )
    validate_bsc05_barriers(
        record.get("barriers"),
        production_replay=production_replay,
        timeline=timeline,
    )
    validate_bsc05_facts(
        record.get("facts"), role_descriptor["facts"], production_replay=production_replay
    )
    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC05_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-05 capture commitments",
    )
    commitment_values = [
        require_sha256(commitments[field], code=code, label=field)
        for field in BSC05_CAPTURE_COMMITMENTS
    ]
    if len(set(commitment_values)) != len(commitment_values):
        raise RunnerError(code, "BSC-05 evidence roles reused the same capture")
    binding = require_sha256(
        record.get("evidence_binding_sha256"), code=code, label="BSC-05 evidence binding"
    )
    if not secrets.compare_digest(binding, bsc05_record_commitment(record)):
        raise RunnerError(code, "BSC-05 evidence binding does not match the record")
    return record


def validate_bsc05_distinct_runs(records: Sequence[Mapping[str, object]]) -> None:
    if len(records) != BSC05_REQUIRED_RUNS:
        raise RunnerError("case_runs_incomplete", "BSC-05 requires exactly three completed runs")
    for field in ("attempt_id", "evidence_binding_sha256"):
        if len({record.get(field) for record in records}) != BSC05_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", "BSC-05 run identities must be distinct")
    first_firmware = records[0].get("firmware")
    if any(record.get("firmware") != first_firmware for record in records[1:]):
        raise RunnerError("case_runs_mixed", "BSC-05 runs must use one bound firmware artifact")
    for field in BSC05_CAPTURE_COMMITMENTS:
        values: list[object] = []
        for record in records:
            commitments = record.get("capture_commitments")
            assert isinstance(commitments, dict)
            values.append(commitments[field])
        if len(set(values)) != BSC05_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", "BSC-05 run captures must be distinct")


def resolve_bsc05_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError("local_inventory_missing", "BSC-05 requires the ignored local hardware inventory")
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
        required_capabilities=BSC05_DUT_CAPABILITIES,
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=BSC05_RIG_CAPABILITIES,
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-05 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def run_bsc05_adapter(
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
        BSC05_CASE_ID,
        "--role-id",
        str(expected["role_id"]),
        "--case-descriptor-sha256",
        str(expected["case_descriptor_sha256"]),
        "--case-descriptor-json",
        json.dumps(
            expected["case_descriptor"],
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ),
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
            timeout=BSC05_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-05 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-05 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-05 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 64 * 1024:
        raise RunnerError("case_record_invalid", "BSC-05 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-05 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc05_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def run_bsc05_case(args: argparse.Namespace) -> int:
    case_descriptor = load_bsc05_case_descriptor()
    role_descriptor = bsc05_role_descriptor(
        case_descriptor, production_replay=args.production_replay
    )
    if args.runs != BSC05_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-05 requires exactly three runs per collection role")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-05 requires an opaque local rig alias")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-05 collection roles are atomic")
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError("untrusted_override", "authoritative BSC-05 forbids an untracked rig adapter")
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-05 physical execution remains blocked until a tracked rig adapter exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-05 test execution requires a mocked adapter")

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-05 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-05 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc05_hardware(
        args, Path(args.pio_command)
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-05 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-05 serial endpoint is not present")

    collection_role = "production-replay" if args.production_replay else "fault-collection"
    role_id = role_descriptor["role_id"]
    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc05-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / f"{run_id}-{collection_role}"
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(
        run_root, boundary=Path(os.path.abspath(args.repo_root)).parent
    )
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-05 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc05-{secrets.token_hex(16)}"
    descriptor_sha = bsc05_descriptor_commitment(case_descriptor)
    records: list[dict[str, object]] = []
    run_artifacts: list[dict[str, object]] = []
    for run_index in range(1, BSC05_REQUIRED_RUNS + 1):
        attempt_id = f"attempt-{secrets.token_hex(16)}"
        expected: dict[str, object] = {
            "case_id": BSC05_CASE_ID,
            "role_id": role_id,
            "session_id": session_id,
            "attempt_id": attempt_id,
            "run_index": run_index,
            "target_sha": git_state.head_sha,
            "dut_alias": args.board,
            "rig_alias": args.rig,
            "execution_mode": "simulated",
            "hardware_observed": False,
            "case_descriptor": case_descriptor,
            "case_descriptor_sha256": descriptor_sha,
            "role_descriptor": role_descriptor,
        }
        require_unchanged_git_state(repository, git_state)
        record = run_bsc05_adapter(
            adapter=adapter,
            repository=repository,
            serial_port=serial_port,
            expected=expected,
            environment=os.environ.copy(),
        )
        require_unchanged_git_state(repository, git_state)
        attempt_path = run_root / f"attempt-{run_index}.json"
        write_json_atomic(attempt_path, record)
        records.append(record)
        run_artifacts.append(
            {
                "run_index": run_index,
                "attempt_sha256": hashlib.sha256(attempt_id.encode("ascii")).hexdigest(),
                "artifact": attempt_path.name,
                "sha256": sha256_file(attempt_path),
                "evidence_binding_sha256": record["evidence_binding_sha256"],
            }
        )
    validate_bsc05_distinct_runs(records)
    firmware = records[0]["firmware"]
    assert isinstance(firmware, dict)
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc05-alert-generation-fence",
        "case_id": BSC05_CASE_ID,
        "collection_role": collection_role,
        "profile_role_id": role_id,
        "case_descriptor_sha256": descriptor_sha,
        "target_sha": git_state.head_sha,
        "session_sha256": hashlib.sha256(session_id.encode("ascii")).hexdigest(),
        "execution_mode": "simulated",
        "hardware_observed": False,
        "authoritative": False,
        "physical_collection_completed": False,
        "non_qualifying": True,
        "qualification_status": "BLOCKED",
        "qualification_blockers": list(
            case_drivers.get_case_driver(BSC05_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC05_REQUIRED_RUNS,
        "runs_completed": len(records),
        "production_replay_required": collection_role == "fault-collection",
        "firmware_target": {
            "environment": firmware["environment"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
            "build_kind": firmware["build_kind"],
        },
        "run_artifacts": run_artifacts,
        "artifact_sha256": {
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc05.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc05.rig-attestation.v1", rig_attestation
            ),
        },
    }
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def bsc13_profile_descriptor(role: str) -> dict[str, object]:
    code = "case_driver_contract_invalid"
    profile, errors = qualification.load_pinned_profile()
    if profile is None or errors:
        raise RunnerError(code, "BSC-13 pinned qualification profile is invalid")
    contract = next(
        (candidate for candidate in profile["required_cases"] if candidate["id"] == BSC13_CASE_ID),
        None,
    )
    row = require_exact_object(
        contract,
        {
            "id",
            "minimum_runs",
            "fault_build_required",
            "production_replay_required",
            "required_dut_capabilities",
            "required_rig_capabilities",
            "scenario",
            "production_replay",
        },
        code=code,
        label="BSC-13 case descriptor",
    )
    if (
        row.get("id") != BSC13_CASE_ID
        or type(row.get("minimum_runs")) is not int
        or row.get("minimum_runs") != BSC13_REQUIRED_RUNS
        or row.get("fault_build_required") is not True
        or row.get("production_replay_required") is not True
        or row.get("required_dut_capabilities") != list(BSC13_DUT_CAPABILITIES)
        or row.get("required_rig_capabilities") != list(BSC13_RIG_CAPABILITIES)
    ):
        raise RunnerError(code, "BSC-13 pinned case descriptor does not match the typed driver")

    expected_facts: list[dict[str, object]]
    if role == "fault-collection":
        descriptor = row.get("scenario")
        expected_role_id = "obd-connect-edge-preemption-fault"
        expected_build = "hil-fault"
        expected_faults = list(BSC13_FAULT_IDS)
        expected_barriers = list(BSC13_BARRIER_IDS)
        expected_facts = [
            {"id": "unowned-link-disconnected", "type": "boolean", "expected": True},
            {"id": "callback-confirmed-link-down", "type": "boolean", "expected": True},
            {"id": "coordinator-reached-idle", "type": "boolean", "expected": True},
            {"id": "phantom-connected-status-observed", "type": "boolean", "expected": False},
            {"id": "resume-scan-count", "type": "integer", "minimum": 1, "maximum": 1},
            {"id": "successful-reconnect-count", "type": "integer", "minimum": 1, "maximum": 1},
            {"id": "barrier-generation-matched", "type": "boolean", "expected": True},
        ]
    elif role == "production-replay":
        descriptor = row.get("production_replay")
        expected_role_id = "obd-connect-edge-production-replay"
        expected_build = "production"
        expected_faults = []
        expected_barriers = []
        expected_facts = [
            {"id": "orphan-link-observed", "type": "boolean", "expected": False},
            {"id": "phantom-connected-status-observed", "type": "boolean", "expected": False},
            {"id": "single-reconnect-succeeded", "type": "boolean", "expected": True},
            {"id": "hil-fault-control-active", "type": "boolean", "expected": False},
        ]
    else:
        raise RunnerError(code, "BSC-13 collection role is invalid")

    typed = require_exact_object(
        descriptor,
        {
            "role_id",
            "schema",
            "build_kind",
            "stimulus_ids",
            "fault_ids",
            "barrier_ids",
            "vbus_isolation_required",
            "reset_contract",
            "facts",
        },
        code=code,
        label="BSC-13 role descriptor",
    )
    expected_descriptor: dict[str, object] = {
        "role_id": expected_role_id,
        "schema": "case-observation-v1",
        "build_kind": expected_build,
        "stimulus_ids": list(BSC13_STIMULUS_IDS),
        "fault_ids": expected_faults,
        "barrier_ids": expected_barriers,
        "vbus_isolation_required": False,
        "reset_contract": {
            "expected_kind": "none",
            "expected_count": 0,
            "unexpected_count": 0,
        },
        "facts": expected_facts,
    }
    if typed != expected_descriptor:
        raise RunnerError(code, "BSC-13 pinned role descriptor does not match the typed driver")
    return expected_descriptor


def bsc13_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc13.case-record.v1", committed)


def validate_bsc13_hil_events(value: object, *, captured_generation: int, start_ms: int, end_ms: int) -> None:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != 3:
        raise RunnerError(code, "BSC-13 fault critical window lifecycle is incomplete")
    identity: tuple[int, int, int, int] | None = None
    elapsed_values: list[int] = []
    for sequence, (event, event_id) in enumerate(zip(value, ("ready", "fired", "released"), strict=True), start=1):
        row = require_exact_object(
            event,
            {"id", "sequence", "elapsed_ms", "arm_sequence", "ready_sequence", "generation", "phase"},
            code=code,
            label="BSC-13 HIL event",
        )
        elapsed = row.get("elapsed_ms")
        current_identity = tuple(row.get(field) for field in ("arm_sequence", "ready_sequence", "generation", "phase"))
        if (
            row.get("id") != event_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed < start_ms
            or elapsed > end_ms
            or any(type(item) is not int or item <= 0 for item in current_identity[:3])
            or current_identity[2] != captured_generation
            or type(current_identity[3]) is not int
            or current_identity[3] != 1
        ):
            raise RunnerError(code, "BSC-13 HIL event evidence is invalid")
        typed_identity = (current_identity[0], current_identity[1], current_identity[2], current_identity[3])
        if identity is None:
            identity = typed_identity
        elif typed_identity != identity:
            raise RunnerError(code, "BSC-13 HIL event identity changed during a critical window")
        elapsed_values.append(elapsed)
    if any(later < earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, "BSC-13 HIL event times moved backwards")


def validate_bsc13_critical_windows(value: object, *, role: str) -> list[dict[str, object]]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(BSC13_CRITICAL_WINDOW_ROLES):
        raise RunnerError(code, "BSC-13 critical-window coverage is incomplete")
    windows: list[dict[str, object]] = []
    for sequence, (window, expected_role) in enumerate(
        zip(value, BSC13_CRITICAL_WINDOW_ROLES, strict=True), start=1
    ):
        row = require_exact_object(
            window,
            {
                "role",
                "sequence",
                "start_elapsed_ms",
                "completion_elapsed_ms",
                "captured_generation",
                "cancellation_epoch_before",
                "cancellation_epoch_after",
                "callback_link_down_generation",
                "callback_confirmed_link_down",
                "session_ownership_adopted",
                "barrier_ready",
                "barrier_released",
                "coordinator_reached_idle",
                "coordinator_idle_elapsed_ms",
                "phantom_connected_status_observed",
                "resume_scan_count",
                "successful_reconnect_count",
                "hil_events",
            },
            code=code,
            label="BSC-13 critical window",
        )
        start_ms = row.get("start_elapsed_ms")
        completion_ms = row.get("completion_elapsed_ms")
        generation = row.get("captured_generation")
        epoch_before = row.get("cancellation_epoch_before")
        epoch_after = row.get("cancellation_epoch_after")
        idle_ms = row.get("coordinator_idle_elapsed_ms")
        if (
            row.get("role") != expected_role
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(start_ms) is not int
            or start_ms < 0
            or type(completion_ms) is not int
            or completion_ms < start_ms
            or type(generation) is not int
            or generation <= 0
            or type(epoch_before) is not int
            or epoch_before < 0
            or type(epoch_after) is not int
            or epoch_after <= epoch_before
            or type(row.get("callback_link_down_generation")) is not int
            or row.get("callback_link_down_generation") != generation
            or row.get("callback_confirmed_link_down") is not True
            or row.get("session_ownership_adopted") is not False
            or row.get("coordinator_reached_idle") is not True
            or type(idle_ms) is not int
            or idle_ms < 0
            or idle_ms > BSC13_IDLE_DEADLINE_MS
            or row.get("phantom_connected_status_observed") is not False
            or type(row.get("resume_scan_count")) is not int
            or row.get("resume_scan_count") != 1
            or type(row.get("successful_reconnect_count")) is not int
            or row.get("successful_reconnect_count") != 1
        ):
            raise RunnerError(code, "BSC-13 critical-window evidence is invalid")
        if role == "fault-collection":
            if row.get("barrier_ready") is not True or row.get("barrier_released") is not True:
                raise RunnerError(code, "BSC-13 fault critical window lacks its barrier lifecycle")
            validate_bsc13_hil_events(
                row.get("hil_events"),
                captured_generation=generation,
                start_ms=start_ms,
                end_ms=completion_ms,
            )
        elif (
            row.get("barrier_ready") is not False
            or row.get("barrier_released") is not False
            or row.get("hil_events") != []
        ):
            raise RunnerError(code, "BSC-13 production replay contains HIL barrier evidence")
        windows.append(row)
    return windows


def validate_bsc13_facts(value: object, *, role: str) -> None:
    code = "case_record_invalid"
    facts = require_exact_object(
        value,
        BSC13_FAULT_FACTS if role == "fault-collection" else BSC13_PRODUCTION_FACTS,
        code=code,
        label="BSC-13 facts",
    )
    if role == "fault-collection":
        if (
            facts.get("unowned-link-disconnected") is not True
            or facts.get("callback-confirmed-link-down") is not True
            or facts.get("coordinator-reached-idle") is not True
            or facts.get("phantom-connected-status-observed") is not False
            or type(facts.get("resume-scan-count")) is not int
            or facts.get("resume-scan-count") != 1
            or type(facts.get("successful-reconnect-count")) is not int
            or facts.get("successful-reconnect-count") != 1
            or facts.get("barrier-generation-matched") is not True
        ):
            raise RunnerError(code, "BSC-13 fault-build facts are invalid")
    elif (
        facts.get("orphan-link-observed") is not False
        or facts.get("phantom-connected-status-observed") is not False
        or facts.get("single-reconnect-succeeded") is not True
        or facts.get("hil-fault-control-active") is not False
    ):
        raise RunnerError(code, "BSC-13 production replay facts are invalid")


def validate_bsc13_adapter_record(
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
            "run_index",
            "target_sha",
            "dut_alias",
            "rig_alias",
            "execution_mode",
            "hardware_observed",
            "started_at_utc",
            "completed_at_utc",
            "descriptor",
            "firmware",
            "critical_windows",
            "facts",
            "capture_commitments",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-13 adapter record",
    )
    for field in (
        "case_id",
        "role",
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
            raise RunnerError(code, f"BSC-13 {field} does not match the runner invocation")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1:
        raise RunnerError(code, "BSC-13 adapter schema is unsupported")
    if type(record.get("run_index")) is not int or not 1 <= record["run_index"] <= BSC13_REQUIRED_RUNS:
        raise RunnerError(code, "BSC-13 run index is invalid")
    if type(record.get("hardware_observed")) is not bool:
        raise RunnerError(code, "BSC-13 hardware observation flag is invalid")
    role = record.get("role")
    if role not in {"fault-collection", "production-replay"}:
        raise RunnerError(code, "BSC-13 collection role is invalid")
    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-13 start")
    completed = parse_runner_utc(record.get("completed_at_utc"), code=code, label="BSC-13 completion")
    if completed < started:
        raise RunnerError(code, "BSC-13 completion predates its start")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "BSC-13 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=0):
        raise RunnerError(code, "BSC-13 physical record postdates adapter execution")

    descriptor = require_exact_object(
        record.get("descriptor"),
        set(bsc13_profile_descriptor(role)),
        code=code,
        label="BSC-13 bound descriptor",
    )
    if descriptor != bsc13_profile_descriptor(role):
        raise RunnerError(code, "BSC-13 adapter descriptor does not match the pinned role")

    firmware = require_exact_object(
        record.get("firmware"),
        {"environment", "target_sha", "binary_sha256", "hil_fault_control_active"},
        code=code,
        label="BSC-13 firmware",
    )
    expected_environment = BSC13_HIL_ENVIRONMENT if role == "fault-collection" else BSC13_PRODUCTION_ENVIRONMENT
    expected_hil = role == "fault-collection"
    if (
        firmware.get("environment") != expected_environment
        or firmware.get("target_sha") != expected.get("target_sha")
        or firmware.get("hil_fault_control_active") is not expected_hil
    ):
        raise RunnerError(code, "BSC-13 firmware role or target is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-13 firmware binary")

    validate_bsc13_critical_windows(record.get("critical_windows"), role=role)
    validate_bsc13_facts(record.get("facts"), role=role)
    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC13_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-13 capture commitments",
    )
    commitment_values = [
        require_sha256(commitments[field], code=code, label=field) for field in BSC13_CAPTURE_COMMITMENTS
    ]
    if len(set(commitment_values)) != len(commitment_values):
        raise RunnerError(code, "BSC-13 evidence roles reused the same capture")
    binding = require_sha256(
        record.get("evidence_binding_sha256"), code=code, label="BSC-13 evidence binding"
    )
    if not secrets.compare_digest(binding, bsc13_record_commitment(record)):
        raise RunnerError(code, "BSC-13 evidence binding does not match the record")
    return record


def validate_bsc13_distinct_runs(records: Sequence[Mapping[str, object]]) -> None:
    if len(records) != BSC13_REQUIRED_RUNS:
        raise RunnerError("case_runs_incomplete", "BSC-13 requires exactly three completed runs")
    for field in ("attempt_id", "evidence_binding_sha256"):
        if len({record.get(field) for record in records}) != BSC13_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", "BSC-13 run identities must be distinct")
    first_firmware = records[0].get("firmware")
    if any(record.get("firmware") != first_firmware for record in records[1:]):
        raise RunnerError("case_runs_mixed", "BSC-13 runs must use one bound firmware artifact")
    for field in BSC13_CAPTURE_COMMITMENTS:
        values = []
        for record in records:
            commitments = record.get("capture_commitments")
            assert isinstance(commitments, dict)
            values.append(commitments[field])
        if len(set(values)) != BSC13_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", "BSC-13 run captures must be distinct")
    generations: list[object] = []
    for record in records:
        windows = record.get("critical_windows")
        assert isinstance(windows, list)
        generations.extend(window["captured_generation"] for window in windows if isinstance(window, dict))
    if len(generations) != BSC13_REQUIRED_RUNS * len(BSC13_CRITICAL_WINDOW_ROLES) or len(set(generations)) != len(
        generations
    ):
        raise RunnerError("case_runs_reused", "BSC-13 critical-window generations must be distinct")


def resolve_bsc13_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError("local_inventory_missing", "BSC-13 requires the ignored local hardware inventory")
    try:
        inventory = resolve_hil_board.load_inventory(args.template, inventory_path)
        port_records = (
            resolve_hil_board.parse_port_records(resolve_hil_board._read_json(args.ports_json, "serial port inventory"))
            if args.ports_json is not None
            else resolve_hil_board.enumerate_serial_ports(str(pio_executable))
        )
    except resolve_hil_board.ResolverError as exc:
        raise RunnerError("case_board_resolution_failed", exc.message) from exc
    dut_resolution, dut_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.board,
        required_capabilities=BSC13_DUT_CAPABILITIES,
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=BSC13_RIG_CAPABILITIES,
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-13 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def run_bsc13_adapter(
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
        BSC13_CASE_ID,
        "--role",
        str(expected["role"]),
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
            timeout=BSC13_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-13 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-13 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-13 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 64 * 1024:
        raise RunnerError("case_record_invalid", "BSC-13 adapter output size is invalid")
    try:
        payload = json.loads(completed.stdout.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys)
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-13 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc13_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def run_bsc13_case(args: argparse.Namespace) -> int:
    role = "production-replay" if args.production_replay else "fault-collection"
    bsc13_profile_descriptor(role)
    if args.runs != BSC13_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-13 requires exactly three runs per collection role")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-13 requires an opaque local rig alias")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-13 collection roles are atomic")
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError("untrusted_override", "authoritative BSC-13 forbids an untracked rig adapter")
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-13 physical execution remains blocked until a tracked rig adapter exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-13 test execution requires a mocked adapter")

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-13 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-13 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc13_hardware(args, Path(args.pio_command))
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-13 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-13 serial endpoint is not present")

    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc13-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / f"{run_id}-{role}"
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-13 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc13-{secrets.token_hex(16)}"
    records: list[dict[str, object]] = []
    run_artifacts: list[dict[str, object]] = []
    for run_index in range(1, BSC13_REQUIRED_RUNS + 1):
        attempt_id = f"attempt-{secrets.token_hex(16)}"
        expected: dict[str, object] = {
            "case_id": BSC13_CASE_ID,
            "role": role,
            "session_id": session_id,
            "attempt_id": attempt_id,
            "run_index": run_index,
            "target_sha": git_state.head_sha,
            "dut_alias": args.board,
            "rig_alias": args.rig,
            "execution_mode": "simulated",
            "hardware_observed": False,
        }
        require_unchanged_git_state(repository, git_state)
        record = run_bsc13_adapter(
            adapter=adapter,
            repository=repository,
            serial_port=serial_port,
            expected=expected,
            environment=os.environ.copy(),
        )
        require_unchanged_git_state(repository, git_state)
        attempt_path = run_root / f"attempt-{run_index}.json"
        write_json_atomic(attempt_path, record)
        records.append(record)
        run_artifacts.append(
            {
                "run_index": run_index,
                "attempt_sha256": hashlib.sha256(attempt_id.encode("ascii")).hexdigest(),
                "artifact": attempt_path.name,
                "sha256": sha256_file(attempt_path),
                "evidence_binding_sha256": record["evidence_binding_sha256"],
            }
        )
    validate_bsc13_distinct_runs(records)
    firmware = records[0]["firmware"]
    assert isinstance(firmware, dict)
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc13-obd-connect-edge-preemption",
        "case_id": BSC13_CASE_ID,
        "collection_role": role,
        "target_sha": git_state.head_sha,
        "session_sha256": hashlib.sha256(session_id.encode("ascii")).hexdigest(),
        "execution_mode": "simulated",
        "hardware_observed": False,
        "authoritative": False,
        "physical_collection_completed": False,
        "non_qualifying": True,
        "qualification_status": "BLOCKED",
        "qualification_blockers": list(case_drivers.get_case_driver(BSC13_CASE_ID).qualification_blockers),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC13_REQUIRED_RUNS,
        "runs_completed": len(records),
        "production_replay_required": role == "fault-collection",
        "critical_window_roles": list(BSC13_CRITICAL_WINDOW_ROLES),
        "firmware_target": {
            "environment": firmware["environment"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
        },
        "run_artifacts": run_artifacts,
        "artifact_sha256": {
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment("v1simple.bsc13.dut-attestation.v1", dut_attestation),
            "rig_attestation": canonical_case_commitment("v1simple.bsc13.rig-attestation.v1", rig_attestation),
        },
    }
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def load_bsc14_case_descriptor() -> dict[str, object]:
    profile, errors = qualification.load_pinned_profile()
    if profile is None or errors:
        raise RunnerError(
            "qualification_profile_invalid",
            "pinned BSC-14 qualification descriptor is invalid",
        )
    descriptor = next(
        (entry for entry in profile["required_cases"] if entry["id"] == BSC14_CASE_ID),
        None,
    )
    if not isinstance(descriptor, dict):
        raise RunnerError(
            "case_driver_contract_invalid",
            "BSC-14 is absent from the pinned qualification profile",
        )
    return descriptor


def bsc14_role_descriptor(
    case_descriptor: Mapping[str, object], *, production_replay: bool
) -> dict[str, object]:
    key = "production_replay" if production_replay else "scenario"
    role = case_descriptor.get(key)
    if not isinstance(role, dict):
        raise RunnerError(
            "case_driver_contract_invalid",
            "pinned BSC-14 role descriptor is unavailable",
        )
    return role


def bsc14_descriptor_commitment(case_descriptor: Mapping[str, object]) -> str:
    return canonical_case_commitment(
        "v1simple.bsc14.case-descriptor.v1", case_descriptor
    )




def resolve_bsc14_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
    case_descriptor: Mapping[str, object],
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-14 requires the ignored local hardware inventory",
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
        required_capabilities=case_descriptor["required_dut_capabilities"],
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=case_descriptor["required_rig_capabilities"],
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-14 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def bsc14_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc14.case-record.v1", committed)


def validate_bsc14_stimuli(value: object, expected_ids: Sequence[str]) -> None:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, "BSC-14 stimulus sequence is incomplete")
    elapsed_values: list[int] = []
    for sequence, (stimulus, expected_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            stimulus,
            {"id", "sequence", "elapsed_ms", "result"},
            code=code,
            label="BSC-14 stimulus",
        )
        elapsed = row.get("elapsed_ms")
        if (
            row.get("id") != expected_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed < 0
            or row.get("result") != "pass"
        ):
            raise RunnerError(code, "BSC-14 stimulus order or result is invalid")
        elapsed_values.append(elapsed)
    if any(later <= earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, "BSC-14 stimulus times must increase strictly")


def validate_bsc14_faults(value: object, expected_ids: Sequence[str]) -> None:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, "BSC-14 fault sequence does not match the pinned descriptor")
    for sequence, (event, event_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            event,
            {
                "id",
                "sequence",
                "armed_elapsed_ms",
                "triggered_elapsed_ms",
                "cleared_elapsed_ms",
            },
            code=code,
            label="BSC-14 fault",
        )
        times = tuple(
            row.get(field)
            for field in (
                "armed_elapsed_ms",
                "triggered_elapsed_ms",
                "cleared_elapsed_ms",
            )
        )
        if (
            row.get("id") != event_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or any(type(item) is not int or item < 0 for item in times)
            or not (times[0] <= times[1] <= times[2])
        ):
            raise RunnerError(code, "BSC-14 fault evidence is invalid")


def validate_bsc14_barriers(value: object, expected_ids: Sequence[str]) -> None:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, "BSC-14 barriers do not match the pinned descriptor")
    for sequence, (event, event_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            event,
            {"id", "sequence", "ready_elapsed_ms", "released_elapsed_ms", "timed_out"},
            code=code,
            label="BSC-14 barrier",
        )
        ready = row.get("ready_elapsed_ms")
        released = row.get("released_elapsed_ms")
        if (
            row.get("id") != event_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(ready) is not int
            or ready < 0
            or type(released) is not int
            or released < ready
            or row.get("timed_out") is not False
        ):
            raise RunnerError(code, "BSC-14 barrier evidence is invalid")


def validate_bsc14_resets(value: object, reset_contract: Mapping[str, object]) -> None:
    code = "case_record_invalid"
    resets = require_exact_object(
        value,
        {"expected_kind", "planned", "observed", "unexpected"},
        code=code,
        label="BSC-14 resets",
    )
    expected_count = reset_contract.get("expected_count")
    if (
        resets.get("expected_kind") != reset_contract.get("expected_kind")
        or type(resets.get("planned")) is not int
        or resets.get("planned") != expected_count
        or type(resets.get("observed")) is not int
        or resets.get("observed") != expected_count
        or type(resets.get("unexpected")) is not int
        or resets.get("unexpected") != reset_contract.get("unexpected_count")
    ):
        raise RunnerError(code, "BSC-14 reset evidence does not match the pinned descriptor")


def validate_bsc14_facts(value: object, contracts: object) -> None:
    code = "case_record_invalid"
    if not isinstance(contracts, list):
        raise RunnerError(code, "BSC-14 fact descriptor is invalid")
    contract_by_id = {
        contract.get("id"): contract for contract in contracts if isinstance(contract, dict)
    }
    if len(contract_by_id) != len(contracts) or not all(
        isinstance(fact_id, str) for fact_id in contract_by_id
    ):
        raise RunnerError(code, "BSC-14 fact descriptor is invalid")
    facts = require_exact_object(
        value, set(contract_by_id), code=code, label="BSC-14 facts"
    )
    for fact_id, contract in contract_by_id.items():
        observed = facts.get(fact_id)
        if contract.get("type") == "boolean":
            if type(observed) is not bool or observed is not contract.get("expected"):
                raise RunnerError(code, f"BSC-14 fact {fact_id} is invalid")
        elif contract.get("type") == "integer":
            if (
                type(observed) is not int
                or observed < contract.get("minimum")
                or observed > contract.get("maximum")
            ):
                raise RunnerError(code, f"BSC-14 fact {fact_id} is invalid")
        else:
            raise RunnerError(code, "BSC-14 fact descriptor type is invalid")


def validate_bsc14_adapter_record(
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
            "role_id",
            "session_id",
            "attempt_id",
            "target_sha",
            "dut_alias",
            "rig_alias",
            "execution_mode",
            "hardware_observed",
            "started_at_utc",
            "completed_at_utc",
            "case_descriptor",
            "case_descriptor_sha256",
            "firmware",
            "stimuli",
            "faults",
            "barriers",
            "vbus_isolated",
            "resets",
            "facts",
            "capture_commitments",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-14 adapter record",
    )
    for field in (
        "case_id",
        "role_id",
        "session_id",
        "attempt_id",
        "target_sha",
        "dut_alias",
        "rig_alias",
        "execution_mode",
        "hardware_observed",
    ):
        if record.get(field) != expected.get(field):
            raise RunnerError(code, f"BSC-14 {field} does not match the runner invocation")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1:
        raise RunnerError(code, "BSC-14 adapter schema is unsupported")
    if type(record.get("hardware_observed")) is not bool:
        raise RunnerError(code, "BSC-14 hardware observation flag is invalid")
    case_descriptor = expected.get("case_descriptor")
    role_descriptor = expected.get("role_descriptor")
    if not isinstance(case_descriptor, dict) or not isinstance(role_descriptor, dict):
        raise RunnerError(code, "BSC-14 pinned descriptor binding is invalid")
    if record.get("case_descriptor") != case_descriptor:
        raise RunnerError(code, "BSC-14 case descriptor does not match the pinned profile")
    expected_descriptor_sha = bsc14_descriptor_commitment(case_descriptor)
    if (
        expected.get("case_descriptor_sha256") != expected_descriptor_sha
        or record.get("case_descriptor_sha256") != expected_descriptor_sha
    ):
        raise RunnerError(code, "BSC-14 case descriptor digest is invalid")
    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-14 start")
    completed = parse_runner_utc(record.get("completed_at_utc"), code=code, label="BSC-14 completion")
    if completed < started:
        raise RunnerError(code, "BSC-14 completion predates its start")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "BSC-14 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=0):
        raise RunnerError(code, "BSC-14 physical record postdates adapter execution")

    firmware = require_exact_object(
        record.get("firmware"),
        {
            "environment",
            "target_sha",
            "binary_sha256",
            "hil_fault_control_active",
            "build_kind",
        },
        code=code,
        label="BSC-14 firmware",
    )
    build_kind = role_descriptor.get("build_kind")
    expected_environment = (
        BSC14_HIL_ENVIRONMENT if build_kind == "hil-fault" else BSC14_PRODUCTION_ENVIRONMENT
    )
    expected_hil = build_kind == "hil-fault"
    if (
        firmware.get("environment") != expected_environment
        or firmware.get("target_sha") != expected.get("target_sha")
        or firmware.get("hil_fault_control_active") is not expected_hil
        or firmware.get("build_kind") != build_kind
    ):
        raise RunnerError(code, "BSC-14 firmware role or target is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-14 firmware binary")

    validate_bsc14_stimuli(record.get("stimuli"), role_descriptor["stimulus_ids"])
    validate_bsc14_faults(record.get("faults"), role_descriptor["fault_ids"])
    validate_bsc14_barriers(record.get("barriers"), role_descriptor["barrier_ids"])
    if (
        type(record.get("vbus_isolated")) is not bool
        or record.get("vbus_isolated") is not role_descriptor["vbus_isolation_required"]
    ):
        raise RunnerError(code, "BSC-14 VBUS observation does not match the pinned descriptor")
    validate_bsc14_resets(record.get("resets"), role_descriptor["reset_contract"])
    validate_bsc14_facts(record.get("facts"), role_descriptor["facts"])

    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC14_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-14 capture commitments",
    )
    commitment_values = [
        require_sha256(commitments[field], code=code, label=field)
        for field in BSC14_CAPTURE_COMMITMENTS
    ]
    if len(set(commitment_values)) != len(commitment_values):
        raise RunnerError(code, "BSC-14 evidence roles reused the same capture")
    binding = require_sha256(
        record.get("evidence_binding_sha256"), code=code, label="BSC-14 evidence binding"
    )
    if not secrets.compare_digest(binding, bsc14_record_commitment(record)):
        raise RunnerError(code, "BSC-14 evidence binding does not match the record")
    return record


def run_bsc14_adapter(
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
        BSC14_CASE_ID,
        "--role-id",
        str(expected["role_id"]),
        "--case-descriptor-sha256",
        str(expected["case_descriptor_sha256"]),
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
            timeout=BSC14_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-14 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-14 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-14 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 64 * 1024:
        raise RunnerError("case_record_invalid", "BSC-14 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-14 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc14_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def run_bsc14_case(args: argparse.Namespace) -> int:
    case_descriptor = load_bsc14_case_descriptor()
    if args.runs != case_descriptor["minimum_runs"]:
        raise RunnerError("invalid_runs", "BSC-14 requires exactly one run per collection role")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-14 requires an opaque local rig alias")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-14 collection roles are atomic")
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError("untrusted_override", "authoritative BSC-14 forbids an untracked rig adapter")
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-14 physical execution remains blocked until a tracked rig adapter exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-14 test execution requires a mocked adapter")
    role_descriptor = bsc14_role_descriptor(
        case_descriptor, production_replay=args.production_replay
    )
    reset_contract = role_descriptor["reset_contract"]
    if reset_contract["expected_count"] > 0 and not args.ack_destructive_hard_cuts:
        raise RunnerError(
            "safety_ack_required",
            "BSC-14 fault collection requires destructive-reset acknowledgement",
        )

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-14 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-14 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc14_hardware(
        args, Path(args.pio_command), case_descriptor
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-14 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-14 serial endpoint is not present")

    role_id = role_descriptor["role_id"]
    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc14-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / f"{run_id}-{role_id}"
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-14 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc14-{secrets.token_hex(16)}"
    attempt_id = f"attempt-{secrets.token_hex(16)}"
    descriptor_sha = bsc14_descriptor_commitment(case_descriptor)
    expected: dict[str, object] = {
        "case_id": BSC14_CASE_ID,
        "role_id": role_id,
        "session_id": session_id,
        "attempt_id": attempt_id,
        "target_sha": git_state.head_sha,
        "dut_alias": args.board,
        "rig_alias": args.rig,
        "execution_mode": "simulated",
        "hardware_observed": False,
        "case_descriptor": case_descriptor,
        "case_descriptor_sha256": descriptor_sha,
        "role_descriptor": role_descriptor,
    }
    require_unchanged_git_state(repository, git_state)
    record = run_bsc14_adapter(
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
        "run_kind": "bug-squash-bsc14-storage-reset-durability",
        "case_id": BSC14_CASE_ID,
        "collection_role": role_id,
        "case_descriptor_sha256": descriptor_sha,
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
            case_drivers.get_case_driver(BSC14_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": case_descriptor["minimum_runs"],
        "runs_completed": 1,
        "production_replay_required": bool(
            case_descriptor["production_replay_required"] and not args.production_replay
        ),
        "firmware_target": {
            "environment": firmware["environment"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
            "build_kind": firmware["build_kind"],
        },
        "evidence_binding_sha256": record["evidence_binding_sha256"],
        "artifact_sha256": {
            "adapter_record": sha256_file(attempt_path),
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc14.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc14.rig-attestation.v1", rig_attestation
            ),
        },
    }
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def resolve_bsc16_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-16 requires the ignored local hardware inventory",
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
        required_capabilities=BSC16_DUT_CAPABILITIES,
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=BSC16_RIG_CAPABILITIES,
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-16 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def bsc16_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc16.case-record.v1", committed)


def validate_bsc16_stimuli(value: object, expected_ids: Sequence[str]) -> None:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, "BSC-16 stimulus sequence is incomplete")
    elapsed_values: list[int] = []
    for sequence, (stimulus, expected_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            stimulus,
            {"id", "sequence", "elapsed_ms", "result"},
            code=code,
            label="BSC-16 stimulus",
        )
        elapsed = row.get("elapsed_ms")
        if (
            row.get("id") != expected_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed < 0
            or row.get("result") != "pass"
        ):
            raise RunnerError(code, "BSC-16 stimulus order or result is invalid")
        elapsed_values.append(elapsed)
    if any(later <= earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, "BSC-16 stimulus times must increase strictly")


def validate_bsc16_fault_lifecycle(value: object, *, role: str) -> None:
    code = "case_record_invalid"
    if role == "production-replay":
        if value != []:
            raise RunnerError(code, "BSC-16 production replay contains HIL fault events")
        return
    if not isinstance(value, list) or len(value) != 3:
        raise RunnerError(code, "BSC-16 ADC fault lifecycle is incomplete")
    identity: tuple[int, int, int, int] | None = None
    elapsed_values: list[int] = []
    for sequence, (event, event_id) in enumerate(
        zip(value, ("ready", "fired", "released"), strict=True), start=1
    ):
        row = require_exact_object(
            event,
            {
                "id",
                "sequence",
                "elapsed_ms",
                "arm_sequence",
                "ready_sequence",
                "generation",
                "phase",
                "latch_initialized",
                "adc_handle_allocated",
                "voltage_valid",
                "source_classification",
                "power_button_enabled",
            },
            code=code,
            label="BSC-16 ADC fault event",
        )
        elapsed = row.get("elapsed_ms")
        current_identity = tuple(
            row.get(field)
            for field in ("arm_sequence", "ready_sequence", "generation", "phase")
        )
        if (
            row.get("id") != event_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed < 0
            or any(type(item) is not int or item <= 0 for item in current_identity[:3])
            or type(current_identity[3]) is not int
            or current_identity[3] != 1
            or row.get("latch_initialized") is not True
            or row.get("adc_handle_allocated") is not False
            or row.get("voltage_valid") is not False
            or row.get("source_classification") not in {"battery", "unknown"}
            or row.get("power_button_enabled") is not True
        ):
            raise RunnerError(code, "BSC-16 ADC fault lifecycle evidence is invalid")
        typed_identity = (
            current_identity[0],
            current_identity[1],
            current_identity[2],
            current_identity[3],
        )
        if identity is None:
            identity = typed_identity
        elif typed_identity != identity:
            raise RunnerError(code, "BSC-16 ADC fault identity changed during execution")
        elapsed_values.append(elapsed)
    if any(later < earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, "BSC-16 ADC fault times moved backwards")


def validate_bsc16_facts(value: object, *, role: str) -> None:
    code = "case_record_invalid"
    expected_keys = BSC16_FAULT_FACTS if role == "fault-collection" else BSC16_PRODUCTION_FACTS
    facts = require_exact_object(value, expected_keys, code=code, label="BSC-16 facts")
    if role == "fault-collection":
        bounce = facts.get("gpio16-bounce-ms")
        delay = facts.get("usb-confirmation-delay-ms")
        if (
            facts.get("pwr-wake-transient-usb-observed") is not False
            or type(delay) is not int
            or delay < 2800
            or delay > 4000
            or facts.get("adc-failure-voltage-degraded") is not True
            or facts.get("adc-failure-power-button-operational") is not True
            or facts.get("long-hold-classified-as-usb") is not False
            or facts.get("long-hold-shutdown-succeeded") is not True
            or facts.get("source-flapping-observed") is not False
            or type(bounce) is not int
            or bounce < 0
            or bounce > 24
        ):
            raise RunnerError(code, "BSC-16 fault-build facts do not satisfy the policy bounds")
    elif (
        facts.get("battery-classification-correct") is not True
        or facts.get("usb-classification-correct") is not True
        or facts.get("power-button-operational") is not True
        or facts.get("source-flapping-observed") is not False
        or facts.get("hil-fault-control-active") is not False
    ):
        raise RunnerError(code, "BSC-16 production replay facts are invalid")


def validate_bsc16_adapter_record(
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
            "stimuli",
            "facts",
            "fault_lifecycle",
            "capture_commitments",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-16 adapter record",
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
            raise RunnerError(code, f"BSC-16 {field} does not match the runner invocation")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1:
        raise RunnerError(code, "BSC-16 adapter schema is unsupported")
    if type(record.get("hardware_observed")) is not bool:
        raise RunnerError(code, "BSC-16 hardware observation flag is invalid")
    role = record.get("role")
    if role not in {"fault-collection", "production-replay"}:
        raise RunnerError(code, "BSC-16 collection role is invalid")
    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-16 start")
    completed = parse_runner_utc(record.get("completed_at_utc"), code=code, label="BSC-16 completion")
    if completed < started:
        raise RunnerError(code, "BSC-16 completion predates its start")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "BSC-16 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=0):
        raise RunnerError(code, "BSC-16 physical record postdates adapter execution")

    firmware = require_exact_object(
        record.get("firmware"),
        {"environment", "target_sha", "binary_sha256", "hil_fault_control_active"},
        code=code,
        label="BSC-16 firmware",
    )
    expected_environment = BSC16_HIL_ENVIRONMENT if role == "fault-collection" else BSC16_PRODUCTION_ENVIRONMENT
    expected_hil = role == "fault-collection"
    if (
        firmware.get("environment") != expected_environment
        or firmware.get("target_sha") != expected.get("target_sha")
        or firmware.get("hil_fault_control_active") is not expected_hil
    ):
        raise RunnerError(code, "BSC-16 firmware role or target is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-16 firmware binary")

    validate_bsc16_stimuli(
        record.get("stimuli"),
        BSC16_FAULT_STIMULUS_IDS if role == "fault-collection" else BSC16_PRODUCTION_STIMULUS_IDS,
    )
    validate_bsc16_facts(record.get("facts"), role=role)
    validate_bsc16_fault_lifecycle(record.get("fault_lifecycle"), role=role)

    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC16_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-16 capture commitments",
    )
    commitment_values = [
        require_sha256(commitments[field], code=code, label=field)
        for field in BSC16_CAPTURE_COMMITMENTS
    ]
    if len(set(commitment_values)) != len(commitment_values):
        raise RunnerError(code, "BSC-16 evidence roles reused the same capture")
    binding = require_sha256(
        record.get("evidence_binding_sha256"), code=code, label="BSC-16 evidence binding"
    )
    if not secrets.compare_digest(binding, bsc16_record_commitment(record)):
        raise RunnerError(code, "BSC-16 evidence binding does not match the record")
    return record


def run_bsc16_adapter(
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
        BSC16_CASE_ID,
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
            timeout=BSC16_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-16 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-16 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-16 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 64 * 1024:
        raise RunnerError("case_record_invalid", "BSC-16 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-16 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc16_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def run_bsc16_case(args: argparse.Namespace) -> int:
    if args.runs != BSC16_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-16 requires exactly one run per collection role")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-16 requires an opaque local rig alias")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-16 collection roles are atomic")
    if not args.ack_vbus_isolated or not args.ack_destructive_hard_cuts:
        raise RunnerError(
            "safety_ack_required",
            "BSC-16 requires explicit VBUS-isolation and destructive-cut acknowledgements",
        )
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError("untrusted_override", "authoritative BSC-16 forbids an untracked rig adapter")
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-16 physical execution remains blocked until a tracked rig adapter exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-16 test execution requires a mocked adapter")

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-16 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-16 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc16_hardware(args, Path(args.pio_command))
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-16 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-16 serial endpoint is not present")

    role = "production-replay" if args.production_replay else "fault-collection"
    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc16-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / f"{run_id}-{role}"
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-16 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc16-{secrets.token_hex(16)}"
    attempt_id = f"attempt-{secrets.token_hex(16)}"
    expected: dict[str, object] = {
        "case_id": BSC16_CASE_ID,
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
    record = run_bsc16_adapter(
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
        "run_kind": "bug-squash-bsc16-battery-source-policy",
        "case_id": BSC16_CASE_ID,
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
            case_drivers.get_case_driver(BSC16_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC16_REQUIRED_RUNS,
        "runs_completed": 1,
        "production_replay_required": role == "fault-collection",
        "firmware_target": {
            "environment": firmware["environment"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
        },
        "evidence_binding_sha256": record["evidence_binding_sha256"],
        "artifact_sha256": {
            "adapter_record": sha256_file(attempt_path),
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc16.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc16.rig-attestation.v1", rig_attestation
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


def load_bsc10_case_descriptor() -> dict[str, object]:
    profile, errors = qualification.load_pinned_profile()
    if profile is None or errors:
        raise RunnerError(
            "qualification_profile_invalid",
            "pinned BSC-10 qualification descriptor is invalid",
        )
    descriptor = next(
        (entry for entry in profile["required_cases"] if entry["id"] == BSC10_CASE_ID),
        None,
    )
    if not isinstance(descriptor, dict):
        raise RunnerError(
            "case_driver_contract_invalid",
            "BSC-10 is absent from the pinned qualification profile",
        )
    return descriptor


def bsc10_role_descriptor(
    case_descriptor: Mapping[str, object], *, production_replay: bool
) -> dict[str, object]:
    key = "production_replay" if production_replay else "scenario"
    role = case_descriptor.get(key)
    if not isinstance(role, dict):
        raise RunnerError(
            "case_driver_contract_invalid",
            "pinned BSC-10 role descriptor is unavailable",
        )
    return role


def bsc10_descriptor_commitment(case_descriptor: Mapping[str, object]) -> str:
    return canonical_case_commitment(
        "v1simple.bsc10.case-descriptor.v1", case_descriptor
    )


def resolve_bsc10_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
    case_descriptor: Mapping[str, object],
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-10 requires the ignored local hardware inventory",
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
        required_capabilities=case_descriptor["required_dut_capabilities"],
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=case_descriptor["required_rig_capabilities"],
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-10 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def bsc10_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc10.case-record.v1", committed)


def validate_bsc10_stimuli(
    value: object, expected_ids: Sequence[str]
) -> list[dict[str, object]]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, "BSC-10 stimulus sequence is incomplete")
    rows: list[dict[str, object]] = []
    elapsed_values: list[int] = []
    for sequence, (stimulus, expected_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            stimulus,
            {"id", "sequence", "elapsed_ms", "result"},
            code=code,
            label="BSC-10 stimulus",
        )
        elapsed = row.get("elapsed_ms")
        if (
            row.get("id") != expected_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed < 0
            or row.get("result") != "pass"
        ):
            raise RunnerError(code, "BSC-10 stimulus order, timing, or result is invalid")
        rows.append(row)
        elapsed_values.append(elapsed)
    if any(later <= earlier for earlier, later in zip(elapsed_values, elapsed_values[1:])):
        raise RunnerError(code, "BSC-10 stimulus times must increase strictly")
    return rows


def validate_bsc10_faults(
    value: object, expected_ids: Sequence[str]
) -> list[dict[str, object]]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, "BSC-10 fault sequence does not match the pinned descriptor")
    rows: list[dict[str, object]] = []
    for sequence, (fault, expected_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            fault,
            {
                "id",
                "sequence",
                "armed_elapsed_ms",
                "triggered_elapsed_ms",
                "cleared_elapsed_ms",
            },
            code=code,
            label="BSC-10 fault",
        )
        times = tuple(
            row.get(field)
            for field in (
                "armed_elapsed_ms",
                "triggered_elapsed_ms",
                "cleared_elapsed_ms",
            )
        )
        if (
            row.get("id") != expected_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or any(type(item) is not int or item < 0 for item in times)
            or not (times[0] <= times[1] <= times[2])
        ):
            raise RunnerError(code, "BSC-10 fault identity, order, or timing is invalid")
        rows.append(row)
    return rows


def validate_bsc10_barriers(
    value: object, expected_ids: Sequence[str]
) -> list[dict[str, object]]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError(code, "BSC-10 barriers do not match the pinned descriptor")
    rows: list[dict[str, object]] = []
    for sequence, (barrier, expected_id) in enumerate(
        zip(value, expected_ids, strict=True), start=1
    ):
        row = require_exact_object(
            barrier,
            {"id", "sequence", "ready_elapsed_ms", "released_elapsed_ms", "timed_out"},
            code=code,
            label="BSC-10 barrier",
        )
        ready = row.get("ready_elapsed_ms")
        released = row.get("released_elapsed_ms")
        if (
            row.get("id") != expected_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(ready) is not int
            or ready < 0
            or type(released) is not int
            or released < ready
            or row.get("timed_out") is not False
        ):
            raise RunnerError(code, "BSC-10 barrier identity, order, or timing is invalid")
        rows.append(row)
    return rows


def validate_bsc10_resets(value: object, reset_contract: Mapping[str, object]) -> None:
    code = "case_record_invalid"
    resets = require_exact_object(
        value,
        {"expected_kind", "planned", "observed", "unexpected"},
        code=code,
        label="BSC-10 resets",
    )
    expected_count = reset_contract.get("expected_count")
    if (
        resets.get("expected_kind") != reset_contract.get("expected_kind")
        or type(resets.get("planned")) is not int
        or resets.get("planned") != expected_count
        or type(resets.get("observed")) is not int
        or resets.get("observed") != expected_count
        or type(resets.get("unexpected")) is not int
        or resets.get("unexpected") != reset_contract.get("unexpected_count")
    ):
        raise RunnerError(code, "BSC-10 reset evidence does not match the pinned descriptor")


def validate_bsc10_facts(value: object, contracts: object) -> None:
    code = "case_record_invalid"
    if not isinstance(contracts, list):
        raise RunnerError(code, "BSC-10 fact descriptor is invalid")
    contract_by_id = {
        contract.get("id"): contract for contract in contracts if isinstance(contract, dict)
    }
    if len(contract_by_id) != len(contracts) or not all(
        isinstance(fact_id, str) for fact_id in contract_by_id
    ):
        raise RunnerError(code, "BSC-10 fact descriptor is invalid")
    facts = require_exact_object(value, set(contract_by_id), code=code, label="BSC-10 facts")
    for fact_id, contract in contract_by_id.items():
        observed = facts.get(fact_id)
        if contract.get("type") == "boolean":
            if type(observed) is not bool or observed is not contract.get("expected"):
                raise RunnerError(code, f"BSC-10 fact {fact_id} is invalid")
        elif contract.get("type") == "integer":
            minimum = contract.get("minimum")
            maximum = contract.get("maximum")
            if (
                type(observed) is not int
                or type(minimum) is not int
                or type(maximum) is not int
                or observed < minimum
                or observed > maximum
            ):
                raise RunnerError(code, f"BSC-10 fact {fact_id} is invalid")
        else:
            raise RunnerError(code, "BSC-10 fact descriptor type is invalid")


def validate_bsc10_timeline(
    *,
    stimuli: Sequence[Mapping[str, object]],
    faults: Sequence[Mapping[str, object]],
    barriers: Sequence[Mapping[str, object]],
    production_replay: bool,
    duration_ms: int,
) -> None:
    code = "case_record_invalid"
    observed_times = [int(row["elapsed_ms"]) for row in stimuli]
    for row in faults:
        observed_times.extend(
            int(row[field])
            for field in (
                "armed_elapsed_ms",
                "triggered_elapsed_ms",
                "cleared_elapsed_ms",
            )
        )
    for row in barriers:
        observed_times.extend(
            int(row[field]) for field in ("ready_elapsed_ms", "released_elapsed_ms")
        )
    if observed_times and max(observed_times) > duration_ms + 1_000:
        raise RunnerError(code, "BSC-10 observations exceed the recorded run duration")
    if production_replay:
        if len(stimuli) != 2 or faults or barriers:
            raise RunnerError(code, "BSC-10 production replay contains fault instrumentation")
        return
    if len(stimuli) != 5 or len(faults) != 2 or len(barriers) != 1:
        raise RunnerError(code, "BSC-10 fault timeline is incomplete")
    stimulus_times = [int(row["elapsed_ms"]) for row in stimuli]
    first_fault = faults[0]
    response_fault = faults[1]
    barrier = barriers[0]
    if not (
        int(first_fault["armed_elapsed_ms"])
        <= int(first_fault["triggered_elapsed_ms"])
        <= stimulus_times[0]
        <= int(first_fault["cleared_elapsed_ms"])
        <= stimulus_times[1]
        and int(barrier["ready_elapsed_ms"])
        <= stimulus_times[0]
        <= int(barrier["released_elapsed_ms"])
        <= stimulus_times[1]
        and int(first_fault["triggered_elapsed_ms"])
        <= int(barrier["ready_elapsed_ms"])
        <= int(barrier["released_elapsed_ms"])
        <= int(first_fault["cleared_elapsed_ms"])
        and stimulus_times[1] < stimulus_times[2] < stimulus_times[3]
        and int(response_fault["armed_elapsed_ms"])
        <= stimulus_times[3]
        <= int(response_fault["triggered_elapsed_ms"])
        <= int(response_fault["cleared_elapsed_ms"])
        <= stimulus_times[4]
    ):
        raise RunnerError(code, "BSC-10 fault, barrier, and stimulus windows are inconsistent")


def validate_bsc10_adapter_record(
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
            "role_id",
            "session_id",
            "attempt_id",
            "target_sha",
            "dut_alias",
            "rig_alias",
            "execution_mode",
            "hardware_observed",
            "started_at_utc",
            "completed_at_utc",
            "case_descriptor",
            "case_descriptor_sha256",
            "firmware",
            "stimuli",
            "faults",
            "barriers",
            "vbus_isolated",
            "resets",
            "facts",
            "capture_commitments",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-10 adapter record",
    )
    for field in (
        "case_id",
        "role_id",
        "session_id",
        "attempt_id",
        "target_sha",
        "dut_alias",
        "rig_alias",
        "execution_mode",
        "hardware_observed",
    ):
        if record.get(field) != expected.get(field):
            raise RunnerError(code, f"BSC-10 {field} does not match the runner invocation")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1:
        raise RunnerError(code, "BSC-10 adapter schema is unsupported")
    if type(record.get("hardware_observed")) is not bool:
        raise RunnerError(code, "BSC-10 hardware observation flag is invalid")

    case_descriptor = expected.get("case_descriptor")
    role_descriptor = expected.get("role_descriptor")
    if not isinstance(case_descriptor, dict) or not isinstance(role_descriptor, dict):
        raise RunnerError(code, "BSC-10 pinned descriptor binding is invalid")
    if record.get("case_descriptor") != case_descriptor:
        raise RunnerError(code, "BSC-10 case descriptor does not match the pinned profile")
    expected_descriptor_sha = bsc10_descriptor_commitment(case_descriptor)
    if (
        expected.get("case_descriptor_sha256") != expected_descriptor_sha
        or record.get("case_descriptor_sha256") != expected_descriptor_sha
    ):
        raise RunnerError(code, "BSC-10 case descriptor digest is invalid")

    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-10 start")
    completed = parse_runner_utc(record.get("completed_at_utc"), code=code, label="BSC-10 completion")
    if completed < started:
        raise RunnerError(code, "BSC-10 completion predates its start")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "BSC-10 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=999999):
        raise RunnerError(code, "BSC-10 physical record postdates adapter execution")

    firmware = require_exact_object(
        record.get("firmware"),
        {
            "environment",
            "target_sha",
            "binary_sha256",
            "hil_fault_control_active",
            "build_kind",
        },
        code=code,
        label="BSC-10 firmware",
    )
    build_kind = role_descriptor.get("build_kind")
    expected_environment = (
        BSC10_HIL_ENVIRONMENT if build_kind == "hil-fault" else BSC10_PRODUCTION_ENVIRONMENT
    )
    expected_hil = build_kind == "hil-fault"
    if (
        firmware.get("environment") != expected_environment
        or firmware.get("target_sha") != expected.get("target_sha")
        or firmware.get("hil_fault_control_active") is not expected_hil
        or firmware.get("build_kind") != build_kind
    ):
        raise RunnerError(code, "BSC-10 firmware role or target is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-10 firmware binary")

    stimuli = validate_bsc10_stimuli(record.get("stimuli"), role_descriptor["stimulus_ids"])
    faults = validate_bsc10_faults(record.get("faults"), role_descriptor["fault_ids"])
    barriers = validate_bsc10_barriers(record.get("barriers"), role_descriptor["barrier_ids"])
    if (
        type(record.get("vbus_isolated")) is not bool
        or record.get("vbus_isolated") is not role_descriptor["vbus_isolation_required"]
    ):
        raise RunnerError(code, "BSC-10 VBUS observation does not match the pinned descriptor")
    validate_bsc10_resets(record.get("resets"), role_descriptor["reset_contract"])
    validate_bsc10_facts(record.get("facts"), role_descriptor["facts"])
    validate_bsc10_timeline(
        stimuli=stimuli,
        faults=faults,
        barriers=barriers,
        production_replay=bool(expected.get("production_replay")),
        duration_ms=int((completed - started).total_seconds() * 1_000),
    )

    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC10_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-10 capture commitments",
    )
    commitment_values = [
        require_sha256(commitments[field], code=code, label=field)
        for field in BSC10_CAPTURE_COMMITMENTS
    ]
    if len(set(commitment_values)) != len(commitment_values):
        raise RunnerError(code, "BSC-10 evidence roles reused the same capture")
    binding = require_sha256(
        record.get("evidence_binding_sha256"), code=code, label="BSC-10 evidence binding"
    )
    if not secrets.compare_digest(binding, bsc10_record_commitment(record)):
        raise RunnerError(code, "BSC-10 evidence binding does not match the record")
    return record


def run_bsc10_adapter(
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
        BSC10_CASE_ID,
        "--role-id",
        str(expected["role_id"]),
        "--case-descriptor-sha256",
        str(expected["case_descriptor_sha256"]),
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
            timeout=BSC10_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-10 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-10 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-10 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 64 * 1024:
        raise RunnerError("case_record_invalid", "BSC-10 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-10 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc10_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def run_bsc10_case(args: argparse.Namespace) -> int:
    case_descriptor = load_bsc10_case_descriptor()
    if args.runs != case_descriptor["minimum_runs"]:
        raise RunnerError("invalid_runs", "BSC-10 requires exactly one run per collection role")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-10 requires an opaque local rig alias")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-10 collection roles are atomic")
    role_descriptor = bsc10_role_descriptor(
        case_descriptor, production_replay=args.production_replay
    )
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError("untrusted_override", "authoritative BSC-10 forbids an untracked rig adapter")
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-10 physical execution remains blocked until a tracked rig adapter exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-10 test execution requires a mocked adapter")

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-10 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-10 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc10_hardware(
        args, Path(args.pio_command), case_descriptor
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-10 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-10 serial endpoint is not present")

    role_id = role_descriptor["role_id"]
    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc10-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / f"{run_id}-{role_id}"
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-10 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc10-{secrets.token_hex(16)}"
    attempt_id = f"attempt-{secrets.token_hex(16)}"
    descriptor_sha = bsc10_descriptor_commitment(case_descriptor)
    expected: dict[str, object] = {
        "case_id": BSC10_CASE_ID,
        "role_id": role_id,
        "session_id": session_id,
        "attempt_id": attempt_id,
        "target_sha": git_state.head_sha,
        "dut_alias": args.board,
        "rig_alias": args.rig,
        "execution_mode": "simulated",
        "hardware_observed": False,
        "case_descriptor": case_descriptor,
        "case_descriptor_sha256": descriptor_sha,
        "role_descriptor": role_descriptor,
        "production_replay": args.production_replay,
    }
    require_unchanged_git_state(repository, git_state)
    record = run_bsc10_adapter(
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
        "run_kind": "bug-squash-bsc10-wifi-enable-transaction",
        "case_id": BSC10_CASE_ID,
        "collection_role": role_id,
        "case_descriptor_sha256": descriptor_sha,
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
            case_drivers.get_case_driver(BSC10_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": case_descriptor["minimum_runs"],
        "runs_completed": 1,
        "production_replay_required": bool(
            case_descriptor["production_replay_required"] and not args.production_replay
        ),
        "firmware_target": {
            "environment": firmware["environment"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
            "build_kind": firmware["build_kind"],
        },
        "evidence_binding_sha256": record["evidence_binding_sha256"],
        "artifact_sha256": {
            "adapter_record": sha256_file(attempt_path),
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc10.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc10.rig-attestation.v1", rig_attestation
            ),
        },
    }
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def run_registered_case_foundation(args: argparse.Namespace, case_id: str) -> int:
    """Fail closed at the tracked rig boundary for a registered case.

    These entrypoints are deliberately real, typed dispatch boundaries rather
    than aliases to another case.  They validate the profile-owned invocation
    shape before refusing physical mutation until that case's tracked rig
    adapter is present.  This keeps unavailable hardware from being mistaken
    for an unavailable or substituted driver.
    """

    profile, profile_errors = qualification.load_pinned_profile()
    if profile is None or profile_errors:
        raise RunnerError("qualification_profile_invalid", "pinned qualification profile is invalid")
    case_contract = next(
        (candidate for candidate in profile["required_cases"] if candidate["id"] == case_id),
        None,
    )
    if case_contract is None:
        raise RunnerError("case_driver_contract_invalid", "registered case is absent from the pinned profile")
    if args.runs != case_contract["minimum_runs"]:
        raise RunnerError(
            "invalid_runs",
            f"{case_id} requires exactly {case_contract['minimum_runs']} run(s)",
        )
    if args.rig is None:
        raise RunnerError("rig_alias_required", f"{case_id} requires an opaque local rig alias")
    if args.production_replay and not case_contract["production_replay_required"]:
        raise RunnerError("unsupported_mode", f"{case_id} has no production-replay role")
    if not args.production_replay and case_contract["scenario"]["vbus_isolation_required"]:
        if not args.ack_vbus_isolated:
            raise RunnerError(
                "operator_preconditions_incomplete",
                f"{case_id} requires explicit VBUS-isolation acknowledgement",
            )
    if args.case_adapter is not None:
        raise RunnerError(
            "untrusted_override",
            f"authoritative {case_id} forbids an untracked rig adapter",
        )
    raise RunnerError(
        "case_rig_adapter_unavailable",
        f"{case_id} physical execution remains blocked until its tracked rig adapter exists",
    )


def run_bsc06_case(args: argparse.Namespace) -> int:
    return run_registered_case_foundation(args, "BSC-06")


def run_bsc07_case(args: argparse.Namespace) -> int:
    return run_registered_case_foundation(args, "BSC-07")


def run_bsc08_case(args: argparse.Namespace) -> int:
    return run_registered_case_foundation(args, "BSC-08")


def run_bsc09_case(args: argparse.Namespace) -> int:
    return run_registered_case_foundation(args, "BSC-09")


def run_bsc12_case(args: argparse.Namespace) -> int:
    return run_registered_case_foundation(args, "BSC-12")


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
        "run_bsc04_case": run_bsc04_case,
        "run_bsc05_case": run_bsc05_case,
        "run_bsc06_case": run_bsc06_case,
        "run_bsc07_case": run_bsc07_case,
        "run_bsc08_case": run_bsc08_case,
        "run_bsc09_case": run_bsc09_case,
        "run_bsc10_case": run_bsc10_case,
        "run_bsc11_case": run_bsc11_case,
        "run_bsc12_case": run_bsc12_case,
        "run_bsc13_case": run_bsc13_case,
        "run_bsc14_case": run_bsc14_case,
        "run_bsc16_case": run_bsc16_case,
    }


def resolve_case_handler(
    driver: case_drivers.CaseDriver,
) -> Callable[[argparse.Namespace], int]:
    """Resolve a registry entry before any case-owned hardware mutation."""

    if driver != case_drivers.get_case_driver(driver.case_id):
        raise RunnerError(
            "case_driver_contract_invalid",
            "tracked case-driver descriptor was substituted",
        )
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
