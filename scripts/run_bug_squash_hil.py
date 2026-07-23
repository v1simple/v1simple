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
from dataclasses import dataclass, replace
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
from typing import Callable, Mapping, Sequence
import xml.etree.ElementTree as ET

import bug_squash_hil_adapter_protocol as adapter_protocol
import bug_squash_hil_case_drivers as case_drivers
import bug_squash_hil_rig_adapters as rig_adapters
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
BSC05_HIL_ENVIRONMENT = "waveshare-349-hil"
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
BSC05_FAULT_IDS = ("v1-notification-delay-once",)
BSC05_FAULT_EVENT_IDS = ("ready", "fired", "released")
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
BSC12_CASE_ID = "BSC-12"
BSC12_PROFILE_VERSION = 5
BSC12_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC12_REQUIRED_RUNS = 1
BSC12_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC12_DUT_CAPABILITIES = (
    "firmware-execution",
    "persistence",
    "power-button",
    "serial",
)
BSC12_RIG_CAPABILITIES = (
    "artifact-capture",
    "bond-peer",
    "power-control",
    "sd-media",
    "utc-time-source",
    "vbus-isolation",
    "wake-input-control",
)
BSC12_STIMULUS_IDS = (
    "begin-portable-shutdown",
    "assert-wake-input",
    "mutate-setting",
    "mutate-bond",
    "wait-for-writers",
    "force-reset",
)
BSC12_BARRIER_IDS = (
    "wake-input-asserted-during-handoff",
    "writers-completed",
)
BSC12_FACT_IDS = (
    "poweroff-returned-false",
    "disconnected-screen-restored",
    "session-marker-unclean-after-reset",
    "settings-writer-count",
    "bond-writer-count",
    "setting-survived-reset",
    "bond-survived-reset",
    "real-rtc-wake-input-observed",
)
BSC12_CAPTURE_COMMITMENTS = (
    "firmware_build_sha256",
    "persistence_after_sha256",
    "persistence_before_sha256",
    "power_reset_trace_sha256",
    "serial_log_sha256",
    "shutdown_timeline_sha256",
    "wake_input_trace_sha256",
)
BSC12_CAPTURE_ROLE_FIELDS = tuple(
    (artifact.role, artifact.filename, commitment)
    for artifact, commitment in zip(
        rig_adapters.BSC12_RAW_ARTIFACTS,
        BSC12_CAPTURE_COMMITMENTS,
        strict=True,
    )
)
BSC12_WRITER_SOURCES = (
    ("settings", "deferred-settings-backup-writer"),
    ("bond", "ble-bond-backup-writer"),
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
BSC09_CASE_ID = "BSC-09"
BSC09_PROFILE_ID = "bug-squash-hil-v1"
BSC09_PROFILE_VERSION = 5
BSC09_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC09_REQUIRED_RUNS = 3
BSC09_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC09_MAX_RECORD_BYTES = 512 * 1024
BSC09_DUT_CAPABILITIES = (
    "firmware-execution",
    "maintenance-mode",
    "serial",
    "wifi-scan",
)
BSC09_RIG_CAPABILITIES = (
    "artifact-capture",
    "browser-client",
    "multiple-access-points",
    "utc-time-source",
)
BSC09_STIMULUS_IDS = (
    "start-maintenance-autoconnect",
    "open-scan-modal",
    "close-scan-modal",
    "reopen-scan-modal",
    "drop-one-status-poll",
    "retry-status-poll",
)
BSC09_BARRIER_IDS = ("scan-overlap-observed",)
BSC09_FACT_IDS = (
    "maintenance-snapshot-stable",
    "ui-snapshot-stable",
    "consumer-generations-isolated",
    "retry-succeeded",
    "spinner-terminated",
    "wifi-mode-settled",
    "accumulating-scan-heap-loss-observed",
)
BSC09_CAPTURE_COMMITMENTS = (
    "browser_projection_sha256",
    "case_observation_sha256",
    "firmware_build_sha256",
    "health_projection_sha256",
    "heap_projection_sha256",
    "wifi_mode_projection_sha256",
    "wifi_scan_projection_sha256",
)
BSC09_RAW_CAPTURE_FIELDS = {
    "browser-projection": "browser_projection_sha256",
    "case-observation": "case_observation_sha256",
    "firmware-build": "firmware_build_sha256",
    "health-projection": "health_projection_sha256",
    "heap-projection": "heap_projection_sha256",
    "wifi-mode-projection": "wifi_mode_projection_sha256",
    "wifi-scan-projection": "wifi_scan_projection_sha256",
}
BSC09_LIFECYCLE_EVENTS = (
    ("request-started", "maintenance", 2, 0, True, False, False),
    ("request-joined", "ui", 3, 0, True, False, False),
    ("request-joined", "ui", 3, 0, True, False, False),
    ("request-joined", "ui", 3, 0, True, False, False),
    ("harvest-completed", "none", 0, 3, False, True, False),
    ("snapshot-read", "maintenance", 0, 3, False, False, False),
    ("snapshot-read", "ui", 0, 3, False, False, False),
)
BSC09_BROWSER_ACTIONS = (
    ("initial-post", "POST", "/api/wifi/scan", 200, True, True, True, False, True),
    ("close-modal", "LOCAL", "", 0, False, False, False, False, False),
    ("reopen-post", "POST", "/api/wifi/scan", 200, True, True, True, False, True),
    ("dropped-poll", "GET", "/api/wifi/scan", 0, False, False, False, True, False),
    ("stale-poll-response", "GET", "/api/wifi/scan", 200, False, False, False, True, False),
    ("retry-post", "POST", "/api/wifi/scan", 200, True, True, True, False, True),
    ("resumed-poll", "GET", "/api/wifi/scan", 200, True, True, True, False, True),
    ("terminal-poll", "GET", "/api/wifi/scan", 200, False, True, False, False, True),
)
BSC06_CASE_ID = "BSC-06"
BSC06_HIL_ENVIRONMENT = "waveshare-349-hil"
BSC06_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC06_REQUIRED_RUNS = 3
BSC06_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC06_NEGATIVE_ACK_MAX_MS = 5_000
BSC06_BARRIER_MAX_HOLD_MS = 1_000
BSC06_PROFILE_FAULT_ID = "transport-ready-barrier"
BSC06_CONTROLLER_FAULT_ID = "obd-transport-operation-barrier-once"
BSC06_FAULT_ID_MAP = {BSC06_PROFILE_FAULT_ID: BSC06_CONTROLLER_FAULT_ID}
BSC06_HIL_EVENT_REASONS = {
    "ready": "polling_write_after_epoch_claim",
    "fired": "transport_owner_barrier_active",
    "cancellation": "newer_cancellation_epoch_suppressed_write",
    "link-down": "matching_link_down_suppressed_write",
}
BSC06_DUT_CAPABILITIES = (
    "firmware-execution",
    "obd-connectivity",
    "proxy-connectivity",
    "serial",
)
BSC06_RIG_CAPABILITIES = (
    "artifact-capture",
    "obd-peer",
    "proxy-client",
    "utc-time-source",
    "v1-peer",
)
BSC06_FAULT_RACES = (
    ("proxy-attach", "race-proxy-attach", "cancellation"),
    ("adapter-loss", "race-adapter-loss", "link-down"),
    ("forget-device", "race-forget-device", "cancellation"),
    ("shutdown", "race-shutdown", "cancellation"),
)
BSC06_PRODUCTION_RACES = BSC06_FAULT_RACES[:2]
BSC06_FAULT_FACTS = {
    "post-cancel-gatt-observed",
    "link-down-before-handle-retire",
    "negative-ack-delay-ms",
    "clean-reconnect-count",
    "heap-corruption-observed",
    "barrier-generation-matched",
}
BSC06_PRODUCTION_FACTS = {
    "transport-ownership-preserved",
    "clean-reconnect-succeeded",
    "hil-fault-control-active",
}
BSC06_CAPTURE_COMMITMENTS = (
    "build_evidence_sha256",
    "control_queue_timeline_sha256",
    "obd_race_timeline_sha256",
    "panic_summary_sha256",
    "serial_log_sha256",
)
BSC07_CASE_ID = "BSC-07"
BSC07_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC07_REQUIRED_RUNS = 1
BSC07_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC07_VOLTAGE_REFRESH_MAX_MS = 10_000
BSC07_POWER_HOLD_MS = 2_000
BSC07_CRITICAL_GRACE_MIN_MS = 4_500
BSC07_CRITICAL_GRACE_MAX_MS = 6_500
BSC07_HEALTH_SAMPLE_MAX_AGE_MS = 1_000
BSC07_DUT_CAPABILITIES = (
    "battery-monitor",
    "firmware-execution",
    "maintenance-mode",
    "power-button",
    "serial",
)
BSC07_RIG_CAPABILITIES = (
    "ap-traffic",
    "artifact-capture",
    "power-control",
    "utc-time-source",
    "vbus-isolation",
)
BSC07_STIMULUS_IDS = (
    "maintenance-boot",
    "apply-ap-load",
    "change-battery-voltage",
    "hold-power-button",
    "apply-critical-voltage",
)
BSC07_CAPTURE_COMMITMENTS = (
    "ap_traffic_sha256",
    "build_evidence_sha256",
    "power_timeline_sha256",
    "reset_summary_sha256",
    "serial_log_sha256",
    "ui_health_sha256",
)
BSC07_CAPTURE_ROLE_BY_FIELD = {
    "ap_traffic_sha256": "ap-traffic",
    "build_evidence_sha256": "firmware-build",
    "power_timeline_sha256": "power-timeline",
    "reset_summary_sha256": "reset-summary",
    "serial_log_sha256": "serial-log",
    "ui_health_sha256": "ui-health",
}
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
BSC08_CASE_ID = "BSC-08"
BSC08_PRODUCTION_ENVIRONMENT = "waveshare-349"
BSC08_REQUIRED_RUNS = 3
BSC08_ADAPTER_TIMEOUT_SECONDS = 1_800
BSC08_DUT_CAPABILITIES = (
    "firmware-execution",
    "proxy-connectivity",
    "serial",
    "v1-connectivity",
)
BSC08_RIG_CAPABILITIES = (
    "artifact-capture",
    "proxy-client",
    "utc-time-source",
    "v1-peer",
)
BSC08_STIMULUS_IDS = (
    "start-bidirectional-stream",
    "disable-proxy",
    "reenable-proxy",
    "send-old-epoch-traffic",
    "send-fresh-epoch-traffic",
)
BSC08_BARRIER_IDS = ("active-callback-observed", "release-opportunity-observed")
BSC08_FACT_IDS = (
    "queue-index-corruption-observed",
    "heap-corruption-observed",
    "old-epoch-forwarded",
    "deferred-release-opportunities",
    "fresh-bidirectional-traffic-resumed",
    "monotonic-heap-loss-observed",
)
BSC08_SNAPSHOT_PHASES = (
    "baseline",
    "streaming",
    "disabled",
    "reenabled",
    "old-traffic",
    "fresh-traffic",
    "final",
)
BSC08_RAW_ARTIFACT_ROLES = tuple(artifact.role for artifact in rig_adapters.BSC08_RAW_ARTIFACTS)
BSC08_CAPTURE_BINDING_DOMAIN = "v1simple.bsc08.source-capture-binding.v1"
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


@dataclass(frozen=True)
class RigAdapterAdmission:
    adapter: rig_adapters.RigAdapter
    simulated: bool
    git_state: GitState | None
    source_sha256: str | None


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


def verify_tracked_rig_adapter_source(
    repository: Path,
    git_state: GitState,
    adapter: rig_adapters.RigAdapter,
) -> str:
    """Bind an implemented adapter's worktree bytes to its target-commit blob."""

    try:
        rig_adapters.validate_adapter_descriptor(adapter)
    except rig_adapters.RigAdapterContractError as exc:
        raise RunnerError("adapter_contract_invalid", "rig-adapter descriptor is invalid") from exc
    if not adapter.implemented or adapter.source_path is None:
        raise RunnerError("case_rig_adapter_unavailable", "tracked rig adapter is unavailable")
    source_path = repository / adapter.source_path
    require_no_symlink_components(source_path, boundary=repository)
    try:
        before = os.stat(source_path, follow_symlinks=False)
    except OSError as exc:
        raise RunnerError("adapter_source_invalid", "tracked rig-adapter source is unavailable") from exc
    if (
        not stat.S_ISREG(before.st_mode)
        or before.st_nlink != 1
        or not 1 <= before.st_size <= 4 * 1024 * 1024
    ):
        raise RunnerError("adapter_source_invalid", "tracked rig-adapter source is not a safe file")
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise RunnerError("adapter_source_invalid", "safe source opening is unavailable")
    flags = os.O_RDONLY | nofollow
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    descriptor = -1
    try:
        descriptor = os.open(source_path, flags)
        opened = os.fstat(descriptor)
        before_identity = (before.st_dev, before.st_ino, before.st_mode, before.st_nlink, before.st_size)
        opened_identity = (
            opened.st_dev,
            opened.st_ino,
            opened.st_mode,
            opened.st_nlink,
            opened.st_size,
        )
        if opened_identity != before_identity:
            raise RunnerError("adapter_source_changed", "rig-adapter source changed before hashing")
        digest = hashlib.sha256()
        worktree_bytes = bytearray()
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            worktree_bytes.extend(chunk)
            if len(worktree_bytes) > 4 * 1024 * 1024:
                raise RunnerError("adapter_source_invalid", "rig-adapter source is oversized")
            digest.update(chunk)
        after = os.fstat(descriptor)
        after_identity = (
            after.st_dev,
            after.st_ino,
            after.st_mode,
            after.st_nlink,
            after.st_size,
        )
        if after_identity != opened_identity or len(worktree_bytes) != after.st_size:
            raise RunnerError("adapter_source_changed", "rig-adapter source changed while hashing")
    except RunnerError:
        raise
    except OSError as exc:
        raise RunnerError("adapter_source_invalid", "rig-adapter source could not be hashed") from exc
    finally:
        if descriptor >= 0:
            try:
                os.close(descriptor)
            except OSError:
                pass

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
    git_prefix = [
        str(AUTHORITATIVE_GIT),
        "-C",
        str(repository),
        "-c",
        "core.fsmonitor=false",
    ]
    tree = subprocess.run(
        [*git_prefix, "ls-tree", "-z", git_state.head_sha, "--", adapter.source_path],
        cwd=repository,
        env=git_environment,
        capture_output=True,
        check=False,
    )
    if tree.returncode != 0:
        raise RunnerError("adapter_source_invalid", "rig-adapter source is not tracked")
    rows = [row for row in tree.stdout.split(b"\0") if row]
    if len(rows) != 1:
        raise RunnerError("adapter_source_invalid", "rig-adapter source is not uniquely tracked")
    try:
        metadata, tracked_path = rows[0].split(b"\t", 1)
        mode, object_type, blob_sha = metadata.split(b" ", 2)
    except ValueError as exc:
        raise RunnerError("adapter_source_invalid", "rig-adapter tree record is invalid") from exc
    if (
        mode not in {b"100644", b"100755"}
        or object_type != b"blob"
        or tracked_path.decode("utf-8", errors="strict") != adapter.source_path
        or re.fullmatch(rb"[0-9a-f]{40,64}", blob_sha) is None
    ):
        raise RunnerError("adapter_source_invalid", "rig-adapter tree identity is invalid")
    committed = subprocess.run(
        [*git_prefix, "cat-file", "blob", blob_sha.decode("ascii")],
        cwd=repository,
        env=git_environment,
        capture_output=True,
        check=False,
    )
    if committed.returncode != 0 or committed.stdout != bytes(worktree_bytes):
        raise RunnerError(
            "adapter_source_mismatch",
            "rig-adapter worktree source does not match the target commit",
        )
    try:
        final = os.stat(source_path, follow_symlinks=False)
    except OSError as exc:
        raise RunnerError("adapter_source_changed", "rig-adapter source changed after hashing") from exc
    final_identity = (final.st_dev, final.st_ino, final.st_mode, final.st_nlink, final.st_size)
    if final_identity != before_identity:
        raise RunnerError("adapter_source_changed", "rig-adapter source path changed while hashing")
    return digest.hexdigest()


def read_json(path: Path, label: str) -> object:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_json_keys,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("artifact_invalid", f"{label} could not be read as JSON") from exc


def read_json_bytes(content: bytes, label: str) -> object:
    """Parse already-verified artifact bytes without reopening their path."""

    try:
        return json.loads(
            content.decode("utf-8", errors="strict"),
            object_pairs_hook=reject_duplicate_json_keys,
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
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
        or row.get("fault_build_required") is not True
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
    expected_build_kind = "production" if production_replay else "hil-fault"
    expected_faults = [] if production_replay else list(BSC05_FAULT_IDS)
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
        or descriptor.get("build_kind") != expected_build_kind
        or descriptor.get("stimulus_ids") != list(expected_stimuli)
        or descriptor.get("fault_ids") != expected_faults
        or descriptor.get("barrier_ids") != expected_barriers
        or descriptor.get("vbus_isolation_required") is not False
        or reset.get("expected_kind") != "none"
        or type(reset.get("expected_count")) is not int
        or reset.get("expected_count") != 0
        or type(reset.get("unexpected_count")) is not int
        or reset.get("unexpected_count") != 0
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
    value: object, expected_ids: Sequence[str], *, maximum_elapsed_ms: int
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
            or elapsed < 0
            or elapsed > maximum_elapsed_ms
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
    maximum_elapsed_ms: int,
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
    if any(
        type(timeline.get(field)) is not int
        or timeline[field] < 0
        or timeline[field] > maximum_elapsed_ms
        for field in integer_fields
    ):
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
    maximum_elapsed_ms: int,
) -> dict[str, object] | None:
    code = "case_record_invalid"
    if production_replay:
        if value != []:
            raise RunnerError(code, "BSC-05 production replay contains barrier evidence")
        return None
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
        or ready < 0
        or ready > maximum_elapsed_ms
        or released < 0
        or released > maximum_elapsed_ms
        or ready < timeline["fragment_started_elapsed_ms"]
        or ready > timeline["disconnect_elapsed_ms"]
        or released < timeline["old_callback_release_elapsed_ms"]
        or released > timeline["old_callback_rejected_elapsed_ms"]
        or barrier.get("old_generation") != timeline["old_generation"]
        or barrier.get("new_generation") != timeline["new_generation"]
        or barrier.get("timed_out") is not False
    ):
        raise RunnerError(code, "BSC-05 callback barrier identity or timing is invalid")
    return barrier


def validate_bsc05_fault_lifecycle(
    value: object,
    *,
    production_replay: bool,
    timeline: Mapping[str, object],
    barrier: Mapping[str, object] | None,
    maximum_elapsed_ms: int,
) -> None:
    code = "case_record_invalid"
    if production_replay:
        if value != []:
            raise RunnerError(code, "BSC-05 production replay contains HIL fault events")
        return
    if barrier is None or not isinstance(value, list) or len(value) != len(BSC05_FAULT_EVENT_IDS):
        raise RunnerError(code, "BSC-05 notification-delay fault lifecycle is incomplete")

    arm_sequence: int | None = None
    ready_sequence: int | None = None
    elapsed_by_id: dict[str, int] = {}
    expected_reasons = (
        "notification_copied_without_callback_pointer",
        "old_generation_notification_delayed",
        "new_session_rejected_old_generation_copy",
    )
    for sequence, (event, event_id) in enumerate(
        zip(value, BSC05_FAULT_EVENT_IDS, strict=True), start=1
    ):
        row = require_exact_object(
            event,
            {
                "id",
                "sequence",
                "elapsed_ms",
                "reason",
                "case_id",
                "fault_id",
                "arm_sequence",
                "ready_sequence",
                "generation",
                "phase",
                "old_generation",
                "new_generation",
                "characteristic_class",
                "old_session_closed_elapsed_ms",
                "new_session_opened_elapsed_ms",
                "wrong_generation_rejected",
            },
            code=code,
            label="BSC-05 notification-delay fault event",
        )
        elapsed = row.get("elapsed_ms")
        current_arm = row.get("arm_sequence")
        current_ready = row.get("ready_sequence")
        current_generation = row.get("generation")
        current_phase = row.get("phase")
        released_event = event_id == "released"
        if (
            row.get("id") != event_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed < 0
            or elapsed > maximum_elapsed_ms
            or row.get("reason") != expected_reasons[sequence - 1]
            or row.get("case_id") != BSC05_CASE_ID
            or row.get("fault_id") != BSC05_FAULT_IDS[0]
            or type(current_arm) is not int
            or current_arm <= 0
            or type(current_ready) is not int
            or current_ready <= 0
            or type(current_generation) is not int
            or current_generation != timeline["old_generation"]
            or type(current_phase) is not int
            or current_phase != 1
            or row.get("old_generation") != timeline["old_generation"]
            or row.get("new_generation")
            != (timeline["new_generation"] if released_event else 0)
            or row.get("characteristic_class") != "display"
            or row.get("old_session_closed_elapsed_ms")
            != (timeline["old_session_closed_elapsed_ms"] if released_event else 0)
            or row.get("new_session_opened_elapsed_ms")
            != (timeline["new_session_opened_elapsed_ms"] if released_event else 0)
            or row.get("wrong_generation_rejected") is not released_event
        ):
            raise RunnerError(code, "BSC-05 notification-delay fault event is invalid")
        if arm_sequence is None:
            arm_sequence = current_arm
        elif current_arm != arm_sequence:
            raise RunnerError(code, "BSC-05 notification-delay arm identity changed")
        if ready_sequence is None:
            ready_sequence = current_ready
        elif current_ready != ready_sequence:
            raise RunnerError(code, "BSC-05 notification-delay ready identity changed")
        elapsed_by_id[event_id] = elapsed

    ready = elapsed_by_id["ready"]
    fired = elapsed_by_id["fired"]
    released = elapsed_by_id["released"]
    if (
        ready != barrier["ready_elapsed_ms"]
        or not timeline["fragment_started_elapsed_ms"] <= ready <= fired <= timeline["disconnect_elapsed_ms"]
        or released != barrier["released_elapsed_ms"]
        or not timeline["old_callback_release_elapsed_ms"] <= released <= timeline["old_callback_rejected_elapsed_ms"]
    ):
        raise RunnerError(code, "BSC-05 notification-delay fault timing is invalid")


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


def validate_bsc05_resets(value: object, reset_contract: object) -> None:
    code = "case_record_invalid"
    contract = require_exact_object(
        reset_contract,
        {"expected_kind", "expected_count", "unexpected_count"},
        code=code,
        label="BSC-05 reset contract",
    )
    resets = require_exact_object(
        value,
        {"expected_kind", "planned", "observed", "unexpected"},
        code=code,
        label="BSC-05 reset evidence",
    )
    if (
        resets.get("expected_kind") != contract.get("expected_kind")
        or type(resets.get("planned")) is not int
        or resets.get("planned") != contract.get("expected_count")
        or type(resets.get("observed")) is not int
        or resets.get("observed") != contract.get("expected_count")
        or type(resets.get("unexpected")) is not int
        or resets.get("unexpected") != contract.get("unexpected_count")
    ):
        raise RunnerError(code, "BSC-05 reset evidence does not match the pinned descriptor")


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
            "fault_lifecycle",
            "resets",
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
    maximum_elapsed_ms = int((completed - started).total_seconds() * 1000)

    production_replay = role_descriptor.get("role_id") == "alert-generation-production-replay"
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
    expected_environment = (
        BSC05_PRODUCTION_ENVIRONMENT if production_replay else BSC05_HIL_ENVIRONMENT
    )
    expected_hil = not production_replay
    if (
        firmware.get("environment") != expected_environment
        or firmware.get("target_sha") != expected.get("target_sha")
        or firmware.get("hil_fault_control_active") is not expected_hil
        or firmware.get("build_kind") != role_descriptor.get("build_kind")
    ):
        raise RunnerError(code, "BSC-05 firmware role or target is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-05 firmware binary")

    stimuli = validate_bsc05_stimuli(
        record.get("stimuli"),
        role_descriptor["stimulus_ids"],
        maximum_elapsed_ms=maximum_elapsed_ms,
    )
    timeline = validate_bsc05_generation_timeline(
        record.get("generation_timeline"),
        production_replay=production_replay,
        stimuli=stimuli,
        maximum_elapsed_ms=maximum_elapsed_ms,
    )
    barrier = validate_bsc05_barriers(
        record.get("barriers"),
        production_replay=production_replay,
        timeline=timeline,
        maximum_elapsed_ms=maximum_elapsed_ms,
    )
    validate_bsc05_fault_lifecycle(
        record.get("fault_lifecycle"),
        production_replay=production_replay,
        timeline=timeline,
        barrier=barrier,
        maximum_elapsed_ms=maximum_elapsed_ms,
    )
    validate_bsc05_resets(record.get("resets"), role_descriptor.get("reset_contract"))
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
    generation_pairs: list[tuple[object, object]] = []
    for record in records:
        timeline = record.get("generation_timeline")
        assert isinstance(timeline, dict)
        generation_pairs.append((timeline.get("old_generation"), timeline.get("new_generation")))
    if len(set(generation_pairs)) != BSC05_REQUIRED_RUNS:
        raise RunnerError("case_runs_reused", "BSC-05 generation pairs must be distinct")


def resolve_bsc05_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
    case_descriptor: Mapping[str, object],
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    dut_capabilities = case_descriptor.get("required_dut_capabilities")
    rig_capabilities = case_descriptor.get("required_rig_capabilities")
    if (
        not isinstance(dut_capabilities, list)
        or not all(isinstance(capability, str) for capability in dut_capabilities)
        or not isinstance(rig_capabilities, list)
        or not all(isinstance(capability, str) for capability in rig_capabilities)
    ):
        raise RunnerError(
            "case_driver_contract_invalid",
            "BSC-05 capability descriptors are invalid",
        )
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
        required_capabilities=tuple(dut_capabilities),
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=tuple(rig_capabilities),
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
        args, Path(args.pio_command), case_descriptor
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


def load_bsc06_case_descriptor() -> dict[str, object]:
    profile, errors = qualification.load_pinned_profile()
    if profile is None or errors:
        raise RunnerError(
            "qualification_profile_invalid",
            "BSC-06 requires the exact pinned qualification profile",
        )
    matches = [case for case in profile["required_cases"] if case.get("id") == BSC06_CASE_ID]
    if len(matches) != 1:
        raise RunnerError("qualification_profile_invalid", "BSC-06 descriptor is unavailable")
    descriptor = matches[0]
    if (
        set(descriptor)
        != {
            "id",
            "minimum_runs",
            "fault_build_required",
            "production_replay_required",
            "required_dut_capabilities",
            "required_rig_capabilities",
            "scenario",
            "production_replay",
        }
        or descriptor.get("minimum_runs") != BSC06_REQUIRED_RUNS
        or descriptor.get("fault_build_required") is not True
        or descriptor.get("production_replay_required") is not True
        or descriptor.get("required_dut_capabilities") != list(BSC06_DUT_CAPABILITIES)
        or descriptor.get("required_rig_capabilities") != list(BSC06_RIG_CAPABILITIES)
    ):
        raise RunnerError("qualification_profile_invalid", "BSC-06 case contract drifted")
    bsc06_role_descriptor(descriptor, production_replay=False)
    bsc06_role_descriptor(descriptor, production_replay=True)
    return descriptor


def bsc06_role_descriptor(
    case_descriptor: Mapping[str, object], *, production_replay: bool
) -> dict[str, object]:
    key = "production_replay" if production_replay else "scenario"
    raw = case_descriptor.get(key)
    if not isinstance(raw, dict):
        raise RunnerError("qualification_profile_invalid", "BSC-06 role descriptor is missing")
    role = dict(raw)
    expected_stimuli = (
        ["start-obd-polling", "race-proxy-attach", "race-adapter-loss", "restore-obd-peer"]
        if production_replay
        else [
            "start-obd-polling",
            "race-proxy-attach",
            "race-adapter-loss",
            "race-forget-device",
            "race-shutdown",
            "restore-obd-peer",
        ]
    )
    expected_facts = BSC06_PRODUCTION_FACTS if production_replay else BSC06_FAULT_FACTS
    if (
        set(role)
        != {
            "role_id",
            "schema",
            "build_kind",
            "stimulus_ids",
            "fault_ids",
            "barrier_ids",
            "vbus_isolation_required",
            "reset_contract",
            "facts",
        }
        or role.get("role_id")
        != ("obd-transport-production-replay" if production_replay else "obd-transport-race-fault")
        or role.get("schema") != "case-observation-v1"
        or role.get("build_kind") != ("production" if production_replay else "hil-fault")
        or role.get("stimulus_ids") != expected_stimuli
        or role.get("fault_ids") != ([] if production_replay else [BSC06_PROFILE_FAULT_ID])
        or role.get("barrier_ids")
        != ([] if production_replay else ["connect-window-ready", "cancel-window-ready"])
        or role.get("vbus_isolation_required") is not False
        or role.get("reset_contract")
        != {"expected_kind": "none", "expected_count": 0, "unexpected_count": 0}
        or not isinstance(role.get("facts"), list)
        or {fact.get("id") for fact in role["facts"] if isinstance(fact, dict)} != expected_facts
    ):
        raise RunnerError("qualification_profile_invalid", "BSC-06 pinned role contract drifted")
    return role


def bsc06_descriptor_commitment(case_descriptor: Mapping[str, object]) -> str:
    return canonical_case_commitment("v1simple.bsc06.case-descriptor.v1", case_descriptor)


def bsc06_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc06.case-record.v1", committed)


def validate_bsc06_stimuli(
    value: object, role_descriptor: Mapping[str, object], *, duration_ms: int
) -> dict[str, int]:
    expected_ids = role_descriptor["stimulus_ids"]
    assert isinstance(expected_ids, list)
    if not isinstance(value, list) or len(value) != len(expected_ids):
        raise RunnerError("case_record_invalid", "BSC-06 stimulus coverage is incomplete")
    elapsed_by_id: dict[str, int] = {}
    previous = -1
    for sequence, (raw, event_id) in enumerate(zip(value, expected_ids, strict=True), start=1):
        event = require_exact_object(
            raw,
            {"id", "sequence", "elapsed_ms", "result"},
            code="case_record_invalid",
            label="BSC-06 stimulus",
        )
        elapsed = event.get("elapsed_ms")
        if (
            event.get("id") != event_id
            or type(event.get("sequence")) is not int
            or event.get("sequence") != sequence
            or type(elapsed) is not int
            or not 0 <= elapsed <= duration_ms
            or elapsed <= previous
            or event.get("result") != "pass"
        ):
            raise RunnerError("case_record_invalid", "BSC-06 stimulus order or timing is invalid")
        elapsed_by_id[event_id] = elapsed
        previous = elapsed
    return elapsed_by_id


def validate_bsc06_hil_events(
    value: object,
    *,
    generation: int,
    release_cause: str,
    start_ms: int,
    completion_ms: int,
    cancellation_epoch_before: int,
    cancellation_epoch_after: int,
) -> None:
    if not isinstance(value, list) or len(value) != 3:
        raise RunnerError("case_record_invalid", "BSC-06 HIL lifecycle is incomplete")
    identity: tuple[int, int, int, int, int, int, int] | None = None
    previous = start_ms - 1
    for sequence, (raw, event_id) in enumerate(
        zip(value, ("ready", "fired", "released"), strict=True), start=1
    ):
        base_fields = {
            "hil_event",
            "reason",
            "case_id",
            "fault_id",
            "sequence",
            "elapsed_ms",
            "arm_sequence",
            "ready_sequence",
            "generation",
            "phase",
            "request_id",
            "dispatch_epoch",
            "operation",
            "runtime_state",
            "ready_timestamp_ms",
        }
        completion_fields = {
            "cancellation_epoch",
            "link_down_generation",
            "completion_timestamp_ms",
            "operation_suppressed",
            "controller_release_recorded",
        }
        event = require_exact_object(
            raw,
            base_fields | (completion_fields if event_id == "released" else set()),
            code="case_record_invalid",
            label="BSC-06 HIL event",
        )
        elapsed = event.get("elapsed_ms")
        ready_timestamp_ms = event.get("ready_timestamp_ms")
        current_identity = tuple(
            event.get(field)
            for field in (
                "arm_sequence",
                "ready_sequence",
                "generation",
                "phase",
                "request_id",
                "dispatch_epoch",
                "ready_timestamp_ms",
            )
        )
        expected_reason = (
            BSC06_HIL_EVENT_REASONS[release_cause]
            if event_id == "released"
            else BSC06_HIL_EVENT_REASONS[event_id]
        )
        if (
            event.get("hil_event") != event_id
            or event.get("reason") != expected_reason
            or event.get("case_id") != BSC06_CASE_ID
            or event.get("fault_id") != BSC06_FAULT_ID_MAP[BSC06_PROFILE_FAULT_ID]
            or type(event.get("sequence")) is not int
            or event.get("sequence") != sequence
            or type(elapsed) is not int
            or elapsed <= previous
            or elapsed > completion_ms
            or any(type(item) is not int or item <= 0 for item in current_identity[:-1])
            or type(ready_timestamp_ms) is not int
            or not 0 <= ready_timestamp_ms <= 0xFFFFFFFF
            or current_identity[2] != generation
            or current_identity[3] != 1
            or event.get("operation") != "write"
            or event.get("runtime_state") != "polling"
        ):
            raise RunnerError("case_record_invalid", "BSC-06 HIL event identity or timing is invalid")
        typed_identity = tuple(int(item) for item in current_identity)
        if identity is None:
            identity = typed_identity
        elif typed_identity != identity:
            raise RunnerError("case_record_invalid", "BSC-06 HIL identity changed inside a race")
        if event_id == "released":
            completion_timestamp_ms = event.get("completion_timestamp_ms")
            cancellation_epoch = event.get("cancellation_epoch")
            link_down_generation = event.get("link_down_generation")
            expected_link_down = generation if release_cause == "link-down" else 0
            if (
                type(cancellation_epoch) is not int
                or cancellation_epoch != cancellation_epoch_after
                or type(link_down_generation) is not int
                or link_down_generation != expected_link_down
                or type(completion_timestamp_ms) is not int
                or not 0 <= completion_timestamp_ms <= 0xFFFFFFFF
                or (completion_timestamp_ms - ready_timestamp_ms) % (1 << 32)
                > BSC06_BARRIER_MAX_HOLD_MS
                or event.get("operation_suppressed") is not True
                or event.get("controller_release_recorded") is not True
            ):
                raise RunnerError("case_record_invalid", "BSC-06 HIL completion evidence is invalid")
        previous = elapsed


def validate_bsc06_barriers(
    value: object,
    *,
    hil_events: Sequence[Mapping[str, object]],
    start_ms: int,
    completion_ms: int,
) -> None:
    if not isinstance(value, list) or len(value) != 2:
        raise RunnerError("case_record_invalid", "BSC-06 barrier coverage is incomplete")
    expected_ids = ("connect-window-ready", "cancel-window-ready")
    expected_times = (
        (hil_events[0]["elapsed_ms"], hil_events[1]["elapsed_ms"]),
        (hil_events[1]["elapsed_ms"], hil_events[2]["elapsed_ms"]),
    )
    previous_release = start_ms
    for sequence, (raw, barrier_id, (ready_expected, released_expected)) in enumerate(
        zip(value, expected_ids, expected_times, strict=True), start=1
    ):
        barrier = require_exact_object(
            raw,
            {"id", "sequence", "ready_elapsed_ms", "released_elapsed_ms"},
            code="case_record_invalid",
            label="BSC-06 barrier",
        )
        ready_ms = barrier.get("ready_elapsed_ms")
        released_ms = barrier.get("released_elapsed_ms")
        if (
            barrier.get("id") != barrier_id
            or type(barrier.get("sequence")) is not int
            or barrier.get("sequence") != sequence
            or type(ready_ms) is not int
            or type(released_ms) is not int
            or ready_ms != ready_expected
            or released_ms != released_expected
            or ready_ms < previous_release
            or released_ms < ready_ms
            or released_ms > completion_ms
        ):
            raise RunnerError("case_record_invalid", "BSC-06 barrier order or timing is invalid")
        previous_release = released_ms


def validate_bsc06_race_windows(
    value: object,
    *,
    production_replay: bool,
    stimuli: Mapping[str, int],
    duration_ms: int,
) -> list[dict[str, object]]:
    expected_races = BSC06_PRODUCTION_RACES if production_replay else BSC06_FAULT_RACES
    if not isinstance(value, list) or len(value) != len(expected_races):
        raise RunnerError("case_record_invalid", "BSC-06 race-window coverage is incomplete")
    windows: list[dict[str, object]] = []
    generations: set[int] = set()
    restore_ms = stimuli["restore-obd-peer"]
    previous_completion_ms = -1
    for sequence, (raw, (race_id, stimulus_id, release_cause)) in enumerate(
        zip(value, expected_races, strict=True), start=1
    ):
        window = require_exact_object(
            raw,
            {
                "race_id",
                "sequence",
                "stimulus_id",
                "release_cause",
                "start_elapsed_ms",
                "cancellation_elapsed_ms",
                "last_gatt_operation_elapsed_ms",
                "negative_ack_elapsed_ms",
                "link_down_elapsed_ms",
                "handle_retired_elapsed_ms",
                "completion_elapsed_ms",
                "captured_generation",
                "cancellation_epoch_before",
                "cancellation_epoch_after",
                "callback_link_down_generation",
                "callback_confirmed_link_down",
                "post_cancel_gatt_observed",
                "negative_ack_delay_ms",
                "reconnect_count",
                "heap_corruption_observed",
                "barrier_ready",
                "barrier_released",
                "barriers",
                "hil_events",
            },
            code="case_record_invalid",
            label="BSC-06 race window",
        )
        start_ms = window.get("start_elapsed_ms")
        cancel_ms = window.get("cancellation_elapsed_ms")
        last_gatt_ms = window.get("last_gatt_operation_elapsed_ms")
        nack_ms = window.get("negative_ack_elapsed_ms")
        link_down_ms = window.get("link_down_elapsed_ms")
        retired_ms = window.get("handle_retired_elapsed_ms")
        completion_ms = window.get("completion_elapsed_ms")
        generation = window.get("captured_generation")
        epoch_before = window.get("cancellation_epoch_before")
        epoch_after = window.get("cancellation_epoch_after")
        delay_ms = window.get("negative_ack_delay_ms")
        if (
            window.get("race_id") != race_id
            or window.get("stimulus_id") != stimulus_id
            or window.get("release_cause") != release_cause
            or type(window.get("sequence")) is not int
            or window.get("sequence") != sequence
            or any(
                type(item) is not int
                for item in (
                    start_ms,
                    cancel_ms,
                    last_gatt_ms,
                    nack_ms,
                    link_down_ms,
                    retired_ms,
                    completion_ms,
                    generation,
                    epoch_before,
                    epoch_after,
                    delay_ms,
                )
            )
            or any(
                not 0 <= item <= duration_ms
                for item in (
                    start_ms,
                    cancel_ms,
                    last_gatt_ms,
                    nack_ms,
                    link_down_ms,
                    retired_ms,
                    completion_ms,
                )
            )
            or start_ms != stimuli[stimulus_id]
            or not start_ms <= last_gatt_ms <= cancel_ms
            or start_ms <= previous_completion_ms
            or not (start_ms <= cancel_ms <= nack_ms <= completion_ms <= restore_ms)
            or not (cancel_ms <= link_down_ms <= retired_ms <= completion_ms)
            or delay_ms != nack_ms - cancel_ms
            or not 0 <= delay_ms <= BSC06_NEGATIVE_ACK_MAX_MS
            or generation <= 0
            or generation in generations
            or epoch_before < 0
            or (
                epoch_after <= epoch_before
                if release_cause == "cancellation"
                else epoch_after != epoch_before
            )
            or type(window.get("callback_link_down_generation")) is not int
            or window.get("callback_link_down_generation") != generation
            or window.get("callback_confirmed_link_down") is not True
            or window.get("post_cancel_gatt_observed") is not False
            or window.get("reconnect_count") != 1
            or type(window.get("reconnect_count")) is not int
            or window.get("heap_corruption_observed") is not False
        ):
            raise RunnerError("case_record_invalid", "BSC-06 transport race evidence is invalid")
        generations.add(generation)
        previous_completion_ms = completion_ms
        if production_replay:
            if (
                window.get("barrier_ready") is not False
                or window.get("barrier_released") is not False
                or window.get("barriers") != []
                or window.get("hil_events") != []
            ):
                raise RunnerError("case_record_invalid", "BSC-06 production replay contains HIL evidence")
        else:
            if window.get("barrier_ready") is not True or window.get("barrier_released") is not True:
                raise RunnerError("case_record_invalid", "BSC-06 race lacks its barrier lifecycle")
            validate_bsc06_hil_events(
                window.get("hil_events"),
                generation=generation,
                release_cause=release_cause,
                start_ms=start_ms,
                completion_ms=completion_ms,
                cancellation_epoch_before=epoch_before,
                cancellation_epoch_after=epoch_after,
            )
            hil_events = window.get("hil_events")
            assert isinstance(hil_events, list)
            validate_bsc06_barriers(
                window.get("barriers"),
                hil_events=hil_events,
                start_ms=start_ms,
                completion_ms=completion_ms,
            )
        windows.append(window)
    return windows


def validate_bsc06_facts(
    value: object, *, production_replay: bool, windows: Sequence[Mapping[str, object]]
) -> None:
    facts = require_exact_object(
        value,
        BSC06_PRODUCTION_FACTS if production_replay else BSC06_FAULT_FACTS,
        code="case_record_invalid",
        label="BSC-06 facts",
    )
    if production_replay:
        valid = (
            facts.get("transport-ownership-preserved") is True
            and facts.get("clean-reconnect-succeeded") is True
            and facts.get("hil-fault-control-active") is False
        )
    else:
        max_delay = max(int(window["negative_ack_delay_ms"]) for window in windows)
        valid = (
            facts.get("post-cancel-gatt-observed") is False
            and facts.get("link-down-before-handle-retire") is True
            and type(facts.get("negative-ack-delay-ms")) is int
            and facts.get("negative-ack-delay-ms") == max_delay
            and type(facts.get("clean-reconnect-count")) is int
            and facts.get("clean-reconnect-count") == 1
            and facts.get("heap-corruption-observed") is False
            and facts.get("barrier-generation-matched") is True
        )
    if not valid:
        raise RunnerError("case_record_invalid", "BSC-06 aggregate facts are invalid")


def validate_bsc06_reset_observation(
    value: object, role_descriptor: Mapping[str, object]
) -> None:
    observation = require_exact_object(
        value,
        {
            "expected_kind",
            "expected_count",
            "unexpected_count",
            "panic_observed",
            "watchdog_reset_observed",
            "load_prohibited_observed",
        },
        code="case_record_invalid",
        label="BSC-06 reset observation",
    )
    reset_contract = role_descriptor.get("reset_contract")
    assert isinstance(reset_contract, dict)
    if (
        observation.get("expected_kind") != reset_contract.get("expected_kind")
        or type(observation.get("expected_count")) is not int
        or observation.get("expected_count") != reset_contract.get("expected_count")
        or type(observation.get("unexpected_count")) is not int
        or observation.get("unexpected_count") != reset_contract.get("unexpected_count")
        or type(observation.get("panic_observed")) is not bool
        or observation.get("panic_observed") is not False
        or type(observation.get("watchdog_reset_observed")) is not bool
        or observation.get("watchdog_reset_observed") is not False
        or type(observation.get("load_prohibited_observed")) is not bool
        or observation.get("load_prohibited_observed") is not False
    ):
        raise RunnerError("case_record_invalid", "BSC-06 reset observation violates the pinned contract")


def validate_bsc06_adapter_record(
    payload: object,
    *,
    expected: Mapping[str, object],
    command_started: datetime | None = None,
    command_completed: datetime | None = None,
) -> dict[str, object]:
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
            "case_descriptor_sha256",
            "descriptor",
            "firmware",
            "stimuli",
            "race_windows",
            "reset_observation",
            "facts",
            "capture_commitments",
            "evidence_binding_sha256",
        },
        code="case_record_invalid",
        label="BSC-06 adapter record",
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
        "case_descriptor_sha256",
    ):
        if record.get(field) != expected.get(field):
            raise RunnerError("case_record_invalid", f"BSC-06 {field} does not match the runner")
    if (
        type(record.get("schema_version")) is not int
        or record.get("schema_version") != 1
        or type(record.get("run_index")) is not int
        or not 1 <= record["run_index"] <= BSC06_REQUIRED_RUNS
        or type(record.get("hardware_observed")) is not bool
    ):
        raise RunnerError("case_record_invalid", "BSC-06 record identity types are invalid")
    started = parse_runner_utc(record.get("started_at_utc"), code="case_record_invalid", label="BSC-06 start")
    completed = parse_runner_utc(
        record.get("completed_at_utc"), code="case_record_invalid", label="BSC-06 completion"
    )
    if completed < started:
        raise RunnerError("case_record_invalid", "BSC-06 completion predates its start")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError("case_record_invalid", "BSC-06 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=0):
        raise RunnerError("case_record_invalid", "BSC-06 physical record postdates adapter execution")
    duration_ms = int((completed - started).total_seconds() * 1000)

    case_descriptor = expected["case_descriptor"]
    assert isinstance(case_descriptor, dict)
    production_replay = record.get("role_id") == "obd-transport-production-replay"
    role_descriptor = bsc06_role_descriptor(case_descriptor, production_replay=production_replay)
    descriptor = require_exact_object(
        record.get("descriptor"),
        set(role_descriptor),
        code="case_record_invalid",
        label="BSC-06 descriptor",
    )
    if descriptor != role_descriptor:
        raise RunnerError("case_record_invalid", "BSC-06 descriptor does not match the pinned role")

    firmware = require_exact_object(
        record.get("firmware"),
        {"environment", "build_kind", "target_sha", "binary_sha256", "hil_fault_control_active"},
        code="case_record_invalid",
        label="BSC-06 firmware",
    )
    expected_environment = BSC06_PRODUCTION_ENVIRONMENT if production_replay else BSC06_HIL_ENVIRONMENT
    expected_build = "production" if production_replay else "hil-fault"
    hil_fault_control_active = firmware.get("hil_fault_control_active")
    if (
        firmware.get("environment") != expected_environment
        or firmware.get("build_kind") != expected_build
        or firmware.get("target_sha") != expected.get("target_sha")
        or type(hil_fault_control_active) is not bool
        or hil_fault_control_active is not (not production_replay)
    ):
        raise RunnerError("case_record_invalid", "BSC-06 firmware role is invalid")
    require_sha256(firmware.get("binary_sha256"), code="case_record_invalid", label="BSC-06 firmware")

    stimuli = validate_bsc06_stimuli(
        record.get("stimuli"), role_descriptor, duration_ms=duration_ms
    )
    windows = validate_bsc06_race_windows(
        record.get("race_windows"),
        production_replay=production_replay,
        stimuli=stimuli,
        duration_ms=duration_ms,
    )
    validate_bsc06_reset_observation(record.get("reset_observation"), role_descriptor)
    validate_bsc06_facts(record.get("facts"), production_replay=production_replay, windows=windows)
    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC06_CAPTURE_COMMITMENTS),
        code="case_record_invalid",
        label="BSC-06 capture commitments",
    )
    capture_hashes = [
        require_sha256(commitments[field], code="case_record_invalid", label=field)
        for field in BSC06_CAPTURE_COMMITMENTS
    ]
    if len(set(capture_hashes)) != len(capture_hashes):
        raise RunnerError("case_record_invalid", "BSC-06 evidence roles reused a capture")
    binding = require_sha256(
        record.get("evidence_binding_sha256"), code="case_record_invalid", label="BSC-06 binding"
    )
    if not secrets.compare_digest(binding, bsc06_record_commitment(record)):
        raise RunnerError("case_record_invalid", "BSC-06 evidence binding is invalid")
    return record


def validate_bsc06_distinct_runs(records: Sequence[Mapping[str, object]]) -> None:
    if len(records) != BSC06_REQUIRED_RUNS:
        raise RunnerError("case_runs_incomplete", "BSC-06 requires exactly three runs")
    for field in ("attempt_id", "evidence_binding_sha256"):
        if len({record.get(field) for record in records}) != BSC06_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", "BSC-06 run identities must be distinct")
    for field in ("firmware", "case_descriptor_sha256", "descriptor"):
        if any(record.get(field) != records[0].get(field) for record in records[1:]):
            raise RunnerError("case_runs_mixed", "BSC-06 runs changed their bound target")
    for field in BSC06_CAPTURE_COMMITMENTS:
        values = []
        for record in records:
            captures = record.get("capture_commitments")
            assert isinstance(captures, dict)
            values.append(captures[field])
        if len(set(values)) != BSC06_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", "BSC-06 run captures must be distinct")
    generations = [
        window["captured_generation"]
        for record in records
        for window in record["race_windows"]
        if isinstance(window, dict)
    ]
    if len(generations) != len(set(generations)):
        raise RunnerError("case_runs_reused", "BSC-06 race generations must be distinct")


def resolve_bsc06_hardware(
    args: argparse.Namespace, pio_executable: Path
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError("local_inventory_missing", "BSC-06 requires the ignored local hardware inventory")
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
        required_capabilities=BSC06_DUT_CAPABILITIES,
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=BSC06_RIG_CAPABILITIES,
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-06 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def run_bsc06_adapter(
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
        BSC06_CASE_ID,
        "--role-id",
        str(expected["role_id"]),
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
            timeout=BSC06_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-06 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-06 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-06 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 128 * 1024:
        raise RunnerError("case_record_invalid", "BSC-06 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-06 adapter output is not strict JSON") from exc
    physical = expected.get("execution_mode") == "physical"
    return validate_bsc06_adapter_record(
        payload,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def run_bsc06_case(args: argparse.Namespace) -> int:
    case_descriptor = load_bsc06_case_descriptor()
    production_replay = args.production_replay
    role_descriptor = bsc06_role_descriptor(case_descriptor, production_replay=production_replay)
    if args.runs != BSC06_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-06 requires exactly three runs per collection role")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-06 requires an opaque local rig alias")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-06 collection roles are atomic")
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError("untrusted_override", "authoritative BSC-06 forbids an untracked rig adapter")
        raise RunnerError(
            "case_rig_adapter_unavailable",
            "BSC-06 physical execution remains blocked until a tracked rig adapter exists",
        )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-06 test execution requires a mocked adapter")

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-06 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-06 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc06_hardware(
        args, Path(args.pio_command)
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-06 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-06 serial endpoint is not present")

    role_id = role_descriptor["role_id"]
    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc06-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / f"{run_id}-{role_id}"
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-06 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    descriptor_sha = bsc06_descriptor_commitment(case_descriptor)
    session_id = f"bsc06-{secrets.token_hex(16)}"
    records: list[dict[str, object]] = []
    run_artifacts: list[dict[str, object]] = []
    for run_index in range(1, BSC06_REQUIRED_RUNS + 1):
        attempt_id = f"attempt-{secrets.token_hex(16)}"
        expected: dict[str, object] = {
            "case_id": BSC06_CASE_ID,
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
        }
        require_unchanged_git_state(repository, git_state)
        record = run_bsc06_adapter(
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
    validate_bsc06_distinct_runs(records)
    firmware = records[0]["firmware"]
    assert isinstance(firmware, dict)
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc06-obd-transport-races",
        "case_id": BSC06_CASE_ID,
        "collection_role": role_id,
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
            case_drivers.get_case_driver(BSC06_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC06_REQUIRED_RUNS,
        "runs_completed": len(records),
        "production_replay_required": not production_replay,
        "race_roles": [race[0] for race in (BSC06_PRODUCTION_RACES if production_replay else BSC06_FAULT_RACES)],
        "firmware_target": {
            "environment": firmware["environment"],
            "build_kind": firmware["build_kind"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
        },
        "run_artifacts": run_artifacts,
        "artifact_sha256": {
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc06.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc06.rig-attestation.v1", rig_attestation
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
    admit_case_rig_adapter(
        args,
        case_contract=case_descriptor,
        role_id=str(role_descriptor["role_id"]),
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


def load_bsc07_case_descriptor() -> dict[str, object]:
    profile, errors = qualification.load_pinned_profile()
    if profile is None or errors or profile.get("profile_version") != 5:
        raise RunnerError(
            "qualification_profile_invalid",
            "pinned BSC-07 qualification descriptor is invalid",
        )
    descriptor = next(
        (entry for entry in profile["required_cases"] if entry.get("id") == BSC07_CASE_ID),
        None,
    )
    if not isinstance(descriptor, dict):
        raise RunnerError(
            "case_driver_contract_invalid",
            "BSC-07 is absent from the pinned qualification profile",
        )
    role = descriptor.get("scenario")
    expected_facts = [
        {"id": "voltage-refresh-delay-ms", "type": "integer", "minimum": 0, "maximum": 10000},
        {"id": "power-button-handled-under-ap-load", "type": "boolean", "expected": True},
        {"id": "critical-shutdown-grace-ms", "type": "integer", "minimum": 4500, "maximum": 6500},
        {"id": "critical-warning-observed", "type": "boolean", "expected": True},
        {"id": "ui-responsive-until-shutdown", "type": "boolean", "expected": True},
        {"id": "loop-stall-observed", "type": "boolean", "expected": False},
    ]
    if (
        set(descriptor)
        != {
            "id",
            "minimum_runs",
            "fault_build_required",
            "production_replay_required",
            "required_dut_capabilities",
            "required_rig_capabilities",
            "scenario",
            "production_replay",
        }
        or descriptor.get("minimum_runs") != BSC07_REQUIRED_RUNS
        or type(descriptor.get("minimum_runs")) is not int
        or descriptor.get("fault_build_required") is not False
        or descriptor.get("production_replay_required") is not False
        or descriptor.get("production_replay") is not None
        or descriptor.get("required_dut_capabilities") != list(BSC07_DUT_CAPABILITIES)
        or descriptor.get("required_rig_capabilities") != list(BSC07_RIG_CAPABILITIES)
        or not isinstance(role, dict)
        or set(role)
        != {
            "role_id",
            "schema",
            "build_kind",
            "stimulus_ids",
            "fault_ids",
            "barrier_ids",
            "vbus_isolation_required",
            "reset_contract",
            "facts",
        }
        or role.get("role_id") != "maintenance-power-safety"
        or role.get("schema") != "case-observation-v1"
        or role.get("build_kind") != "production"
        or role.get("stimulus_ids") != list(BSC07_STIMULUS_IDS)
        or role.get("fault_ids") != []
        or role.get("barrier_ids") != ["critical-voltage-grace"]
        or role.get("vbus_isolation_required") is not True
        or role.get("reset_contract")
        != {"expected_kind": "intentional-shutdown", "expected_count": 1, "unexpected_count": 0}
        or role.get("facts") != expected_facts
    ):
        raise RunnerError(
            "case_driver_contract_invalid",
            "BSC-07 profile-v5 contract drifted",
        )
    return descriptor


def bsc07_descriptor_commitment(case_descriptor: Mapping[str, object]) -> str:
    return canonical_case_commitment("v1simple.bsc07.case-descriptor.v1", case_descriptor)


def bsc07_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc07.case-record.v1", committed)


def bsc07_manifest_capture_commitments(
    manifest: object,
    *,
    expected_request_sha256: str,
) -> dict[str, str]:
    payload = require_exact_object(
        manifest,
        {
            "schema_version",
            "protocol_version",
            "request_commitment_sha256",
            "artifacts",
            "manifest_commitment_sha256",
        },
        code="raw_artifact_manifest_invalid",
        label="BSC-07 raw artifact manifest",
    )
    if (
        type(payload.get("schema_version")) is not int
        or payload.get("schema_version") != adapter_protocol.SCHEMA_VERSION
        or type(payload.get("protocol_version")) is not int
        or payload.get("protocol_version") != rig_adapters.ADAPTER_PROTOCOL_VERSION
        or payload.get("request_commitment_sha256") != expected_request_sha256
    ):
        raise RunnerError("raw_artifact_manifest_invalid", "BSC-07 raw manifest identity is invalid")
    rows = payload.get("artifacts")
    if not isinstance(rows, list) or len(rows) != len(rig_adapters.BSC07_RAW_ARTIFACTS):
        raise RunnerError("raw_artifact_manifest_invalid", "BSC-07 raw artifact set is incomplete")
    digest_by_role: dict[str, str] = {}
    for raw, contract in zip(rows, rig_adapters.BSC07_RAW_ARTIFACTS, strict=True):
        row = require_exact_object(
            raw,
            {"filename", "role", "sha256", "size_bytes"},
            code="raw_artifact_manifest_invalid",
            label="BSC-07 raw artifact",
        )
        digest = require_sha256(
            row.get("sha256"), code="raw_artifact_manifest_invalid", label="BSC-07 raw artifact"
        )
        if (
            row.get("filename") != contract.filename
            or row.get("role") != contract.role
            or type(row.get("size_bytes")) is not int
            or not 0 < row["size_bytes"] <= contract.maximum_bytes
            or contract.role in digest_by_role
        ):
            raise RunnerError("raw_artifact_manifest_invalid", "BSC-07 raw artifact contract drifted")
        digest_by_role[contract.role] = digest
    manifest_commitment = require_sha256(
        payload.get("manifest_commitment_sha256"),
        code="raw_artifact_manifest_invalid",
        label="BSC-07 raw manifest binding",
    )
    committed = dict(payload)
    committed.pop("manifest_commitment_sha256")
    if not secrets.compare_digest(
        manifest_commitment,
        adapter_protocol.canonical_commitment(adapter_protocol.MANIFEST_DOMAIN, committed),
    ):
        raise RunnerError("raw_artifact_manifest_invalid", "BSC-07 raw manifest binding is stale")
    return {
        field: digest_by_role[role]
        for field, role in BSC07_CAPTURE_ROLE_BY_FIELD.items()
    }


def validate_bsc07_raw_content(
    content_by_role: Mapping[str, bytes],
    *,
    record: Mapping[str, object],
) -> None:
    """Require the typed record to be a projection of runner-verified raw bytes."""

    expected_roles = {artifact.role for artifact in rig_adapters.BSC07_RAW_ARTIFACTS}
    if set(content_by_role) != expected_roles:
        raise RunnerError("raw_artifact_set_invalid", "BSC-07 raw artifact content is incomplete")

    observations = record.get("observations")
    firmware = record.get("firmware")
    reset = record.get("reset_observation")
    if not isinstance(observations, dict) or not isinstance(firmware, dict) or not isinstance(reset, dict):
        raise RunnerError("case_record_invalid", "BSC-07 record cannot be projected to raw evidence")

    def without_fields(value: object, fields: set[str], label: str) -> dict[str, object]:
        if not isinstance(value, dict) or not fields.issubset(value):
            raise RunnerError("case_record_invalid", f"BSC-07 {label} source binding is incomplete")
        projected = dict(value)
        for field in fields:
            projected.pop(field)
        return projected

    voltage = without_fields(
        observations.get("voltage_refresh"), {"source_role", "source_sha256"}, "voltage"
    )
    traffic = without_fields(
        observations.get("ap_traffic"), {"source_role", "source_sha256"}, "AP traffic"
    )
    button = without_fields(
        observations.get("power_button"), {"source_role", "source_sha256"}, "power button"
    )
    shutdown = without_fields(
        observations.get("critical_shutdown"), {"source_role", "source_sha256"}, "shutdown"
    )
    health = without_fields(observations.get("health"), {"source_commitments"}, "health")
    power = without_fields(
        observations.get("power"), {"source_role", "source_sha256"}, "power"
    )
    reset_projection = without_fields(reset, {"source_role", "source_sha256"}, "reset")
    firmware_projection = {
        field: firmware.get(field)
        for field in ("environment", "build_kind", "target_sha", "hil_fault_control_active")
    }
    expected_by_role: dict[str, object] = {
        "ap-traffic": traffic,
        "firmware-build": firmware_projection,
        "power-timeline": {
            "voltage_refresh": voltage,
            "power_button": button,
            "critical_shutdown": shutdown,
            "power": power,
        },
        "reset-summary": reset_projection,
        "serial-log": {"health": health, "reset": reset_projection},
        "ui-health": health,
    }
    for role, expected_payload in expected_by_role.items():
        observed_payload = read_json_bytes(content_by_role[role], f"BSC-07 {role}")
        if adapter_protocol.canonical_json_bytes(observed_payload) != adapter_protocol.canonical_json_bytes(
            expected_payload
        ):
            raise RunnerError(
                "case_record_invalid",
                f"BSC-07 {role} does not match the runner-verified raw evidence",
            )


def resolve_bsc07_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
    case_descriptor: Mapping[str, object],
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-07 requires the ignored local hardware inventory",
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
        raise RunnerError("case_alias_reused", "BSC-07 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def validate_bsc07_stimuli(value: object, *, duration_ms: int) -> dict[str, int]:
    if not isinstance(value, list) or len(value) != len(BSC07_STIMULUS_IDS):
        raise RunnerError("case_record_invalid", "BSC-07 stimulus sequence is incomplete")
    elapsed_by_id: dict[str, int] = {}
    previous = -1
    for sequence, (raw, stimulus_id) in enumerate(
        zip(value, BSC07_STIMULUS_IDS, strict=True), start=1
    ):
        row = require_exact_object(
            raw,
            {"id", "sequence", "elapsed_ms", "result"},
            code="case_record_invalid",
            label="BSC-07 stimulus",
        )
        elapsed = row.get("elapsed_ms")
        if (
            row.get("id") != stimulus_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or not 0 <= elapsed <= duration_ms
            or elapsed <= previous
            or row.get("result") != "pass"
        ):
            raise RunnerError("case_record_invalid", "BSC-07 stimulus order or timing is invalid")
        elapsed_by_id[stimulus_id] = elapsed
        previous = elapsed
    return elapsed_by_id


def validate_bsc07_observations(
    value: object,
    *,
    stimuli: Mapping[str, int],
    duration_ms: int,
    expected_capture: Callable[[str], str],
) -> dict[str, object]:
    observations = require_exact_object(
        value,
        {"voltage_refresh", "ap_traffic", "power_button", "critical_shutdown", "health", "power"},
        code="case_record_invalid",
        label="BSC-07 observations",
    )
    voltage = require_exact_object(
        observations.get("voltage_refresh"),
        {"changed_elapsed_ms", "refreshed_elapsed_ms", "source_role", "source_sha256"},
        code="case_record_invalid",
        label="BSC-07 voltage refresh",
    )
    traffic = require_exact_object(
        observations.get("ap_traffic"),
        {
            "started_elapsed_ms",
            "last_success_before_hold_elapsed_ms",
            "first_success_after_hold_elapsed_ms",
            "continuous",
            "source_role",
            "source_sha256",
        },
        code="case_record_invalid",
        label="BSC-07 AP traffic",
    )
    button = require_exact_object(
        observations.get("power_button"),
        {
            "hold_started_elapsed_ms",
            "hold_released_elapsed_ms",
            "handled_elapsed_ms",
            "source_role",
            "source_sha256",
        },
        code="case_record_invalid",
        label="BSC-07 power button",
    )
    shutdown = require_exact_object(
        observations.get("critical_shutdown"),
        {
            "applied_elapsed_ms",
            "warning_elapsed_ms",
            "shutdown_elapsed_ms",
            "source_role",
            "source_sha256",
        },
        code="case_record_invalid",
        label="BSC-07 critical shutdown",
    )
    health = require_exact_object(
        observations.get("health"),
        {
            "ui_last_healthy_elapsed_ms",
            "battery_last_healthy_elapsed_ms",
            "loop_last_healthy_elapsed_ms",
            "watchdog_last_healthy_elapsed_ms",
            "source_commitments",
        },
        code="case_record_invalid",
        label="BSC-07 health",
    )
    power = require_exact_object(
        observations.get("power"),
        {
            "vbus_isolated",
            "external_power_removed",
            "power_removed_elapsed_ms",
            "source_role",
            "source_sha256",
        },
        code="case_record_invalid",
        label="BSC-07 power observation",
    )
    changed = voltage.get("changed_elapsed_ms")
    refreshed = voltage.get("refreshed_elapsed_ms")
    traffic_started = traffic.get("started_elapsed_ms")
    before_hold = traffic.get("last_success_before_hold_elapsed_ms")
    after_hold = traffic.get("first_success_after_hold_elapsed_ms")
    hold_started = button.get("hold_started_elapsed_ms")
    hold_released = button.get("hold_released_elapsed_ms")
    handled = button.get("handled_elapsed_ms")
    critical = shutdown.get("applied_elapsed_ms")
    warning = shutdown.get("warning_elapsed_ms")
    shutdown_ms = shutdown.get("shutdown_elapsed_ms")
    health_times = tuple(
        health.get(field)
        for field in (
            "ui_last_healthy_elapsed_ms",
            "battery_last_healthy_elapsed_ms",
            "loop_last_healthy_elapsed_ms",
            "watchdog_last_healthy_elapsed_ms",
        )
    )
    health_sources = require_exact_object(
        health.get("source_commitments"),
        {"serial_log_sha256", "ui_health_sha256"},
        code="case_record_invalid",
        label="BSC-07 health sources",
    )
    numeric = (
        changed,
        refreshed,
        traffic_started,
        before_hold,
        after_hold,
        hold_started,
        hold_released,
        handled,
        critical,
        warning,
        shutdown_ms,
        *health_times,
        power.get("power_removed_elapsed_ms"),
    )
    if (
        any(type(item) is not int or not 0 <= item <= duration_ms for item in numeric)
        or changed != stimuli["change-battery-voltage"]
        or traffic_started != stimuli["apply-ap-load"]
        or hold_started != stimuli["hold-power-button"]
        or critical != stimuli["apply-critical-voltage"]
        or not changed <= refreshed <= hold_started
        or refreshed - changed > BSC07_VOLTAGE_REFRESH_MAX_MS
        or hold_released - hold_started != BSC07_POWER_HOLD_MS
        or not traffic_started <= before_hold <= hold_started
        or not hold_started <= handled <= hold_released
        or not hold_released <= after_hold <= critical
        or traffic.get("continuous") is not True
        or not critical <= warning < shutdown_ms
        or not BSC07_CRITICAL_GRACE_MIN_MS <= shutdown_ms - warning <= BSC07_CRITICAL_GRACE_MAX_MS
        or any(not warning <= item <= shutdown_ms for item in health_times)
        or any(shutdown_ms - item > BSC07_HEALTH_SAMPLE_MAX_AGE_MS for item in health_times)
        or voltage.get("source_role") != "power-timeline"
        or traffic.get("source_role") != "ap-traffic"
        or button.get("source_role") != "power-timeline"
        or shutdown.get("source_role") != "power-timeline"
        or power.get("source_role") != "power-timeline"
        or voltage.get("source_sha256") != expected_capture("power_timeline_sha256")
        or traffic.get("source_sha256") != expected_capture("ap_traffic_sha256")
        or button.get("source_sha256") != expected_capture("power_timeline_sha256")
        or shutdown.get("source_sha256") != expected_capture("power_timeline_sha256")
        or power.get("source_sha256") != expected_capture("power_timeline_sha256")
        or health_sources.get("serial_log_sha256") != expected_capture("serial_log_sha256")
        or health_sources.get("ui_health_sha256") != expected_capture("ui_health_sha256")
        or power.get("vbus_isolated") is not True
        or power.get("external_power_removed") is not True
        or power.get("power_removed_elapsed_ms") != shutdown_ms
    ):
        raise RunnerError("case_record_invalid", "BSC-07 power-safety observations are invalid")
    return observations


def validate_bsc07_barriers(
    value: object,
    *,
    observations: Mapping[str, object],
) -> None:
    if not isinstance(value, list) or len(value) != 1:
        raise RunnerError("case_record_invalid", "BSC-07 critical grace barrier is missing")
    row = require_exact_object(
        value[0],
        {"id", "sequence", "ready_elapsed_ms", "released_elapsed_ms", "timed_out"},
        code="case_record_invalid",
        label="BSC-07 barrier",
    )
    shutdown = observations.get("critical_shutdown")
    assert isinstance(shutdown, dict)
    if (
        row.get("id") != "critical-voltage-grace"
        or type(row.get("sequence")) is not int
        or row.get("sequence") != 1
        or type(row.get("ready_elapsed_ms")) is not int
        or row.get("ready_elapsed_ms") != shutdown.get("warning_elapsed_ms")
        or type(row.get("released_elapsed_ms")) is not int
        or row.get("released_elapsed_ms") != shutdown.get("shutdown_elapsed_ms")
        or row.get("timed_out") is not False
    ):
        raise RunnerError("case_record_invalid", "BSC-07 critical grace barrier is invalid")


def validate_bsc07_reset_observation(
    value: object,
    *,
    observations: Mapping[str, object],
    expected_capture: Callable[[str], str],
) -> None:
    observation = require_exact_object(
        value,
        {
            "expected_kind",
            "planned_count",
            "observed_count",
            "unexpected_count",
            "panic_observed",
            "watchdog_reset_observed",
            "source_role",
            "source_sha256",
            "shutdown_elapsed_ms",
            "observed_elapsed_ms",
            "reason",
        },
        code="case_record_invalid",
        label="BSC-07 reset observation",
    )
    if (
        observation.get("expected_kind") != "intentional-shutdown"
        or type(observation.get("planned_count")) is not int
        or observation.get("planned_count") != 1
        or type(observation.get("observed_count")) is not int
        or observation.get("observed_count") != 1
        or type(observation.get("unexpected_count")) is not int
        or observation.get("unexpected_count") != 0
        or type(observation.get("panic_observed")) is not bool
        or observation.get("panic_observed") is not False
        or type(observation.get("watchdog_reset_observed")) is not bool
        or observation.get("watchdog_reset_observed") is not False
        or observation.get("source_role") != "reset-summary"
        or observation.get("source_sha256") != expected_capture("reset_summary_sha256")
        or type(observation.get("shutdown_elapsed_ms")) is not int
        or type(observation.get("observed_elapsed_ms")) is not int
        or observation.get("reason") != "intentional-critical-voltage-shutdown"
    ):
        raise RunnerError("case_record_invalid", "BSC-07 reset evidence is invalid")
    shutdown = observations.get("critical_shutdown")
    power = observations.get("power")
    assert isinstance(shutdown, dict) and isinstance(power, dict)
    if (
        observation.get("shutdown_elapsed_ms") != shutdown.get("shutdown_elapsed_ms")
        or observation.get("observed_elapsed_ms") != power.get("power_removed_elapsed_ms")
    ):
        raise RunnerError("case_record_invalid", "BSC-07 reset timing is not bound to power removal")


def validate_bsc07_facts(
    value: object,
    *,
    observations: Mapping[str, object],
) -> None:
    facts = require_exact_object(
        value,
        {
            "voltage-refresh-delay-ms",
            "power-button-handled-under-ap-load",
            "critical-shutdown-grace-ms",
            "critical-warning-observed",
            "ui-responsive-until-shutdown",
            "loop-stall-observed",
        },
        code="case_record_invalid",
        label="BSC-07 facts",
    )
    voltage = observations.get("voltage_refresh")
    traffic = observations.get("ap_traffic")
    button = observations.get("power_button")
    shutdown = observations.get("critical_shutdown")
    health = observations.get("health")
    assert all(isinstance(row, dict) for row in (voltage, traffic, button, shutdown, health))
    refresh_delay = int(voltage["refreshed_elapsed_ms"]) - int(voltage["changed_elapsed_ms"])
    grace = int(shutdown["shutdown_elapsed_ms"]) - int(shutdown["warning_elapsed_ms"])
    if (
        type(facts.get("voltage-refresh-delay-ms")) is not int
        or facts.get("voltage-refresh-delay-ms") != refresh_delay
        or type(facts.get("power-button-handled-under-ap-load")) is not bool
        or facts.get("power-button-handled-under-ap-load")
        is not (
            traffic["continuous"] is True
            and traffic["last_success_before_hold_elapsed_ms"] <= button["handled_elapsed_ms"]
            and button["handled_elapsed_ms"] <= traffic["first_success_after_hold_elapsed_ms"]
        )
        or type(facts.get("critical-shutdown-grace-ms")) is not int
        or facts.get("critical-shutdown-grace-ms") != grace
        or type(facts.get("critical-warning-observed")) is not bool
        or facts.get("critical-warning-observed") is not (shutdown["warning_elapsed_ms"] >= shutdown["applied_elapsed_ms"])
        or type(facts.get("ui-responsive-until-shutdown")) is not bool
        or facts.get("ui-responsive-until-shutdown")
        is not (health["ui_last_healthy_elapsed_ms"] <= shutdown["shutdown_elapsed_ms"])
        or type(facts.get("loop-stall-observed")) is not bool
        or facts.get("loop-stall-observed")
        is not (shutdown["shutdown_elapsed_ms"] - health["loop_last_healthy_elapsed_ms"] > BSC07_HEALTH_SAMPLE_MAX_AGE_MS)
    ):
        raise RunnerError("case_record_invalid", "BSC-07 facts do not match the observations")


def validate_bsc07_adapter_record(
    payload: object,
    *,
    expected: Mapping[str, object],
    raw_manifest: object,
    command_started: datetime | None = None,
    command_completed: datetime | None = None,
) -> dict[str, object]:
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
            "observations",
            "barriers",
            "reset_observation",
            "facts",
            "capture_commitments",
            "raw_artifact_request_sha256",
            "evidence_binding_sha256",
        },
        code="case_record_invalid",
        label="BSC-07 adapter record",
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
            raise RunnerError("case_record_invalid", f"BSC-07 {field} does not match the runner")
    if (
        type(record.get("schema_version")) is not int
        or record.get("schema_version") != 1
        or type(record.get("hardware_observed")) is not bool
    ):
        raise RunnerError("case_record_invalid", "BSC-07 record identity types are invalid")
    raw_request_sha = require_sha256(
        record.get("raw_artifact_request_sha256"),
        code="case_record_invalid",
        label="BSC-07 raw artifact request",
    )
    if raw_request_sha != expected.get("raw_artifact_request_sha256"):
        raise RunnerError("case_record_invalid", "BSC-07 raw artifact request was substituted")
    manifest_commitments = bsc07_manifest_capture_commitments(
        raw_manifest,
        expected_request_sha256=raw_request_sha,
    )
    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC07_CAPTURE_COMMITMENTS),
        code="case_record_invalid",
        label="BSC-07 capture commitments",
    )
    for field in BSC07_CAPTURE_COMMITMENTS:
        observed = require_sha256(commitments.get(field), code="case_record_invalid", label=field)
        if observed != manifest_commitments[field]:
            raise RunnerError("case_record_invalid", f"BSC-07 {field} is not runner-hashed evidence")
    if len(set(manifest_commitments.values())) != len(manifest_commitments):
        raise RunnerError("case_record_invalid", "BSC-07 evidence roles reused a capture")

    def expected_capture(field: str) -> str:
        return manifest_commitments[field]
    case_descriptor = expected.get("case_descriptor")
    descriptor_sha = bsc07_descriptor_commitment(case_descriptor) if isinstance(case_descriptor, dict) else ""
    if (
        not isinstance(case_descriptor, dict)
        or record.get("case_descriptor") != case_descriptor
        or expected.get("case_descriptor_sha256") != descriptor_sha
        or record.get("case_descriptor_sha256") != descriptor_sha
    ):
        raise RunnerError("case_record_invalid", "BSC-07 descriptor binding is invalid")
    started = parse_runner_utc(record.get("started_at_utc"), code="case_record_invalid", label="BSC-07 start")
    completed = parse_runner_utc(
        record.get("completed_at_utc"), code="case_record_invalid", label="BSC-07 completion"
    )
    if completed <= started:
        raise RunnerError("case_record_invalid", "BSC-07 run duration is invalid")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError("case_record_invalid", "BSC-07 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=999999):
        raise RunnerError("case_record_invalid", "BSC-07 physical record postdates adapter execution")
    duration_ms = int((completed - started).total_seconds() * 1_000)
    firmware = require_exact_object(
        record.get("firmware"),
        {"environment", "build_kind", "target_sha", "binary_sha256", "hil_fault_control_active"},
        code="case_record_invalid",
        label="BSC-07 firmware",
    )
    if (
        firmware.get("environment") != BSC07_PRODUCTION_ENVIRONMENT
        or firmware.get("build_kind") != "production"
        or firmware.get("target_sha") != expected.get("target_sha")
        or type(firmware.get("hil_fault_control_active")) is not bool
        or firmware.get("hil_fault_control_active") is not False
    ):
        raise RunnerError("case_record_invalid", "BSC-07 firmware role or target is invalid")
    require_sha256(firmware.get("binary_sha256"), code="case_record_invalid", label="BSC-07 firmware")
    stimuli = validate_bsc07_stimuli(record.get("stimuli"), duration_ms=duration_ms)
    observations = validate_bsc07_observations(
        record.get("observations"),
        stimuli=stimuli,
        duration_ms=duration_ms,
        expected_capture=expected_capture,
    )
    validate_bsc07_barriers(record.get("barriers"), observations=observations)
    validate_bsc07_reset_observation(
        record.get("reset_observation"),
        observations=observations,
        expected_capture=expected_capture,
    )
    validate_bsc07_facts(record.get("facts"), observations=observations)
    binding = require_sha256(
        record.get("evidence_binding_sha256"), code="case_record_invalid", label="BSC-07 binding"
    )
    if not secrets.compare_digest(binding, bsc07_record_commitment(record)):
        raise RunnerError("case_record_invalid", "BSC-07 evidence binding is invalid")
    return record


def run_bsc07_adapter(
    *,
    adapter: Path,
    repository: Path,
    serial_port: str,
    expected: Mapping[str, object],
    environment: Mapping[str, str],
    raw_directory: Path,
    raw_manifest_path: Path,
    role_contract: rig_adapters.AdapterRoleContract,
) -> tuple[dict[str, object], dict[str, object]]:
    command = [
        str(adapter),
        "--case",
        BSC07_CASE_ID,
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
        "--raw-artifact-dir",
        str(raw_directory),
        "--raw-artifact-request-sha256",
        str(expected["raw_artifact_request_sha256"]),
    ]
    command_started = datetime.now(timezone.utc)
    try:
        completed = subprocess.run(
            command,
            cwd=repository,
            env=dict(environment),
            capture_output=True,
            check=False,
            timeout=BSC07_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-07 adapter exceeded its timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-07 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-07 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 128 * 1024:
        raise RunnerError("case_record_invalid", "BSC-07 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-07 adapter output is not strict JSON") from exc
    try:
        raw_manifest = adapter_protocol.collect_raw_artifacts(
            raw_directory=raw_directory,
            role=role_contract,
            request_commitment_sha256=str(expected["raw_artifact_request_sha256"]),
            manifest_path=raw_manifest_path,
        )
        raw_content = adapter_protocol.read_collected_raw_artifacts(
            raw_directory=raw_directory,
            role=role_contract,
            manifest=raw_manifest,
        )
    except adapter_protocol.AdapterProtocolError as exc:
        raise RunnerError(exc.code, exc.message) from exc
    physical = expected.get("execution_mode") == "physical"
    record = validate_bsc07_adapter_record(
        payload,
        expected=expected,
        raw_manifest=raw_manifest,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )
    validate_bsc07_raw_content(raw_content, record=record)
    return record, raw_manifest


def admit_case_rig_adapter(
    args: argparse.Namespace,
    *,
    case_contract: Mapping[str, object],
    role_id: str,
) -> RigAdapterAdmission:
    """Validate the shared adapter boundary before discovery or output mutation."""

    case_id = case_contract.get("id")
    if not isinstance(case_id, str):
        raise RunnerError("adapter_contract_invalid", "case contract lacks a safe identity")
    try:
        adapter = rig_adapters.get_rig_adapter(case_id)
        rig_adapters.validate_adapter_descriptor(adapter)
    except rig_adapters.RigAdapterContractError as exc:
        raise RunnerError(
            "adapter_contract_invalid", "tracked rig-adapter registry is invalid"
        ) from exc
    raw_roles = [case_contract.get("scenario")]
    production = case_contract.get("production_replay")
    if production is not None:
        raw_roles.append(production)
    expected_roles: list[tuple[str, str]] = []
    for raw_role in raw_roles:
        if not isinstance(raw_role, dict):
            raise RunnerError("adapter_contract_invalid", "case role contract is invalid")
        profile_role_id = raw_role.get("role_id")
        build_kind = raw_role.get("build_kind")
        if not isinstance(profile_role_id, str) or not isinstance(build_kind, str):
            raise RunnerError("adapter_contract_invalid", "case role contract is invalid")
        expected_roles.append((profile_role_id, build_kind))
    observed_roles = [(role.role_id, role.build_kind) for role in adapter.roles]
    if (
        adapter.minimum_runs != case_contract.get("minimum_runs")
        or list(adapter.required_dut_capabilities)
        != case_contract.get("required_dut_capabilities")
        or list(adapter.required_rig_capabilities)
        != case_contract.get("required_rig_capabilities")
        or observed_roles != expected_roles
        or role_id not in {item[0] for item in expected_roles}
    ):
        raise RunnerError(
            "adapter_contract_invalid",
            "tracked rig-adapter contract does not match the pinned profile",
        )
    if not test_hooks_enabled():
        if args.case_adapter is not None:
            raise RunnerError(
                "untrusted_override",
                f"authoritative {case_id} forbids an untracked rig adapter",
            )
        if not adapter.implemented:
            raise RunnerError(
                "case_rig_adapter_unavailable",
                f"{case_id} physical execution remains blocked until its tracked rig adapter exists",
            )
        repository = Path(os.path.abspath(args.repo_root))
        git_state = read_git_state(repository)
        if not git_state.tracked_clean:
            raise RunnerError("dirty_target", "tracked rig adapter requires a clean target worktree")
        source_sha256 = verify_tracked_rig_adapter_source(repository, git_state, adapter)
        return RigAdapterAdmission(
            adapter=adapter,
            simulated=False,
            git_state=git_state,
            source_sha256=source_sha256,
        )
    return RigAdapterAdmission(
        adapter=adapter,
        simulated=True,
        git_state=None,
        source_sha256=None,
    )


def bsc08_profile_descriptor() -> dict[str, object]:
    code = "case_driver_contract_invalid"
    profile, errors = qualification.load_pinned_profile()
    if profile is None or errors or profile.get("profile_version") != 5:
        raise RunnerError(code, "BSC-08 requires the exact pinned profile v5 contract")
    contract = next(
        (candidate for candidate in profile["required_cases"] if candidate["id"] == BSC08_CASE_ID),
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
        label="BSC-08 case descriptor",
    )
    expected_facts: list[dict[str, object]] = [
        {"id": "queue-index-corruption-observed", "type": "boolean", "expected": False},
        {"id": "heap-corruption-observed", "type": "boolean", "expected": False},
        {"id": "old-epoch-forwarded", "type": "boolean", "expected": False},
        {"id": "deferred-release-opportunities", "type": "integer", "minimum": 1, "maximum": 1},
        {"id": "fresh-bidirectional-traffic-resumed", "type": "boolean", "expected": True},
        {"id": "monotonic-heap-loss-observed", "type": "boolean", "expected": False},
    ]
    expected = {
        "role_id": "proxy-epoch-teardown",
        "schema": "case-observation-v1",
        "build_kind": "production",
        "stimulus_ids": list(BSC08_STIMULUS_IDS),
        "fault_ids": [],
        "barrier_ids": list(BSC08_BARRIER_IDS),
        "vbus_isolation_required": False,
        "reset_contract": {"expected_kind": "none", "expected_count": 0, "unexpected_count": 0},
        "facts": expected_facts,
    }
    if (
        row.get("id") != BSC08_CASE_ID
        or type(row.get("minimum_runs")) is not int
        or row.get("minimum_runs") != BSC08_REQUIRED_RUNS
        or row.get("fault_build_required") is not False
        or row.get("production_replay_required") is not False
        or row.get("required_dut_capabilities") != list(BSC08_DUT_CAPABILITIES)
        or row.get("required_rig_capabilities") != list(BSC08_RIG_CAPABILITIES)
        or row.get("production_replay") is not None
        or row.get("scenario") != expected
    ):
        raise RunnerError(code, "BSC-08 pinned descriptor does not match the typed driver")
    return expected


def bsc08_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc08.case-record.v1", committed)


def bsc08_source_capture_commitment(record: Mapping[str, object]) -> str:
    return canonical_case_commitment(
        BSC08_CAPTURE_BINDING_DOMAIN,
        {
            "raw_artifact_manifest": record.get("raw_artifact_manifest"),
            "firmware": record.get("firmware"),
            "serial_queries": record.get("serial_queries"),
            "stimuli": record.get("stimuli"),
            "barriers": record.get("barriers"),
            "deliveries": record.get("deliveries"),
            "resets": record.get("resets"),
            "safety": record.get("safety"),
            "facts": record.get("facts"),
        },
    )


def _bsc08_role_contract() -> rig_adapters.AdapterRoleContract:
    adapter = rig_adapters.get_rig_adapter(BSC08_CASE_ID)
    roles = [role for role in adapter.roles if role.role_id == "proxy-epoch-teardown"]
    if len(roles) != 1 or roles[0].raw_artifacts != rig_adapters.BSC08_RAW_ARTIFACTS:
        raise RunnerError("adapter_contract_invalid", "BSC-08 raw-artifact contract drifted")
    return roles[0]


def collect_bsc08_raw_artifact_manifest(
    *,
    raw_directory: Path,
    request_commitment_sha256: str,
    manifest_path: Path,
) -> dict[str, object]:
    """Have the runner hash the exact bounded BSC-08 capture set."""

    try:
        return adapter_protocol.collect_raw_artifacts(
            raw_directory=raw_directory,
            role=_bsc08_role_contract(),
            request_commitment_sha256=request_commitment_sha256,
            manifest_path=manifest_path,
        )
    except adapter_protocol.AdapterProtocolError as exc:
        raise RunnerError(exc.code, exc.message) from exc


def validate_bsc08_raw_artifact_manifest(
    value: object,
    *,
    expected_request_commitment_sha256: str,
) -> dict[str, dict[str, object]]:
    code = "case_record_invalid"
    manifest = require_exact_object(
        value,
        {
            "schema_version",
            "protocol_version",
            "request_commitment_sha256",
            "artifacts",
            "manifest_commitment_sha256",
        },
        code=code,
        label="BSC-08 raw-artifact manifest",
    )
    expected_request = require_sha256(
        expected_request_commitment_sha256,
        code=code,
        label="BSC-08 expected adapter request commitment",
    )
    if (
        type(manifest.get("schema_version")) is not int
        or manifest.get("schema_version") != adapter_protocol.SCHEMA_VERSION
        or type(manifest.get("protocol_version")) is not int
        or manifest.get("protocol_version") != rig_adapters.ADAPTER_PROTOCOL_VERSION
        or manifest.get("request_commitment_sha256") != expected_request
    ):
        raise RunnerError(code, "BSC-08 raw-artifact manifest identity is invalid")
    artifacts = manifest.get("artifacts")
    contracts = _bsc08_role_contract().raw_artifacts
    if not isinstance(artifacts, list) or len(artifacts) != len(contracts):
        raise RunnerError(code, "BSC-08 raw-artifact manifest is incomplete")
    by_role: dict[str, dict[str, object]] = {}
    for raw, contract in zip(artifacts, contracts, strict=True):
        artifact = require_exact_object(
            raw,
            {"filename", "role", "sha256", "size_bytes"},
            code=code,
            label="BSC-08 raw artifact",
        )
        digest = require_sha256(
            artifact.get("sha256"),
            code=code,
            label=f"BSC-08 {contract.role}",
        )
        size = artifact.get("size_bytes")
        if (
            artifact.get("filename") != contract.filename
            or artifact.get("role") != contract.role
            or type(size) is not int
            or not 1 <= size <= contract.maximum_bytes
            or contract.role in by_role
        ):
            raise RunnerError(code, "BSC-08 raw artifact does not match its bounded contract")
        by_role[contract.role] = artifact
        assert isinstance(digest, str)
    committed = dict(manifest)
    commitment = require_sha256(
        committed.pop("manifest_commitment_sha256"),
        code=code,
        label="BSC-08 raw-artifact manifest commitment",
    )
    expected_commitment = adapter_protocol.canonical_commitment(
        adapter_protocol.MANIFEST_DOMAIN,
        committed,
    )
    if not secrets.compare_digest(commitment, expected_commitment):
        raise RunnerError(code, "BSC-08 raw-artifact manifest commitment is stale")
    if len({artifact["sha256"] for artifact in by_role.values()}) != len(by_role):
        raise RunnerError(code, "BSC-08 raw artifact roles reused one capture")
    return by_role


def _bsc08_uint(value: object, *, label: str) -> int:
    if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
        raise RunnerError("case_record_invalid", f"BSC-08 {label} is not a bounded unsigned integer")
    return value


def validate_bsc08_serial_response(value: object, *, expected_nonce: str) -> dict[str, object]:
    code = "case_record_invalid"
    if isinstance(value, dict) and value.get("status") == "busy":
        busy = require_exact_object(
            value,
            {"schema", "nonce", "status"},
            code=code,
            label="BSC-08 busy serial response",
        )
        if type(busy.get("schema")) is not int or busy.get("schema") != 1 or busy.get("nonce") != expected_nonce:
            raise RunnerError(code, "BSC-08 busy response does not bind the runner nonce")
        raise RunnerError("case_incomplete", "BSC-08 zero-timeout snapshot was busy; rerun required")
    response = require_exact_object(
        value,
        {
            "schema",
            "nonce",
            "status",
            "epoch",
            "gateEpoch",
            "active",
            "callbackEntries",
            "admissions",
            "staleRejects",
            "lifecycle",
            "activeOverlap",
            "releaseOpportunity",
            "oldForwarded",
            "proxyQueue",
            "phoneQueue",
            "heap",
        },
        code=code,
        label="BSC-08 serial response",
    )
    if type(response.get("schema")) is not int or response.get("schema") != 1 or response.get("nonce") != expected_nonce:
        raise RunnerError(code, "BSC-08 serial response does not bind the runner nonce")
    if response.get("status") != "ready":
        raise RunnerError(code, "BSC-08 serial response status is invalid")
    for field in ("epoch", "gateEpoch", "active"):
        _bsc08_uint(response.get(field), label=field)
    for field, length in (
        ("callbackEntries", 2),
        ("admissions", 2),
        ("staleRejects", 2),
        ("lifecycle", 4),
        ("proxyQueue", 4),
        ("phoneQueue", 4),
        ("heap", 2),
    ):
        items = response.get(field)
        if not isinstance(items, list) or len(items) != length:
            raise RunnerError(code, f"BSC-08 {field} shape is invalid")
        for index, item in enumerate(items):
            _bsc08_uint(item, label=f"{field}[{index}]")
    for field in ("activeOverlap", "releaseOpportunity", "oldForwarded"):
        if type(response.get(field)) is not bool:
            raise RunnerError(code, f"BSC-08 {field} must be typed boolean")
    if response["active"] != 0:
        raise RunnerError("case_incomplete", "BSC-08 snapshot caught a callback still active")
    return response


def validate_bsc08_queries(value: object) -> list[dict[str, object]]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(BSC08_SNAPSHOT_PHASES):
        raise RunnerError(code, "BSC-08 serial query coverage is incomplete")
    queries: list[dict[str, object]] = []
    previous_elapsed = -1
    nonces: set[str] = set()
    for sequence, (raw, phase) in enumerate(zip(value, BSC08_SNAPSHOT_PHASES, strict=True), start=1):
        query = require_exact_object(
            raw,
            {"phase", "sequence", "elapsed_ms", "nonce", "response"},
            code=code,
            label="BSC-08 serial query",
        )
        elapsed = _bsc08_uint(query.get("elapsed_ms"), label="query elapsed time")
        nonce = query.get("nonce")
        if (
            query.get("phase") != phase
            or type(query.get("sequence")) is not int
            or query.get("sequence") != sequence
            or elapsed < previous_elapsed
            or not isinstance(nonce, str)
            or re.fullmatch(r"[0-9a-f]{32}", nonce) is None
            or nonce in nonces
        ):
            raise RunnerError(code, "BSC-08 serial query identity or ordering is invalid")
        validate_bsc08_serial_response(query.get("response"), expected_nonce=nonce)
        previous_elapsed = elapsed
        nonces.add(nonce)
        queries.append(query)
    return queries


def _bsc08_jsonl(raw: bytes, *, label: str) -> list[dict[str, object]]:
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeError as exc:
        raise RunnerError("case_record_invalid", f"BSC-08 {label} is not UTF-8") from exc
    if not text.endswith("\n"):
        raise RunnerError("case_record_invalid", f"BSC-08 {label} is incomplete")
    rows: list[dict[str, object]] = []
    for line in text.splitlines():
        try:
            value = json.loads(line, object_pairs_hook=reject_duplicate_json_keys)
        except (json.JSONDecodeError, ValueError) as exc:
            raise RunnerError("case_record_invalid", f"BSC-08 {label} is not strict JSONL") from exc
        if not isinstance(value, dict):
            raise RunnerError("case_record_invalid", f"BSC-08 {label} row is not an object")
        rows.append(value)
    return rows


def bsc08_serial_nonce(request_nonce: str, phase: str) -> str:
    return hashlib.sha256(
        b"v1simple.bsc08.serial-nonce.v1\0"
        + request_nonce.encode("ascii")
        + b"\0"
        + phase.encode("ascii")
    ).hexdigest()[:32]


def parse_bsc08_serial_capture(raw: bytes, *, request_nonce: str) -> list[dict[str, object]]:
    rows = _bsc08_jsonl(raw, label="serial capture")
    if len(rows) != len(BSC08_SNAPSHOT_PHASES):
        raise RunnerError("case_record_invalid", "BSC-08 serial query coverage is incomplete")
    queries: list[dict[str, object]] = []
    for sequence, (row, phase) in enumerate(zip(rows, BSC08_SNAPSHOT_PHASES, strict=True), start=1):
        typed = require_exact_object(
            row,
            {"schema_version", "phase", "sequence", "elapsed_ms", "nonce", "response_line"},
            code="case_record_invalid",
            label="BSC-08 serial capture row",
        )
        nonce = bsc08_serial_nonce(request_nonce, phase)
        response_line = typed.get("response_line")
        if (
            type(typed.get("schema_version")) is not int
            or typed.get("schema_version") != 1
            or typed.get("phase") != phase
            or type(typed.get("sequence")) is not int
            or typed.get("sequence") != sequence
            or typed.get("nonce") != nonce
            or not isinstance(response_line, str)
            or not response_line.startswith("QBSC08 ")
        ):
            raise RunnerError("case_record_invalid", "BSC-08 serial capture identity is invalid")
        try:
            response = json.loads(
                response_line[len("QBSC08 ") :],
                object_pairs_hook=reject_duplicate_json_keys,
            )
        except (json.JSONDecodeError, ValueError) as exc:
            raise RunnerError(
                "case_record_invalid", "BSC-08 serial response is not strict JSON"
            ) from exc
        queries.append(
            {
                "phase": phase,
                "sequence": sequence,
                "elapsed_ms": typed.get("elapsed_ms"),
                "nonce": nonce,
                "response": response,
            }
        )
    return validate_bsc08_queries(queries)


def parse_bsc08_stimuli(
    raw: bytes,
    *,
    request_commitment_sha256: str,
    queries: Sequence[Mapping[str, object]],
) -> tuple[list[dict[str, object]], dict[str, dict[str, str]]]:
    rows = _bsc08_jsonl(raw, label="adapter transcript")
    stimulus_phases = ("streaming", "disabled", "reenabled", "old-traffic", "fresh-traffic")
    if len(rows) != len(BSC08_STIMULUS_IDS):
        raise RunnerError("case_record_invalid", "BSC-08 adapter transcript is incomplete")
    query_by_phase = {query["phase"]: query for query in queries}
    stimuli: list[dict[str, object]] = []
    payloads: dict[str, dict[str, str]] = {}
    for sequence, (row, stimulus_id, phase) in enumerate(
        zip(rows, BSC08_STIMULUS_IDS, stimulus_phases, strict=True), start=1
    ):
        typed = require_exact_object(
            row,
            {
                "schema_version",
                "request_commitment_sha256",
                "event",
                "id",
                "sequence",
                "phase",
                "elapsed_ms",
                "observed",
                "payload_sha256",
            },
            code="case_record_invalid",
            label="BSC-08 adapter transcript row",
        )
        raw_payloads = require_exact_object(
            typed.get("payload_sha256"),
            {"proxy_to_v1", "v1_to_proxy"},
            code="case_record_invalid",
            label="BSC-08 stimulus payload commitments",
        )
        traffic_phase = phase in {"old-traffic", "fresh-traffic"}
        if traffic_phase:
            payload_pair = {
                direction: require_sha256(
                    raw_payloads.get(direction),
                    code="case_record_invalid",
                    label=f"BSC-08 {phase} {direction} payload",
                )
                for direction in ("proxy_to_v1", "v1_to_proxy")
            }
            payloads[phase] = payload_pair
        elif raw_payloads != {"proxy_to_v1": None, "v1_to_proxy": None}:
            raise RunnerError("case_record_invalid", "BSC-08 non-traffic stimulus declared payloads")
        expected_elapsed = query_by_phase[phase]["elapsed_ms"]
        if (
            type(typed.get("schema_version")) is not int
            or typed.get("schema_version") != 1
            or typed.get("request_commitment_sha256") != request_commitment_sha256
            or typed.get("event") != "stimulus"
            or typed.get("id") != stimulus_id
            or type(typed.get("sequence")) is not int
            or typed.get("sequence") != sequence
            or typed.get("phase") != phase
            or typed.get("elapsed_ms") != expected_elapsed
            or typed.get("observed") is not True
        ):
            raise RunnerError("case_record_invalid", "BSC-08 stimulus is not bound to its serial phase")
        stimuli.append(
            {
                "id": stimulus_id,
                "sequence": sequence,
                "elapsed_ms": expected_elapsed,
                "observed": True,
            }
        )
    return stimuli, payloads


def parse_bsc08_firmware(
    raw: bytes,
    *,
    request: Mapping[str, object],
) -> dict[str, object]:
    firmware = require_exact_object(
        read_json_bytes(raw, "BSC-08 firmware build"),
        {
            "schema_version",
            "request_commitment_sha256",
            "environment",
            "build_kind",
            "target_sha",
            "binary_sha256",
            "hil_fault_control_active",
        },
        code="case_record_invalid",
        label="BSC-08 firmware build",
    )
    request_firmware = request.get("firmware")
    if not isinstance(request_firmware, dict):
        raise RunnerError("case_record_invalid", "BSC-08 request firmware identity is invalid")
    if (
        type(firmware.get("schema_version")) is not int
        or firmware.get("schema_version") != 1
        or firmware.get("request_commitment_sha256") != request.get("request_commitment_sha256")
        or firmware.get("environment") != BSC08_PRODUCTION_ENVIRONMENT
        or firmware.get("build_kind") != "production"
        or firmware.get("target_sha") != request.get("target_sha")
        or firmware.get("binary_sha256") != request_firmware.get("binary_sha256")
        or firmware.get("hil_fault_control_active") is not False
    ):
        raise RunnerError("case_record_invalid", "BSC-08 firmware capture is not the requested production image")
    require_sha256(firmware.get("binary_sha256"), code="case_record_invalid", label="BSC-08 firmware")
    return {
        "environment": firmware["environment"],
        "target_sha": firmware["target_sha"],
        "binary_sha256": firmware["binary_sha256"],
        "hil_fault_control_active": firmware["hil_fault_control_active"],
    }


def parse_bsc08_safety(
    raw: bytes,
    *,
    request_commitment_sha256: str,
) -> tuple[dict[str, bool], dict[str, object]]:
    summary = require_exact_object(
        read_json_bytes(raw, "BSC-08 safety summary"),
        {"schema_version", "request_commitment_sha256", "safety", "resets"},
        code="case_record_invalid",
        label="BSC-08 safety summary",
    )
    safety = require_exact_object(
        summary.get("safety"),
        {"panic_observed", "watchdog_reset_observed", "load_prohibited_observed"},
        code="case_record_invalid",
        label="BSC-08 safety",
    )
    resets = require_exact_object(
        summary.get("resets"),
        {"expected_kind", "planned", "observed", "unexpected"},
        code="case_record_invalid",
        label="BSC-08 resets",
    )
    if (
        type(summary.get("schema_version")) is not int
        or summary.get("schema_version") != 1
        or summary.get("request_commitment_sha256") != request_commitment_sha256
        or any(type(value) is not bool for value in safety.values())
        or resets != {"expected_kind": "none", "planned": 0, "observed": 0, "unexpected": 0}
    ):
        raise RunnerError("case_record_invalid", "BSC-08 safety/reset capture is invalid")
    return dict(safety), dict(resets)


def parse_bsc08_peer_receipt(
    raw: bytes,
    *,
    direction: str,
    request_commitment_sha256: str,
    payload_sha256: str,
    current_epoch: int,
    fresh_elapsed_ms: int,
    final_elapsed_ms: int,
) -> dict[str, object]:
    rows = _bsc08_jsonl(raw, label=f"{direction} peer receipts")
    if len(rows) != 1:
        raise RunnerError("case_incomplete", f"BSC-08 {direction} delivery receipt is missing")
    receipt = require_exact_object(
        rows[0],
        {
            "schema_version",
            "request_commitment_sha256",
            "receipt_id",
            "direction",
            "phase",
            "epoch",
            "payload_sha256",
            "delivered",
            "elapsed_ms",
        },
        code="case_record_invalid",
        label="BSC-08 peer receipt",
    )
    elapsed = receipt.get("elapsed_ms")
    if (
        type(receipt.get("schema_version")) is not int
        or receipt.get("schema_version") != 1
        or receipt.get("request_commitment_sha256") != request_commitment_sha256
        or not isinstance(receipt.get("receipt_id"), str)
        or re.fullmatch(r"receipt-[a-z0-9][a-z0-9._-]{0,63}", receipt["receipt_id"]) is None
        or receipt.get("direction") != direction
        or receipt.get("phase") != "fresh-traffic"
        or type(receipt.get("epoch")) is not int
        or receipt.get("epoch") != current_epoch
        or receipt.get("payload_sha256") != payload_sha256
        or receipt.get("delivered") is not True
        or type(elapsed) is not int
        or not fresh_elapsed_ms <= elapsed <= final_elapsed_ms
    ):
        raise RunnerError("case_incomplete", f"BSC-08 {direction} traffic was not delivered")
    return dict(receipt)


def parse_bsc08_raw_evidence(
    *,
    raw_manifest: object,
    raw_content: Mapping[str, bytes],
    request: Mapping[str, object],
) -> dict[str, object]:
    request_commitment = request.get("request_commitment_sha256")
    request_nonce = request.get("nonce")
    if not isinstance(request_commitment, str) or not isinstance(request_nonce, str):
        raise RunnerError("case_record_invalid", "BSC-08 runner request identity is invalid")
    validate_bsc08_raw_artifact_manifest(
        raw_manifest,
        expected_request_commitment_sha256=request_commitment,
    )
    if set(raw_content) != set(BSC08_RAW_ARTIFACT_ROLES):
        raise RunnerError("case_record_invalid", "BSC-08 verified raw artifact content is incomplete")
    queries = parse_bsc08_serial_capture(raw_content["serial-log"], request_nonce=request_nonce)
    stimuli, payloads = parse_bsc08_stimuli(
        raw_content["adapter-transcript"],
        request_commitment_sha256=request_commitment,
        queries=queries,
    )
    firmware = parse_bsc08_firmware(raw_content["firmware-build"], request=request)
    safety, resets = parse_bsc08_safety(
        raw_content["safety-summary"],
        request_commitment_sha256=request_commitment,
    )
    fresh = queries[BSC08_SNAPSHOT_PHASES.index("fresh-traffic")]
    final = queries[-1]
    fresh_response = fresh["response"]
    assert isinstance(fresh_response, dict)
    current_epoch = fresh_response["epoch"]
    assert isinstance(current_epoch, int)
    deliveries = {
        "v1_to_proxy": parse_bsc08_peer_receipt(
            raw_content["proxy-peer-receipts"],
            direction="v1-to-proxy",
            request_commitment_sha256=request_commitment,
            payload_sha256=payloads["fresh-traffic"]["v1_to_proxy"],
            current_epoch=current_epoch,
            fresh_elapsed_ms=int(fresh["elapsed_ms"]),
            final_elapsed_ms=int(final["elapsed_ms"]),
        ),
        "proxy_to_v1": parse_bsc08_peer_receipt(
            raw_content["v1-peer-receipts"],
            direction="proxy-to-v1",
            request_commitment_sha256=request_commitment,
            payload_sha256=payloads["fresh-traffic"]["proxy_to_v1"],
            current_epoch=current_epoch,
            fresh_elapsed_ms=int(fresh["elapsed_ms"]),
            final_elapsed_ms=int(final["elapsed_ms"]),
        ),
    }
    return {
        "firmware": firmware,
        "queries": queries,
        "stimuli": stimuli,
        "deliveries": deliveries,
        "safety": safety,
        "resets": resets,
    }


def derive_bsc08_facts(
    queries: Sequence[Mapping[str, object]],
    safety: Mapping[str, object],
    deliveries: Mapping[str, object],
) -> dict[str, object]:
    responses = [query["response"] for query in queries]
    assert all(isinstance(response, dict) for response in responses)
    typed = [response for response in responses if isinstance(response, dict)]
    queue_corruption = False
    for response in typed:
        for field, expected_capacity in (("proxyQueue", 8), ("phoneQueue", 16)):
            queue = response[field]
            assert isinstance(queue, list)
            head, tail, count, capacity = queue
            queue_corruption = queue_corruption or (
                capacity != expected_capacity
                or head >= capacity
                or tail >= capacity
                or count > capacity
            )
    heap_pairs = [response["heap"] for response in typed]
    monotonic_heap_loss = all(
        later[0] < earlier[0] and later[1] < earlier[1]
        for earlier, later in zip(heap_pairs, heap_pairs[1:])
    )
    old_traffic = typed[BSC08_SNAPSHOT_PHASES.index("old-traffic")]
    fresh_traffic = typed[BSC08_SNAPSHOT_PHASES.index("fresh-traffic")]
    baseline = typed[0]
    deferred = int(
        baseline["releaseOpportunity"] is False
        and any(response["releaseOpportunity"] is True for response in typed[1:])
    )
    old_forwarded = any(response["oldForwarded"] is True for response in typed)
    delivery_rows = list(deliveries.values())
    fresh_resumed = (
        len(delivery_rows) == 2
        and all(isinstance(row, dict) for row in delivery_rows)
        and {row.get("direction") for row in delivery_rows if isinstance(row, dict)}
        == {"v1-to-proxy", "proxy-to-v1"}
        and all(row.get("delivered") is True for row in delivery_rows if isinstance(row, dict))
        and all(row.get("epoch") == fresh_traffic["epoch"] for row in delivery_rows if isinstance(row, dict))
    )
    return {
        "queue-index-corruption-observed": queue_corruption,
        "heap-corruption-observed": any(safety[field] is True for field in safety),
        "old-epoch-forwarded": old_forwarded,
        "deferred-release-opportunities": deferred,
        "fresh-bidirectional-traffic-resumed": fresh_resumed,
        "monotonic-heap-loss-observed": monotonic_heap_loss,
    }


def validate_bsc08_record(
    payload: object,
    *,
    expected: Mapping[str, object],
    raw_manifest: object,
    raw_content: Mapping[str, bytes],
    request: Mapping[str, object],
    qualifying: bool = True,
) -> dict[str, object]:
    code = "case_record_invalid"
    source = parse_bsc08_raw_evidence(
        raw_manifest=raw_manifest,
        raw_content=raw_content,
        request=request,
    )
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
            "descriptor",
            "firmware",
            "serial_queries",
            "stimuli",
            "barriers",
            "deliveries",
            "resets",
            "safety",
            "facts",
            "raw_artifact_manifest",
            "source_capture_binding_sha256",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-08 record",
    )
    for field in ("case_id", "role_id", "session_id", "attempt_id", "run_index", "target_sha", "dut_alias", "rig_alias"):
        if record.get(field) != expected.get(field):
            raise RunnerError(code, f"BSC-08 {field} does not match the runner invocation")
    if type(record.get("schema_version")) is not int or record.get("schema_version") != 1 or record.get("case_id") != BSC08_CASE_ID:
        raise RunnerError(code, "BSC-08 record identity is invalid")
    if type(record.get("run_index")) is not int or not 1 <= record["run_index"] <= BSC08_REQUIRED_RUNS:
        raise RunnerError(code, "BSC-08 run index is invalid")
    if type(record.get("hardware_observed")) is not bool:
        raise RunnerError(code, "BSC-08 hardware observation flag is untyped")
    if qualifying and (record.get("execution_mode") != "physical" or record.get("hardware_observed") is not True):
        raise RunnerError("case_evidence_nonqualifying", "BSC-08 simulated evidence cannot qualify the physical race")
    if not qualifying and record.get("execution_mode") not in {"physical", "simulated"}:
        raise RunnerError(code, "BSC-08 execution mode is invalid")
    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-08 start")
    completed = parse_runner_utc(record.get("completed_at_utc"), code=code, label="BSC-08 completion")
    if completed < started:
        raise RunnerError(code, "BSC-08 completion predates its start")
    if (
        not isinstance(record.get("target_sha"), str)
        or adapter_protocol.FULL_SHA_RE.fullmatch(record["target_sha"]) is None
    ):
        raise RunnerError(code, "BSC-08 target SHA is not a full lowercase Git SHA")
    if record.get("descriptor") != bsc08_profile_descriptor():
        raise RunnerError(code, "BSC-08 record does not bind the pinned descriptor")
    firmware = require_exact_object(
        record.get("firmware"),
        {"environment", "target_sha", "binary_sha256", "hil_fault_control_active"},
        code=code,
        label="BSC-08 firmware",
    )
    if (
        firmware.get("environment") != BSC08_PRODUCTION_ENVIRONMENT
        or firmware.get("target_sha") != record.get("target_sha")
        or firmware.get("hil_fault_control_active") is not False
    ):
        raise RunnerError(code, "BSC-08 firmware is not the bound production image")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-08 firmware binary")
    if firmware != source["firmware"]:
        raise RunnerError(code, "BSC-08 firmware record is not derived from collected build evidence")

    queries = validate_bsc08_queries(record.get("serial_queries"))
    if queries != source["queries"]:
        raise RunnerError(code, "BSC-08 serial observations are not derived from collected DUT bytes")
    duration_ms = int((completed - started).total_seconds() * 1000)
    if duration_ms <= 0 or queries[-1]["elapsed_ms"] > duration_ms:
        raise RunnerError(code, "BSC-08 serial timeline exceeds its UTC collection window")
    responses = [query["response"] for query in queries]
    assert all(isinstance(response, dict) for response in responses)
    typed = [response for response in responses if isinstance(response, dict)]
    baseline, streaming, disabled, reenabled, old_traffic, fresh_traffic, final = typed
    old_epoch = baseline["epoch"]
    new_epoch = reenabled["epoch"]
    if (
        type(old_epoch) is not int
        or old_epoch <= 0
        or streaming["epoch"] != old_epoch
        or baseline["gateEpoch"] != old_epoch
        or streaming["gateEpoch"] != old_epoch
        or disabled["epoch"] != old_epoch
        or disabled["gateEpoch"] != 0
        or type(new_epoch) is not int
        or new_epoch <= old_epoch
        or any(response["epoch"] != new_epoch or response["gateEpoch"] != new_epoch for response in typed[3:])
    ):
        raise RunnerError(code, "BSC-08 queue epoch lifecycle is invalid")
    if (
        baseline["activeOverlap"] is not False
        or baseline["releaseOpportunity"] is not False
        or streaming["activeOverlap"] is not False
        or streaming["releaseOpportunity"] is not False
    ):
        raise RunnerError(code, "BSC-08 natural race observations were already latched before disable")
    if not all(stream > base for base, stream in zip(baseline["callbackEntries"], streaming["callbackEntries"], strict=True)):
        raise RunnerError("case_incomplete", "BSC-08 bidirectional callback stream was not observed")
    if old_traffic["admissions"] != reenabled["admissions"] or not all(
        old > new for new, old in zip(reenabled["staleRejects"], old_traffic["staleRejects"], strict=True)
    ):
        raise RunnerError(code, "BSC-08 old-epoch traffic was not rejected in both directions")
    if not all(
        fresh > old for old, fresh in zip(old_traffic["admissions"], fresh_traffic["admissions"], strict=True)
    ):
        raise RunnerError("case_incomplete", "BSC-08 fresh bidirectional traffic did not resume")
    if disabled["activeOverlap"] is not True or disabled["releaseOpportunity"] is not True:
        raise RunnerError("case_incomplete", "BSC-08 natural callback/release race was not observed")
    if any(response["oldForwarded"] is True for response in typed):
        raise RunnerError(code, "BSC-08 old-epoch forwarding was observed")
    lifecycle_before = baseline["lifecycle"]
    lifecycle_after = reenabled["lifecycle"]
    if (
        lifecycle_after[0] != lifecycle_before[0] + 1
        or lifecycle_after[1] != lifecycle_before[1] + 1
        or lifecycle_after[2] < lifecycle_before[2] + 1
        or lifecycle_after[3] != lifecycle_before[3] + 1
    ):
        raise RunnerError(code, "BSC-08 allocation/disable/release/re-enable lifecycle is invalid")
    if final["epoch"] != new_epoch or final["gateEpoch"] != new_epoch:
        raise RunnerError(code, "BSC-08 final gate does not admit the fresh epoch")

    stimuli = record.get("stimuli")
    if stimuli != source["stimuli"]:
        raise RunnerError(code, "BSC-08 stimuli are not derived from the collected adapter transcript")
    stimulus_phases = ("streaming", "disabled", "reenabled", "old-traffic", "fresh-traffic")
    if not isinstance(stimuli, list) or len(stimuli) != len(BSC08_STIMULUS_IDS):
        raise RunnerError(code, "BSC-08 stimulus timeline is incomplete")
    query_by_phase = {query["phase"]: query for query in queries}
    for sequence, (raw, stimulus_id, phase) in enumerate(zip(stimuli, BSC08_STIMULUS_IDS, stimulus_phases, strict=True), start=1):
        row = require_exact_object(raw, {"id", "sequence", "elapsed_ms", "observed"}, code=code, label="BSC-08 stimulus")
        if row != {"id": stimulus_id, "sequence": sequence, "elapsed_ms": query_by_phase[phase]["elapsed_ms"], "observed": True}:
            raise RunnerError(code, "BSC-08 stimulus is not bound to its serial snapshot")

    barriers = record.get("barriers")
    if not isinstance(barriers, list) or len(barriers) != 2:
        raise RunnerError(code, "BSC-08 barrier evidence is incomplete")
    first_active = next(query for query in queries if query["response"]["activeOverlap"] is True)
    first_release = next(query for query in queries if query["response"]["releaseOpportunity"] is True)
    for sequence, (raw, barrier_id, source_query) in enumerate(
        zip(barriers, BSC08_BARRIER_IDS, (first_active, first_release), strict=True),
        start=1,
    ):
        row = require_exact_object(raw, {"id", "sequence", "elapsed_ms", "observed", "timed_out"}, code=code, label="BSC-08 barrier")
        if row != {"id": barrier_id, "sequence": sequence, "elapsed_ms": source_query["elapsed_ms"], "observed": True, "timed_out": False}:
            raise RunnerError(code, "BSC-08 barrier is not bound to the first natural observation")

    deliveries = require_exact_object(
        record.get("deliveries"),
        {"proxy_to_v1", "v1_to_proxy"},
        code=code,
        label="BSC-08 deliveries",
    )
    if deliveries != source["deliveries"]:
        raise RunnerError(code, "BSC-08 deliveries are not derived from collected peer receipts")
    resets = require_exact_object(record.get("resets"), {"expected_kind", "planned", "observed", "unexpected"}, code=code, label="BSC-08 resets")
    if resets != {"expected_kind": "none", "planned": 0, "observed": 0, "unexpected": 0}:
        raise RunnerError(code, "BSC-08 reset contract is invalid")
    if resets != source["resets"]:
        raise RunnerError(code, "BSC-08 resets are not derived from the collected safety trace")
    safety = require_exact_object(record.get("safety"), {"panic_observed", "watchdog_reset_observed", "load_prohibited_observed"}, code=code, label="BSC-08 safety")
    if any(type(value) is not bool for value in safety.values()):
        raise RunnerError(code, "BSC-08 safety observations must be typed booleans")
    if safety != source["safety"]:
        raise RunnerError(code, "BSC-08 safety is not derived from the collected safety trace")
    derived = derive_bsc08_facts(queries, safety, deliveries)
    facts = require_exact_object(record.get("facts"), set(BSC08_FACT_IDS), code=code, label="BSC-08 facts")
    for field in BSC08_FACT_IDS:
        expected_type = int if field == "deferred-release-opportunities" else bool
        if type(facts[field]) is not expected_type:
            raise RunnerError(code, "BSC-08 facts must preserve exact boolean/integer types")
    if facts != derived:
        raise RunnerError(code, "BSC-08 facts were not derived from runner-owned observations")
    expected_facts = {
        "queue-index-corruption-observed": False,
        "heap-corruption-observed": False,
        "old-epoch-forwarded": False,
        "deferred-release-opportunities": 1,
        "fresh-bidirectional-traffic-resumed": True,
        "monotonic-heap-loss-observed": False,
    }
    if facts != expected_facts:
        raise RunnerError(code, "BSC-08 acceptance facts did not pass")
    expected_request = expected.get("request_commitment_sha256")
    if not isinstance(expected_request, str):
        raise RunnerError(code, "BSC-08 runner invocation lacks its adapter request commitment")
    if request.get("request_commitment_sha256") != expected_request:
        raise RunnerError(code, "BSC-08 adapter request was substituted")
    if record.get("raw_artifact_manifest") != raw_manifest:
        raise RunnerError(code, "BSC-08 record does not embed the runner-collected manifest")
    source_binding = require_sha256(
        record.get("source_capture_binding_sha256"),
        code=code,
        label="BSC-08 source-capture binding",
    )
    if not secrets.compare_digest(source_binding, bsc08_source_capture_commitment(record)):
        raise RunnerError(code, "BSC-08 typed observations are not bound to their raw captures")
    binding = require_sha256(record.get("evidence_binding_sha256"), code=code, label="BSC-08 evidence binding")
    if not secrets.compare_digest(binding, bsc08_record_commitment(record)):
        raise RunnerError(code, "BSC-08 evidence binding is stale")
    return record


def build_bsc08_record_from_raw(
    *,
    expected: Mapping[str, object],
    request: Mapping[str, object],
    raw_manifest: object,
    raw_content: Mapping[str, bytes],
    started_at_utc: str,
    completed_at_utc: str,
) -> dict[str, object]:
    source = parse_bsc08_raw_evidence(
        raw_manifest=raw_manifest,
        raw_content=raw_content,
        request=request,
    )
    queries = source["queries"]
    assert isinstance(queries, list)
    first_active = next(
        (query for query in queries if query["response"]["activeOverlap"] is True),
        None,
    )
    first_release = next(
        (query for query in queries if query["response"]["releaseOpportunity"] is True),
        None,
    )
    if first_active is None or first_release is None:
        raise RunnerError("case_incomplete", "BSC-08 natural callback/release race was not observed")
    deliveries = source["deliveries"]
    safety = source["safety"]
    assert isinstance(deliveries, dict) and isinstance(safety, dict)
    record: dict[str, object] = {
        "schema_version": 1,
        "case_id": BSC08_CASE_ID,
        "role_id": "proxy-epoch-teardown",
        "session_id": expected["session_id"],
        "attempt_id": expected["attempt_id"],
        "run_index": expected["run_index"],
        "target_sha": expected["target_sha"],
        "dut_alias": expected["dut_alias"],
        "rig_alias": expected["rig_alias"],
        "execution_mode": expected["execution_mode"],
        "hardware_observed": expected["hardware_observed"],
        "started_at_utc": started_at_utc,
        "completed_at_utc": completed_at_utc,
        "descriptor": bsc08_profile_descriptor(),
        "firmware": source["firmware"],
        "serial_queries": queries,
        "stimuli": source["stimuli"],
        "barriers": [
            {
                "id": barrier_id,
                "sequence": sequence,
                "elapsed_ms": query["elapsed_ms"],
                "observed": True,
                "timed_out": False,
            }
            for sequence, (barrier_id, query) in enumerate(
                zip(BSC08_BARRIER_IDS, (first_active, first_release), strict=True), start=1
            )
        ],
        "deliveries": deliveries,
        "resets": source["resets"],
        "safety": safety,
        "facts": derive_bsc08_facts(queries, safety, deliveries),
        "raw_artifact_manifest": raw_manifest,
        "source_capture_binding_sha256": "0" * 64,
        "evidence_binding_sha256": "0" * 64,
    }
    record["source_capture_binding_sha256"] = bsc08_source_capture_commitment(record)
    record["evidence_binding_sha256"] = bsc08_record_commitment(record)
    return validate_bsc08_record(
        record,
        expected=expected,
        raw_manifest=raw_manifest,
        raw_content=raw_content,
        request=request,
        qualifying=expected.get("execution_mode") == "physical",
    )


def validate_bsc08_distinct_runs(records: Sequence[Mapping[str, object]]) -> None:
    if len(records) != BSC08_REQUIRED_RUNS:
        raise RunnerError("case_runs_incomplete", "BSC-08 requires exactly three runs")
    for field in ("attempt_id", "source_capture_binding_sha256", "evidence_binding_sha256"):
        if len({record[field] for record in records}) != BSC08_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", "BSC-08 run identities must be distinct")
    firmware = records[0]["firmware"]
    if any(record["firmware"] != firmware for record in records[1:]):
        raise RunnerError("case_runs_mixed", "BSC-08 runs must use one production firmware")
    generations: set[object] = set()
    nonces: set[object] = set()
    artifact_digests: set[object] = set()
    for record in records:
        queries = record["serial_queries"]
        assert isinstance(queries, list)
        run_generations = {query["response"]["epoch"] for query in queries}
        if generations.intersection(run_generations):
            raise RunnerError("case_runs_reused", "BSC-08 queue generations must be distinct across runs")
        generations.update(run_generations)
        run_nonces = {query["nonce"] for query in queries}
        if nonces.intersection(run_nonces):
            raise RunnerError("case_runs_reused", "BSC-08 serial nonces must be distinct across runs")
        nonces.update(run_nonces)
        manifest = record["raw_artifact_manifest"]
        assert isinstance(manifest, dict)
        artifacts = manifest["artifacts"]
        assert isinstance(artifacts, list)
        run_digests = {artifact["sha256"] for artifact in artifacts}
        if artifact_digests.intersection(run_digests):
            raise RunnerError("case_runs_reused", "BSC-08 raw receipts must be distinct across runs")
        artifact_digests.update(run_digests)


def resolve_bsc08_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError("local_inventory_missing", "BSC-08 requires the ignored local hardware inventory")
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
        required_capabilities=BSC08_DUT_CAPABILITIES,
        port_records=port_records,
    )
    _, rig_attestation = bsc03_board_attestation(
        inventory=inventory,
        alias=args.rig,
        required_capabilities=BSC08_RIG_CAPABILITIES,
        port_records=port_records,
    )
    if args.board == args.rig:
        raise RunnerError("case_alias_reused", "BSC-08 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def build_bsc08_production_firmware(
    *,
    repository: Path,
    pio_executable: Path,
    environment: Mapping[str, str],
) -> tuple[Path, str]:
    try:
        completed = subprocess.run(
            [str(pio_executable), "run", "-e", BSC08_PRODUCTION_ENVIRONMENT],
            cwd=repository,
            env=dict(environment),
            capture_output=True,
            check=False,
            timeout=BSC08_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("firmware_build_timeout", "BSC-08 production build exceeded its timeout") from exc
    except OSError as exc:
        raise RunnerError("firmware_build_unavailable", "BSC-08 production build could not start") from exc
    if completed.returncode != 0:
        raise RunnerError("firmware_build_failed", "BSC-08 production firmware build failed")
    binary = repository / ".pio" / "build" / BSC08_PRODUCTION_ENVIRONMENT / "firmware.bin"
    require_no_symlink_components(binary, boundary=repository)
    try:
        metadata = os.stat(binary, follow_symlinks=False)
    except OSError as exc:
        raise RunnerError("firmware_build_invalid", "BSC-08 production binary is unavailable") from exc
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1 or metadata.st_size <= 0:
        raise RunnerError("firmware_build_invalid", "BSC-08 production binary identity is invalid")
    return binary, sha256_file(binary)


def run_bsc08_adapter(
    *,
    adapter_path: Path,
    adapter: rig_adapters.RigAdapter,
    repository: Path,
    request: Mapping[str, object],
    expected: Mapping[str, object],
    raw_directory: Path,
    raw_manifest_path: Path,
    environment: Mapping[str, str],
) -> tuple[dict[str, object], dict[str, object]]:
    if adapter.entrypoint is None:
        raise RunnerError("adapter_contract_invalid", "BSC-08 adapter entrypoint is unavailable")
    command_started = datetime.now(timezone.utc)
    try:
        completed = subprocess.run(
            [
                sys.executable,
                str(adapter_path),
                "--entrypoint",
                adapter.entrypoint,
                "--raw-artifact-dir",
                str(raw_directory),
            ],
            cwd=repository,
            env=dict(environment),
            input=adapter_protocol.canonical_json_bytes(request) + b"\n",
            capture_output=True,
            check=False,
            timeout=BSC08_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-08 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-08 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-08 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 16 * 1024 or len(completed.stderr) > 64 * 1024:
        raise RunnerError("protocol_invalid", "BSC-08 adapter output size is invalid")
    try:
        adapter_protocol.parse_adapter_response(completed.stdout, request=request)
        role = next(role for role in adapter.roles if role.role_id == "proxy-epoch-teardown")
        raw_manifest = adapter_protocol.collect_raw_artifacts(
            raw_directory=raw_directory,
            role=role,
            request_commitment_sha256=str(request["request_commitment_sha256"]),
            manifest_path=raw_manifest_path,
        )
        raw_content = adapter_protocol.read_collected_raw_artifacts(
            raw_directory=raw_directory,
            role=role,
            manifest=raw_manifest,
        )
    except adapter_protocol.AdapterProtocolError as exc:
        raise RunnerError(exc.code, exc.message) from exc
    record = build_bsc08_record_from_raw(
        expected=expected,
        request=request,
        raw_manifest=raw_manifest,
        raw_content=raw_content,
        started_at_utc=command_started.isoformat(timespec="microseconds").replace("+00:00", "Z"),
        completed_at_utc=command_completed.isoformat(timespec="microseconds").replace("+00:00", "Z"),
    )
    return record, raw_manifest


def _bsc08_test_adapter_descriptor(
    adapter: rig_adapters.RigAdapter,
) -> rig_adapters.RigAdapter:
    return replace(
        adapter,
        status="implemented",
        source_path="scripts/bug_squash_hil_bsc08_test_adapter.py",
        entrypoint="main",
    )


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
    role_key = "production_replay" if args.production_replay else "scenario"
    role_contract = case_contract.get(role_key)
    if not isinstance(role_contract, dict) or not isinstance(role_contract.get("role_id"), str):
        raise RunnerError("adapter_contract_invalid", f"{case_id} role contract is unavailable")
    admit_case_rig_adapter(
        args,
        case_contract=case_contract,
        role_id=role_contract["role_id"],
    )
    raise RunnerError(
        "case_rig_adapter_unavailable",
        f"{case_id} physical execution remains blocked until its tracked rig adapter exists",
    )

def load_bsc12_case_descriptor() -> dict[str, object]:
    profile, errors = qualification.load_pinned_profile()
    if (
        profile is None
        or errors
        or profile.get("profile_id") != "bug-squash-hil-v1"
        or profile.get("profile_version") != BSC12_PROFILE_VERSION
    ):
        raise RunnerError(
            "qualification_profile_invalid",
            "BSC-12 requires the exact pinned profile-v5 contract",
        )
    matches = [
        case for case in profile["required_cases"] if case.get("id") == BSC12_CASE_ID
    ]
    if len(matches) != 1:
        raise RunnerError("qualification_profile_invalid", "BSC-12 descriptor is unavailable")
    descriptor = matches[0]
    if (
        set(descriptor)
        != {
            "id",
            "minimum_runs",
            "fault_build_required",
            "production_replay_required",
            "required_dut_capabilities",
            "required_rig_capabilities",
            "scenario",
            "production_replay",
        }
        or descriptor.get("minimum_runs") != BSC12_REQUIRED_RUNS
        or descriptor.get("fault_build_required") is not False
        or descriptor.get("production_replay_required") is not False
        or descriptor.get("required_dut_capabilities") != list(BSC12_DUT_CAPABILITIES)
        or descriptor.get("required_rig_capabilities") != list(BSC12_RIG_CAPABILITIES)
        or descriptor.get("production_replay") is not None
    ):
        raise RunnerError("qualification_profile_invalid", "BSC-12 case contract drifted")
    bsc12_role_descriptor(descriptor)
    return descriptor


def bsc12_role_descriptor(case_descriptor: Mapping[str, object]) -> dict[str, object]:
    raw = case_descriptor.get("scenario")
    if not isinstance(raw, dict):
        raise RunnerError("qualification_profile_invalid", "BSC-12 role descriptor is missing")
    role = dict(raw)
    expected_facts = [
        {"id": "poweroff-returned-false", "type": "boolean", "expected": True},
        {"id": "disconnected-screen-restored", "type": "boolean", "expected": True},
        {"id": "session-marker-unclean-after-reset", "type": "boolean", "expected": True},
        {"id": "settings-writer-count", "type": "integer", "minimum": 1, "maximum": 1},
        {"id": "bond-writer-count", "type": "integer", "minimum": 1, "maximum": 1},
        {"id": "setting-survived-reset", "type": "boolean", "expected": True},
        {"id": "bond-survived-reset", "type": "boolean", "expected": True},
        {"id": "real-rtc-wake-input-observed", "type": "boolean", "expected": True},
    ]
    if (
        set(role)
        != {
            "role_id",
            "schema",
            "build_kind",
            "stimulus_ids",
            "fault_ids",
            "barrier_ids",
            "vbus_isolation_required",
            "reset_contract",
            "facts",
        }
        or role.get("role_id") != "aborted-shutdown-recovery"
        or role.get("schema") != "case-observation-v1"
        or role.get("build_kind") != "production"
        or role.get("stimulus_ids") != list(BSC12_STIMULUS_IDS)
        or role.get("fault_ids") != []
        or role.get("barrier_ids") != list(BSC12_BARRIER_IDS)
        or role.get("vbus_isolation_required") is not True
        or role.get("reset_contract")
        != {"expected_kind": "forced-reset", "expected_count": 1, "unexpected_count": 0}
        or role.get("facts") != expected_facts
    ):
        raise RunnerError("qualification_profile_invalid", "BSC-12 role contract drifted")
    return role


def bsc12_descriptor_commitment(case_descriptor: Mapping[str, object]) -> str:
    return canonical_case_commitment("v1simple.bsc12.case-descriptor.v1", case_descriptor)


def bsc12_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc12.case-record.v1", committed)


def validate_bsc12_raw_artifacts(
    manifest_value: object,
    adapter_manifest_value: object,
    commitments_value: object,
    content_by_role: Mapping[str, bytes],
    *,
    expected_request_sha256: str,
) -> dict[str, object]:
    """Bind typed evidence to the exact bytes in a runner-owned manifest."""

    code = "case_record_invalid"
    contracts = rig_adapters.BSC12_RAW_ARTIFACTS
    manifest = require_exact_object(
        manifest_value,
        {
            "schema_version",
            "protocol_version",
            "request_commitment_sha256",
            "artifacts",
            "manifest_commitment_sha256",
        },
        code=code,
        label="BSC-12 runner raw artifact manifest",
    )
    runner_rows = manifest.get("artifacts")
    if (
        type(manifest.get("schema_version")) is not int
        or manifest.get("schema_version") != adapter_protocol.SCHEMA_VERSION
        or type(manifest.get("protocol_version")) is not int
        or manifest.get("protocol_version") != rig_adapters.ADAPTER_PROTOCOL_VERSION
        or manifest.get("request_commitment_sha256") != expected_request_sha256
        or not isinstance(runner_rows, list)
        or len(runner_rows) != len(contracts)
    ):
        raise RunnerError(code, "BSC-12 runner raw artifact manifest identity is invalid")
    manifest_commitment = require_sha256(
        manifest.get("manifest_commitment_sha256"), code=code, label="BSC-12 raw manifest"
    )
    uncommitted_manifest = dict(manifest)
    uncommitted_manifest.pop("manifest_commitment_sha256")
    if not secrets.compare_digest(
        manifest_commitment,
        adapter_protocol.canonical_commitment(adapter_protocol.MANIFEST_DOMAIN, uncommitted_manifest),
    ):
        raise RunnerError(code, "BSC-12 runner raw artifact manifest binding is stale")
    if not isinstance(adapter_manifest_value, list) or len(adapter_manifest_value) != len(contracts):
        raise RunnerError(code, "BSC-12 adapter raw artifact manifest is incomplete")
    if set(content_by_role) != {artifact.role for artifact in contracts}:
        raise RunnerError(code, "BSC-12 verified raw artifact content is incomplete")
    commitments = require_exact_object(
        commitments_value,
        set(BSC12_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-12 capture commitments",
    )

    parsed: dict[str, object] = {}
    observed_hashes: list[str] = []
    for index, (contract, commitment_field) in enumerate(
        zip(contracts, BSC12_CAPTURE_COMMITMENTS, strict=True)
    ):
        runner_row = require_exact_object(
            runner_rows[index],
            {"role", "filename", "size_bytes", "sha256"},
            code=code,
            label="BSC-12 runner raw artifact manifest row",
        )
        adapter_row = require_exact_object(
            adapter_manifest_value[index],
            {"role", "filename", "bytes", "sha256"},
            code=code,
            label="BSC-12 adapter raw artifact manifest row",
        )
        digest = require_sha256(
            runner_row.get("sha256"), code=code, label="BSC-12 runner raw artifact"
        )
        size = runner_row.get("size_bytes")
        if (
            runner_row.get("role") != contract.role
            or runner_row.get("filename") != contract.filename
            or type(size) is not int
            or not 1 <= size <= contract.maximum_bytes
            or adapter_row.get("role") != contract.role
            or adapter_row.get("filename") != contract.filename
            or type(adapter_row.get("bytes")) is not int
            or adapter_row.get("bytes") != size
            or adapter_row.get("sha256") != digest
            or commitments.get(commitment_field) != digest
        ):
            raise RunnerError(code, "BSC-12 raw artifact manifest does not match collected bytes")
        observed_hashes.append(digest)
        if contract.role == "serial-log":
            try:
                parsed[contract.role] = content_by_role[contract.role].decode(
                    "utf-8", errors="strict"
                )
            except UnicodeError as exc:
                raise RunnerError(code, "BSC-12 serial capture is unreadable") from exc
        else:
            parsed[contract.role] = read_json_bytes(
                content_by_role[contract.role], f"BSC-12 {contract.role}"
            )
    if len(set(observed_hashes)) != len(observed_hashes):
        raise RunnerError(code, "BSC-12 evidence roles reused collected bytes")
    return parsed


def parse_bsc12_serial_events(value: object, *, duration_ms: int) -> dict[str, int]:
    code = "case_record_invalid"
    if not isinstance(value, str) or not value.endswith("\n"):
        raise RunnerError(code, "BSC-12 serial capture is incomplete")
    events: dict[str, int] = {}
    allowed = {
        "shutdown-begin",
        "handoff-begin",
        "wake-asserted",
        "power-off-returned-false",
        "screen-restored-disconnected",
        "marker-rewritten-unclean",
        "settings-writer-complete",
        "bond-writer-complete",
        "reset-forced",
        "boot-observed",
        "setting-readback",
        "bond-readback",
        "panic",
        "watchdog-reset",
        "load-prohibited",
    }
    previous = -1
    for line in value.splitlines():
        try:
            row = json.loads(line, object_pairs_hook=reject_duplicate_json_keys)
        except (json.JSONDecodeError, ValueError) as exc:
            raise RunnerError(code, "BSC-12 serial capture is not strict JSONL") from exc
        row = require_exact_object(
            row,
            {"event", "elapsed_ms"},
            code=code,
            label="BSC-12 serial event",
        )
        event = row.get("event")
        elapsed = row.get("elapsed_ms")
        if (
            not isinstance(event, str)
            or event not in allowed
            or event in events
            or type(elapsed) is not int
            or not 0 <= elapsed <= duration_ms
            or elapsed < previous
        ):
            raise RunnerError(code, "BSC-12 serial event identity or timing is invalid")
        events[event] = elapsed
        previous = elapsed
    required = allowed - {"panic", "watchdog-reset", "load-prohibited"}
    if not required.issubset(events):
        raise RunnerError(code, "BSC-12 serial capture is missing required events")
    return events


def resolve_bsc12_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
    case_descriptor: Mapping[str, object],
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-12 requires the ignored local hardware inventory",
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
        raise RunnerError("case_alias_reused", "BSC-12 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def validate_bsc12_stimuli(value: object, *, duration_ms: int) -> dict[str, int]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(BSC12_STIMULUS_IDS):
        raise RunnerError(code, "BSC-12 stimulus sequence is incomplete")
    elapsed_by_id: dict[str, int] = {}
    previous = -1
    for sequence, (raw, stimulus_id) in enumerate(
        zip(value, BSC12_STIMULUS_IDS, strict=True), start=1
    ):
        row = require_exact_object(
            raw,
            {"id", "sequence", "elapsed_ms", "result"},
            code=code,
            label="BSC-12 stimulus",
        )
        elapsed = row.get("elapsed_ms")
        if (
            row.get("id") != stimulus_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or not 0 <= elapsed <= duration_ms
            or elapsed <= previous
            or row.get("result") != "pass"
        ):
            raise RunnerError(code, "BSC-12 stimulus identity, timing, or result is invalid")
        elapsed_by_id[stimulus_id] = elapsed
        previous = elapsed
    return elapsed_by_id


def validate_bsc12_barriers(
    value: object,
    *,
    duration_ms: int,
) -> list[dict[str, object]]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(BSC12_BARRIER_IDS):
        raise RunnerError(code, "BSC-12 barrier coverage is incomplete")
    expected_sources = ("rtc-gpio-wake-input", "shutdown-persistence-writers")
    barriers: list[dict[str, object]] = []
    previous_release = -1
    for sequence, (raw, barrier_id, source) in enumerate(
        zip(value, BSC12_BARRIER_IDS, expected_sources, strict=True), start=1
    ):
        row = require_exact_object(
            raw,
            {
                "id",
                "source",
                "sequence",
                "ready_elapsed_ms",
                "released_elapsed_ms",
                "timed_out",
            },
            code=code,
            label="BSC-12 barrier",
        )
        ready = row.get("ready_elapsed_ms")
        released = row.get("released_elapsed_ms")
        if (
            row.get("id") != barrier_id
            or row.get("source") != source
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(ready) is not int
            or type(released) is not int
            or not 0 <= ready <= released <= duration_ms
            or ready <= previous_release
            or row.get("timed_out") is not False
        ):
            raise RunnerError(code, "BSC-12 barrier source, order, or timing is invalid")
        barriers.append(row)
        previous_release = released
    return barriers


def validate_bsc12_shutdown_observation(
    value: object,
    *,
    stimuli: Mapping[str, int],
    duration_ms: int,
) -> dict[str, object]:
    code = "case_record_invalid"
    observation = require_exact_object(
        value,
        {
            "shutdown_begin_elapsed_ms",
            "handoff_begin_elapsed_ms",
            "wake_asserted_elapsed_ms",
            "power_off_return_elapsed_ms",
            "screen_restored_elapsed_ms",
            "marker_rewritten_elapsed_ms",
            "wake_input_source",
            "wake_trigger",
            "real_rtc_wake_input",
            "power_off_result",
            "screen_state",
            "marker_state",
            "shutdown_timeline_sha256",
            "wake_input_trace_sha256",
            "serial_log_sha256",
        },
        code=code,
        label="BSC-12 shutdown observation",
    )
    times = tuple(
        observation.get(field)
        for field in (
            "shutdown_begin_elapsed_ms",
            "handoff_begin_elapsed_ms",
            "wake_asserted_elapsed_ms",
            "power_off_return_elapsed_ms",
            "screen_restored_elapsed_ms",
            "marker_rewritten_elapsed_ms",
        )
    )
    if (
        any(type(item) is not int for item in times)
        or not 0 <= times[0] <= times[1] <= times[2] < times[3] <= times[4] <= times[5] <= duration_ms
        or times[0] != stimuli["begin-portable-shutdown"]
        or times[2] != stimuli["assert-wake-input"]
        or times[5] >= stimuli["mutate-setting"]
        or observation.get("wake_input_source") != "rtc-gpio-wake-input"
        or observation.get("wake_trigger") != "active-low"
        or observation.get("real_rtc_wake_input") is not True
        or observation.get("power_off_result") is not False
        or observation.get("screen_state") != "disconnected"
        or observation.get("marker_state") != "unclean"
    ):
        raise RunnerError(code, "BSC-12 shutdown handoff or abort recovery is invalid")
    return observation


def validate_bsc12_writers(
    value: object,
    *,
    stimuli: Mapping[str, int],
    duration_ms: int,
) -> list[dict[str, object]]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(BSC12_WRITER_SOURCES):
        raise RunnerError(code, "BSC-12 writer completion evidence is incomplete")
    rows: list[dict[str, object]] = []
    mutation_ids = ("mutate-setting", "mutate-bond")
    for sequence, (raw, (writer_id, source), mutation_id) in enumerate(
        zip(value, BSC12_WRITER_SOURCES, mutation_ids, strict=True), start=1
    ):
        row = require_exact_object(
            raw,
            {
                "writer_id",
                "source",
                "sequence",
                "mutation_elapsed_ms",
                "completion_elapsed_ms",
                "completion_count",
                "duplicate_count",
                "lost_count",
                "stalled",
            },
            code=code,
            label="BSC-12 writer",
        )
        mutation_elapsed = row.get("mutation_elapsed_ms")
        completion_elapsed = row.get("completion_elapsed_ms")
        if (
            row.get("writer_id") != writer_id
            or row.get("source") != source
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(mutation_elapsed) is not int
            or mutation_elapsed != stimuli[mutation_id]
            or type(completion_elapsed) is not int
            or not stimuli["wait-for-writers"] <= completion_elapsed < stimuli["force-reset"]
            or completion_elapsed > duration_ms
            or type(row.get("completion_count")) is not int
            or row.get("completion_count") != 1
            or type(row.get("duplicate_count")) is not int
            or row.get("duplicate_count") != 0
            or type(row.get("lost_count")) is not int
            or row.get("lost_count") != 0
            or row.get("stalled") is not False
        ):
            raise RunnerError(code, "BSC-12 writer source, cardinality, or completion is invalid")
        rows.append(row)
    return rows


def validate_bsc12_persistence(value: object) -> dict[str, object]:
    code = "case_record_invalid"
    observation = require_exact_object(
        value,
        {
            "source",
            "setting_mutation_sha256",
            "setting_after_reset_sha256",
            "bond_mutation_sha256",
            "bond_after_reset_sha256",
            "session_marker_after_reset",
            "before_readback_elapsed_ms",
            "after_readback_elapsed_ms",
            "persistence_before_sha256",
            "persistence_after_sha256",
        },
        code=code,
        label="BSC-12 persistence",
    )
    hashes = {
        field: require_sha256(observation.get(field), code=code, label=f"BSC-12 {field}")
        for field in (
            "setting_mutation_sha256",
            "setting_after_reset_sha256",
            "bond_mutation_sha256",
            "bond_after_reset_sha256",
        )
    }
    before_readback = observation.get("before_readback_elapsed_ms")
    after_readback = observation.get("after_readback_elapsed_ms")
    if (
        observation.get("source") != "post-reset-persistence-readback"
        or observation.get("session_marker_after_reset") != "unclean"
        or type(before_readback) is not int
        or type(after_readback) is not int
        or before_readback < 0
        or after_readback <= before_readback
        or require_sha256(
            observation.get("persistence_before_sha256"),
            code=code,
            label="BSC-12 persistence-before capture",
        )
        == require_sha256(
            observation.get("persistence_after_sha256"),
            code=code,
            label="BSC-12 persistence-after capture",
        )
        or not secrets.compare_digest(
            hashes["setting_mutation_sha256"], hashes["setting_after_reset_sha256"]
        )
        or not secrets.compare_digest(
            hashes["bond_mutation_sha256"], hashes["bond_after_reset_sha256"]
        )
    ):
        raise RunnerError(code, "BSC-12 post-reset persistence observation is invalid")
    return observation


def validate_bsc12_reset(
    value: object,
    *,
    reset_contract: Mapping[str, object],
    stimuli: Mapping[str, int],
    duration_ms: int,
) -> dict[str, object]:
    code = "case_record_invalid"
    reset = require_exact_object(
        value,
        {
            "expected_kind",
            "source",
            "planned",
            "observed",
            "unexpected",
            "forced_elapsed_ms",
            "boot_observed_elapsed_ms",
            "panic_observed",
            "watchdog_reset_observed",
            "load_prohibited_observed",
            "power_reset_trace_sha256",
            "serial_log_sha256",
        },
        code=code,
        label="BSC-12 reset",
    )
    forced = reset.get("forced_elapsed_ms")
    boot = reset.get("boot_observed_elapsed_ms")
    require_sha256(
        reset.get("power_reset_trace_sha256"),
        code=code,
        label="BSC-12 power/reset capture",
    )
    require_sha256(reset.get("serial_log_sha256"), code=code, label="BSC-12 reset serial capture")
    if (
        reset.get("expected_kind") != reset_contract.get("expected_kind")
        or reset.get("source") != "rig-forced-reset"
        or type(reset.get("planned")) is not int
        or reset.get("planned") != reset_contract.get("expected_count")
        or type(reset.get("observed")) is not int
        or reset.get("observed") != reset_contract.get("expected_count")
        or type(reset.get("unexpected")) is not int
        or reset.get("unexpected") != reset_contract.get("unexpected_count")
        or type(forced) is not int
        or forced != stimuli["force-reset"]
        or type(boot) is not int
        or not forced < boot <= duration_ms
        or type(reset.get("panic_observed")) is not bool
        or reset.get("panic_observed") is not False
        or type(reset.get("watchdog_reset_observed")) is not bool
        or reset.get("watchdog_reset_observed") is not False
        or type(reset.get("load_prohibited_observed")) is not bool
        or reset.get("load_prohibited_observed") is not False
    ):
        raise RunnerError(code, "BSC-12 reset or crash evidence violates the pinned contract")
    return reset


def validate_bsc12_safety_observation(value: object) -> dict[str, object]:
    code = "case_record_invalid"
    observation = require_exact_object(
        value,
        {
            "source",
            "vbus_isolated",
            "vbus_verified_elapsed_ms",
            "destructive_reset_triggered",
            "power_reset_trace_sha256",
        },
        code=code,
        label="BSC-12 safety observation",
    )
    if (
        observation.get("source") != "rig-power-reset-trace"
        or observation.get("vbus_isolated") is not True
        or type(observation.get("vbus_verified_elapsed_ms")) is not int
        or observation.get("vbus_verified_elapsed_ms") < 0
        or observation.get("destructive_reset_triggered") is not True
    ):
        raise RunnerError(code, "BSC-12 VBUS/reset safety observation is invalid")
    require_sha256(
        observation.get("power_reset_trace_sha256"),
        code=code,
        label="BSC-12 safety capture",
    )
    return observation


def validate_bsc12_source_evidence(
    raw: Mapping[str, object],
    *,
    commitments: Mapping[str, object],
    firmware: Mapping[str, object],
    safety: Mapping[str, object],
    shutdown: Mapping[str, object],
    writers: Sequence[Mapping[str, object]],
    persistence: Mapping[str, object],
    reset: Mapping[str, object],
    duration_ms: int,
) -> dict[str, object]:
    """Derive the safety and persistence facts from the captured role bytes."""

    code = "case_record_invalid"
    firmware_raw = require_exact_object(
        raw.get("firmware-build"),
        {"schema_version", "source", "firmware"},
        code=code,
        label="BSC-12 firmware build capture",
    )
    if (
        firmware_raw.get("schema_version") != 1
        or type(firmware_raw.get("schema_version")) is not int
        or firmware_raw.get("source") != "platformio-production-build"
        or firmware_raw.get("firmware") != firmware
    ):
        raise RunnerError(code, "BSC-12 firmware build capture is not bound to the record")

    before = require_exact_object(
        raw.get("persistence-before"),
        {
            "schema_version",
            "source",
            "captured_elapsed_ms",
            "setting_sha256",
            "bond_sha256",
            "session_marker",
        },
        code=code,
        label="BSC-12 pre-reset persistence capture",
    )
    after = require_exact_object(
        raw.get("persistence-after"),
        {
            "schema_version",
            "source",
            "captured_elapsed_ms",
            "setting_sha256",
            "bond_sha256",
            "session_marker",
        },
        code=code,
        label="BSC-12 post-reset persistence capture",
    )
    before_elapsed = before.get("captured_elapsed_ms")
    after_elapsed = after.get("captured_elapsed_ms")
    before_setting = require_sha256(
        before.get("setting_sha256"), code=code, label="BSC-12 pre-reset setting"
    )
    before_bond = require_sha256(before.get("bond_sha256"), code=code, label="BSC-12 pre-reset bond")
    after_setting = require_sha256(
        after.get("setting_sha256"), code=code, label="BSC-12 post-reset setting"
    )
    after_bond = require_sha256(after.get("bond_sha256"), code=code, label="BSC-12 post-reset bond")
    last_writer = max(int(writer["completion_elapsed_ms"]) for writer in writers)
    if (
        before.get("schema_version") != 1
        or after.get("schema_version") != 1
        or type(before.get("schema_version")) is not int
        or type(after.get("schema_version")) is not int
        or before.get("source") != "pre-reset-persistence-readback"
        or after.get("source") != "post-reset-persistence-readback"
        or type(before_elapsed) is not int
        or not last_writer <= before_elapsed < int(reset["forced_elapsed_ms"])
        or type(after_elapsed) is not int
        or not int(reset["boot_observed_elapsed_ms"]) < after_elapsed <= duration_ms
        or before.get("session_marker") != "unclean"
        or after.get("session_marker") != "unclean"
        or persistence.get("before_readback_elapsed_ms") != before_elapsed
        or persistence.get("after_readback_elapsed_ms") != after_elapsed
        or persistence.get("setting_mutation_sha256") != before_setting
        or persistence.get("setting_after_reset_sha256") != after_setting
        or persistence.get("bond_mutation_sha256") != before_bond
        or persistence.get("bond_after_reset_sha256") != after_bond
        or persistence.get("session_marker_after_reset") != after.get("session_marker")
        or persistence.get("persistence_before_sha256")
        != commitments.get("persistence_before_sha256")
        or persistence.get("persistence_after_sha256")
        != commitments.get("persistence_after_sha256")
    ):
        raise RunnerError(code, "BSC-12 persistence claims are not derived from readback captures")

    power = require_exact_object(
        raw.get("power-reset-trace"),
        {
            "schema_version",
            "source",
            "vbus_isolated",
            "vbus_verified_elapsed_ms",
            "forced_reset_edges_elapsed_ms",
        },
        code=code,
        label="BSC-12 power/reset trace",
    )
    forced_edges = power.get("forced_reset_edges_elapsed_ms")
    if (
        power.get("schema_version") != 1
        or type(power.get("schema_version")) is not int
        or power.get("source") != "rig-power-reset-trace"
        or power.get("vbus_isolated") is not True
        or type(power.get("vbus_verified_elapsed_ms")) is not int
        or not 0 <= power.get("vbus_verified_elapsed_ms") <= int(shutdown["shutdown_begin_elapsed_ms"])
        or not isinstance(forced_edges, list)
        or len(forced_edges) != 1
        or type(forced_edges[0]) is not int
        or forced_edges[0] != reset.get("forced_elapsed_ms")
        or safety.get("source") != power.get("source")
        or safety.get("vbus_isolated") is not power.get("vbus_isolated")
        or safety.get("vbus_verified_elapsed_ms") != power.get("vbus_verified_elapsed_ms")
        or safety.get("destructive_reset_triggered") is not True
        or safety.get("power_reset_trace_sha256")
        != commitments.get("power_reset_trace_sha256")
        or reset.get("power_reset_trace_sha256")
        != commitments.get("power_reset_trace_sha256")
    ):
        raise RunnerError(code, "BSC-12 VBUS/reset claims are not derived from the rig trace")

    wake = require_exact_object(
        raw.get("wake-input-trace"),
        {
            "schema_version",
            "source",
            "trigger",
            "asserted_elapsed_ms",
            "deasserted_elapsed_ms",
            "observed_during_handoff",
        },
        code=code,
        label="BSC-12 wake-input trace",
    )
    wake_asserted = wake.get("asserted_elapsed_ms")
    wake_deasserted = wake.get("deasserted_elapsed_ms")
    if (
        wake.get("schema_version") != 1
        or type(wake.get("schema_version")) is not int
        or wake.get("source") != "rtc-gpio-wake-input"
        or wake.get("trigger") != "active-low"
        or type(wake_asserted) is not int
        or type(wake_deasserted) is not int
        or not int(shutdown["handoff_begin_elapsed_ms"])
        <= wake_asserted
        < wake_deasserted
        <= int(shutdown["power_off_return_elapsed_ms"])
        or wake.get("observed_during_handoff") is not True
        or shutdown.get("wake_input_source") != wake.get("source")
        or shutdown.get("wake_trigger") != wake.get("trigger")
        or shutdown.get("wake_asserted_elapsed_ms") != wake_asserted
        or shutdown.get("real_rtc_wake_input") is not wake.get("observed_during_handoff")
        or shutdown.get("wake_input_trace_sha256")
        != commitments.get("wake_input_trace_sha256")
    ):
        raise RunnerError(code, "BSC-12 wake claim is not derived from the wake trace")

    timeline = require_exact_object(
        raw.get("shutdown-timeline"),
        {
            "schema_version",
            "source",
            "shutdown_begin_elapsed_ms",
            "handoff_begin_elapsed_ms",
            "power_off_return_elapsed_ms",
            "screen_restored_elapsed_ms",
            "marker_rewritten_elapsed_ms",
            "power_off_result",
            "screen_state",
            "marker_state",
        },
        code=code,
        label="BSC-12 shutdown timeline",
    )
    timeline_projection = {
        field: shutdown[field]
        for field in (
            "shutdown_begin_elapsed_ms",
            "handoff_begin_elapsed_ms",
            "power_off_return_elapsed_ms",
            "screen_restored_elapsed_ms",
            "marker_rewritten_elapsed_ms",
            "power_off_result",
            "screen_state",
            "marker_state",
        )
    }
    if (
        timeline.get("schema_version") != 1
        or type(timeline.get("schema_version")) is not int
        or timeline.get("source") != "dut-serial-timeline"
        or {key: timeline[key] for key in timeline_projection} != timeline_projection
        or shutdown.get("shutdown_timeline_sha256")
        != commitments.get("shutdown_timeline_sha256")
        or shutdown.get("serial_log_sha256") != commitments.get("serial_log_sha256")
    ):
        raise RunnerError(code, "BSC-12 shutdown claims are not derived from the timeline capture")

    serial_events = parse_bsc12_serial_events(raw.get("serial-log"), duration_ms=duration_ms)
    expected_serial = {
        "shutdown-begin": shutdown["shutdown_begin_elapsed_ms"],
        "handoff-begin": shutdown["handoff_begin_elapsed_ms"],
        "wake-asserted": shutdown["wake_asserted_elapsed_ms"],
        "power-off-returned-false": shutdown["power_off_return_elapsed_ms"],
        "screen-restored-disconnected": shutdown["screen_restored_elapsed_ms"],
        "marker-rewritten-unclean": shutdown["marker_rewritten_elapsed_ms"],
        "settings-writer-complete": writers[0]["completion_elapsed_ms"],
        "bond-writer-complete": writers[1]["completion_elapsed_ms"],
        "reset-forced": reset["forced_elapsed_ms"],
        "boot-observed": reset["boot_observed_elapsed_ms"],
        "setting-readback": after_elapsed,
        "bond-readback": after_elapsed,
    }
    if (
        any(serial_events.get(event) != elapsed for event, elapsed in expected_serial.items())
        or reset.get("serial_log_sha256") != commitments.get("serial_log_sha256")
        or reset.get("panic_observed") is not ("panic" in serial_events)
        or reset.get("watchdog_reset_observed") is not ("watchdog-reset" in serial_events)
        or reset.get("load_prohibited_observed") is not ("load-prohibited" in serial_events)
    ):
        raise RunnerError(code, "BSC-12 reset/crash claims are not derived from the serial capture")

    return {
        "poweroff-returned-false": timeline["power_off_result"] is False,
        "disconnected-screen-restored": timeline["screen_state"] == "disconnected",
        "session-marker-unclean-after-reset": after["session_marker"] == "unclean",
        "settings-writer-count": int("settings-writer-complete" in serial_events),
        "bond-writer-count": int("bond-writer-complete" in serial_events),
        "setting-survived-reset": secrets.compare_digest(before_setting, after_setting),
        "bond-survived-reset": secrets.compare_digest(before_bond, after_bond),
        "real-rtc-wake-input-observed": wake["observed_during_handoff"] is True,
    }


def validate_bsc12_facts(value: object, contracts: object) -> dict[str, object]:
    code = "case_record_invalid"
    if not isinstance(contracts, list) or [item.get("id") for item in contracts] != list(
        BSC12_FACT_IDS
    ):
        raise RunnerError(code, "BSC-12 fact descriptor is invalid")
    facts = require_exact_object(value, set(BSC12_FACT_IDS), code=code, label="BSC-12 facts")
    for contract in contracts:
        fact_id = contract["id"]
        observed = facts.get(fact_id)
        if contract.get("type") == "boolean":
            if type(observed) is not bool or observed is not contract.get("expected"):
                raise RunnerError(code, f"BSC-12 fact {fact_id} is invalid")
        elif contract.get("type") == "integer":
            if (
                type(observed) is not int
                or observed != contract.get("minimum")
                or observed != contract.get("maximum")
            ):
                raise RunnerError(code, f"BSC-12 fact {fact_id} is invalid")
        else:
            raise RunnerError(code, "BSC-12 fact descriptor type is invalid")
    return facts


def validate_bsc12_adapter_record(
    payload: object,
    *,
    expected: Mapping[str, object],
    raw_manifest: object,
    raw_content: Mapping[str, bytes],
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
            "descriptor",
            "firmware",
            "preconditions",
            "safety_observation",
            "stimuli",
            "barriers",
            "shutdown_observation",
            "writers",
            "persistence",
            "reset",
            "facts",
            "capture_commitments",
            "raw_artifact_manifest",
            "raw_artifact_request_sha256",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-12 adapter record",
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
            raise RunnerError(code, f"BSC-12 {field} does not match the runner invocation")
    if (
        type(record.get("schema_version")) is not int
        or record.get("schema_version") != 1
        or type(record.get("run_index")) is not int
        or record.get("run_index") != 1
        or type(record.get("hardware_observed")) is not bool
    ):
        raise RunnerError(code, "BSC-12 record identity types are invalid")
    request_sha = require_sha256(
        record.get("raw_artifact_request_sha256"),
        code=code,
        label="BSC-12 raw artifact request",
    )
    if request_sha != expected.get("raw_artifact_request_sha256"):
        raise RunnerError(code, "BSC-12 raw artifact request was substituted")

    case_descriptor = expected.get("case_descriptor")
    role_descriptor = expected.get("role_descriptor")
    if not isinstance(case_descriptor, dict) or not isinstance(role_descriptor, dict):
        raise RunnerError(code, "BSC-12 pinned descriptor binding is invalid")
    descriptor_sha = bsc12_descriptor_commitment(case_descriptor)
    if (
        record.get("case_descriptor") != case_descriptor
        or record.get("descriptor") != role_descriptor
        or expected.get("case_descriptor_sha256") != descriptor_sha
        or record.get("case_descriptor_sha256") != descriptor_sha
    ):
        raise RunnerError(code, "BSC-12 record does not match the pinned profile-v5 descriptor")

    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-12 start")
    completed = parse_runner_utc(record.get("completed_at_utc"), code=code, label="BSC-12 completion")
    if completed < started:
        raise RunnerError(code, "BSC-12 completion predates its start")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "BSC-12 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=999999):
        raise RunnerError(code, "BSC-12 physical record postdates adapter execution")
    duration_ms = int((completed - started).total_seconds() * 1_000)
    raw = validate_bsc12_raw_artifacts(
        raw_manifest,
        record.get("raw_artifact_manifest"),
        record.get("capture_commitments"),
        raw_content,
        expected_request_sha256=request_sha,
    )
    commitments = require_exact_object(
        record.get("capture_commitments"),
        set(BSC12_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-12 capture commitments",
    )

    firmware = require_exact_object(
        record.get("firmware"),
        {
            "environment",
            "build_kind",
            "target_sha",
            "binary_sha256",
            "hil_fault_control_active",
        },
        code=code,
        label="BSC-12 firmware",
    )
    if (
        firmware.get("environment") != BSC12_PRODUCTION_ENVIRONMENT
        or firmware.get("build_kind") != "production"
        or firmware.get("target_sha") != expected.get("target_sha")
        or type(firmware.get("hil_fault_control_active")) is not bool
        or firmware.get("hil_fault_control_active") is not False
    ):
        raise RunnerError(code, "BSC-12 firmware role or target is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-12 firmware binary")

    preconditions = require_exact_object(
        record.get("preconditions"),
        {"vbus_isolated", "destructive_reset_acknowledged"},
        code=code,
        label="BSC-12 preconditions",
    )
    if (
        preconditions.get("vbus_isolated") is not True
        or preconditions.get("destructive_reset_acknowledged") is not True
    ):
        raise RunnerError(code, "BSC-12 operator preconditions are incomplete")
    safety = validate_bsc12_safety_observation(record.get("safety_observation"))

    stimuli = validate_bsc12_stimuli(record.get("stimuli"), duration_ms=duration_ms)
    barriers = validate_bsc12_barriers(record.get("barriers"), duration_ms=duration_ms)
    shutdown = validate_bsc12_shutdown_observation(
        record.get("shutdown_observation"), stimuli=stimuli, duration_ms=duration_ms
    )
    writers = validate_bsc12_writers(
        record.get("writers"), stimuli=stimuli, duration_ms=duration_ms
    )
    persistence = validate_bsc12_persistence(record.get("persistence"))
    reset = validate_bsc12_reset(
        record.get("reset"),
        reset_contract=role_descriptor["reset_contract"],
        stimuli=stimuli,
        duration_ms=duration_ms,
    )
    facts = validate_bsc12_facts(record.get("facts"), role_descriptor["facts"])
    derived_facts = validate_bsc12_source_evidence(
        raw,
        commitments=commitments,
        firmware=firmware,
        safety=safety,
        shutdown=shutdown,
        writers=writers,
        persistence=persistence,
        reset=reset,
        duration_ms=duration_ms,
    )

    wake_barrier, writers_barrier = barriers
    if (
        wake_barrier["ready_elapsed_ms"] != shutdown["handoff_begin_elapsed_ms"]
        or wake_barrier["released_elapsed_ms"] != shutdown["power_off_return_elapsed_ms"]
        or not wake_barrier["ready_elapsed_ms"]
        <= shutdown["wake_asserted_elapsed_ms"]
        < wake_barrier["released_elapsed_ms"]
        or writers_barrier["ready_elapsed_ms"] != stimuli["wait-for-writers"]
        or writers_barrier["released_elapsed_ms"]
        != max(int(writer["completion_elapsed_ms"]) for writer in writers)
        or facts != derived_facts
        or reset["boot_observed_elapsed_ms"] > duration_ms
    ):
        raise RunnerError(code, "BSC-12 facts are not traceable to typed observations")

    binding = require_sha256(
        record.get("evidence_binding_sha256"), code=code, label="BSC-12 binding"
    )
    if not secrets.compare_digest(binding, bsc12_record_commitment(record)):
        raise RunnerError(code, "BSC-12 evidence binding is stale")
    return record


def run_bsc12_adapter(
    *,
    adapter: Path,
    repository: Path,
    serial_port: str,
    artifact_root: Path,
    raw_manifest_path: Path,
    role_contract: rig_adapters.AdapterRoleContract,
    expected: Mapping[str, object],
    environment: Mapping[str, str],
) -> tuple[dict[str, object], dict[str, object]]:
    command = [
        str(adapter),
        "--case",
        BSC12_CASE_ID,
        "--role-id",
        str(expected["role_id"]),
        "--case-descriptor-sha256",
        str(expected["case_descriptor_sha256"]),
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
        "--raw-artifact-dir",
        str(artifact_root),
        "--raw-artifact-request-sha256",
        str(expected["raw_artifact_request_sha256"]),
        "--vbus-isolated",
        "true",
        "--destructive-reset-acknowledged",
        "true",
    ]
    command_started = datetime.now(timezone.utc)
    try:
        completed = subprocess.run(
            command,
            cwd=repository,
            env=dict(environment),
            capture_output=True,
            check=False,
            timeout=BSC12_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-12 adapter exceeded its bounded timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-12 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-12 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > 128 * 1024:
        raise RunnerError("case_record_invalid", "BSC-12 adapter output size is invalid")
    try:
        payload = json.loads(
            completed.stdout.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError("case_record_invalid", "BSC-12 adapter output is not strict JSON") from exc
    try:
        raw_manifest = adapter_protocol.collect_raw_artifacts(
            raw_directory=artifact_root,
            role=role_contract,
            request_commitment_sha256=str(expected["raw_artifact_request_sha256"]),
            manifest_path=raw_manifest_path,
        )
        raw_content = adapter_protocol.read_collected_raw_artifacts(
            raw_directory=artifact_root,
            role=role_contract,
            manifest=raw_manifest,
        )
    except adapter_protocol.AdapterProtocolError as exc:
        raise RunnerError(exc.code, exc.message) from exc
    physical = expected.get("execution_mode") == "physical"
    return (
        validate_bsc12_adapter_record(
            payload,
            expected=expected,
            raw_manifest=raw_manifest,
            raw_content=raw_content,
            command_started=command_started if physical else None,
            command_completed=command_completed if physical else None,
        ),
        raw_manifest,
    )


def run_bsc12_case(args: argparse.Namespace) -> int:
    case_descriptor = load_bsc12_case_descriptor()
    if args.runs != BSC12_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-12 requires exactly one run")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-12 requires an opaque local rig alias")
    if args.resume:
        raise RunnerError("unsupported_mode", "BSC-12 collection is atomic")
    if args.production_replay:
        raise RunnerError("unsupported_mode", "BSC-12 has no production-replay role")
    role_descriptor = bsc12_role_descriptor(case_descriptor)
    admission = admit_case_rig_adapter(
        args,
        case_contract=case_descriptor,
        role_id=str(role_descriptor["role_id"]),
    )
    if not args.ack_vbus_isolated:
        raise RunnerError(
            "operator_preconditions_incomplete",
            "BSC-12 requires explicit VBUS-isolation acknowledgement",
        )
    if not args.ack_destructive_hard_cuts:
        raise RunnerError(
            "safety_ack_required",
            "BSC-12 requires destructive-reset acknowledgement",
        )
    repository = args.repo_root.resolve()
    if admission.simulated:
        if args.case_adapter is None:
            raise RunnerError("case_adapter_required", "BSC-12 simulation requires a test adapter")
        git_state = read_git_state(repository)
        if not git_state.tracked_clean:
            raise RunnerError("dirty_target", "BSC-12 requires a clean target worktree")
        adapter = args.case_adapter.resolve()
        if not adapter.is_file() or adapter.is_symlink():
            raise RunnerError("case_adapter_unavailable", "BSC-12 adapter must be a regular file")
        adapter_sha = sha256_file(adapter)
    else:
        if (
            admission.git_state is None
            or admission.source_sha256 is None
            or admission.adapter.source_path is None
        ):
            raise RunnerError("case_rig_adapter_unavailable", "BSC-12 tracked adapter is incomplete")
        git_state = admission.git_state
        adapter = repository / admission.adapter.source_path
        adapter_sha = admission.source_sha256
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc12_hardware(
        args, Path(args.pio_command), case_descriptor
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-12 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-12 serial endpoint is not present")

    role_id = str(role_descriptor["role_id"])
    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc12-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / f"{run_id}-{role_id}"
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-12 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc12-{secrets.token_hex(16)}"
    attempt_id = f"attempt-{secrets.token_hex(16)}"
    descriptor_sha = bsc12_descriptor_commitment(case_descriptor)
    raw_request_payload: dict[str, object] = {
        "case_id": BSC12_CASE_ID,
        "role_id": role_id,
        "session_id": session_id,
        "attempt_id": attempt_id,
        "target_sha": git_state.head_sha,
        "case_descriptor_sha256": descriptor_sha,
        "raw_artifacts": [
            {
                "role": artifact.role,
                "filename": artifact.filename,
                "maximum_bytes": artifact.maximum_bytes,
            }
            for artifact in next(
                role for role in admission.adapter.roles if role.role_id == role_id
            ).raw_artifacts
        ],
    }
    raw_request_sha = canonical_case_commitment(
        "v1simple.bsc12.raw-artifact-request.v1", raw_request_payload
    )
    expected: dict[str, object] = {
        "case_id": BSC12_CASE_ID,
        "role_id": role_id,
        "session_id": session_id,
        "attempt_id": attempt_id,
        "run_index": 1,
        "target_sha": git_state.head_sha,
        "dut_alias": args.board,
        "rig_alias": args.rig,
        "execution_mode": "simulated" if admission.simulated else "physical",
        "hardware_observed": not admission.simulated,
        "case_descriptor": case_descriptor,
        "case_descriptor_sha256": descriptor_sha,
        "role_descriptor": role_descriptor,
        "raw_artifact_request_sha256": raw_request_sha,
    }
    require_unchanged_git_state(repository, git_state)
    raw_artifact_root = run_root / "raw"
    raw_artifact_root.mkdir()
    raw_manifest_path = run_root / "raw-artifact-manifest.json"
    adapter_role = next(role for role in admission.adapter.roles if role.role_id == role_id)
    record, raw_manifest = run_bsc12_adapter(
        adapter=adapter,
        repository=repository,
        serial_port=serial_port,
        artifact_root=raw_artifact_root,
        raw_manifest_path=raw_manifest_path,
        role_contract=adapter_role,
        expected=expected,
        environment=os.environ.copy(),
    )
    require_unchanged_git_state(repository, git_state)
    attempt_path = run_root / "attempt.json"
    write_json_atomic(attempt_path, record)
    firmware = record["firmware"]
    assert isinstance(firmware, dict)
    physical = not admission.simulated
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc12-aborted-shutdown-recovery",
        "case_id": BSC12_CASE_ID,
        "collection_role": role_id,
        "case_descriptor_sha256": descriptor_sha,
        "target_sha": git_state.head_sha,
        "session_sha256": hashlib.sha256(session_id.encode("ascii")).hexdigest(),
        "attempt_sha256": hashlib.sha256(attempt_id.encode("ascii")).hexdigest(),
        "execution_mode": "physical" if physical else "simulated",
        "hardware_observed": physical,
        "authoritative": physical,
        "physical_collection_completed": physical,
        "non_qualifying": not physical,
        "qualification_status": "PASS" if physical else "BLOCKED",
        "qualification_blockers": []
        if physical
        else list(case_drivers.get_case_driver(BSC12_CASE_ID).qualification_blockers),
        "artifact_role": "case-qualification"
        if physical
        else "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC12_REQUIRED_RUNS,
        "runs_completed": 1,
        "production_replay_required": False,
        "vbus_isolation_acknowledged": True,
        "destructive_reset_acknowledged": True,
        "firmware_target": {
            "environment": firmware["environment"],
            "build_kind": firmware["build_kind"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
        },
        "raw_artifact_contract": [
            {
                "role": artifact.role,
                "filename": artifact.filename,
                "maximum_bytes": artifact.maximum_bytes,
            }
            for artifact in adapter_role.raw_artifacts
        ],
        "raw_artifact_sha256": {
            role: record["capture_commitments"][commitment]
            for role, _, commitment in BSC12_CAPTURE_ROLE_FIELDS
        },
        "evidence_binding_sha256": record["evidence_binding_sha256"],
        "raw_artifact_request_sha256": raw_request_sha,
        "raw_artifact_manifest_commitment_sha256": raw_manifest[
            "manifest_commitment_sha256"
        ],
        "artifact_sha256": {
            "adapter_record": sha256_file(attempt_path),
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "rig_adapter_registry": sha256_file(ROOT / rig_adapters.REGISTRY_SOURCE_PATH),
            "raw_artifact_manifest": sha256_file(raw_manifest_path),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc12.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc12.rig-attestation.v1", rig_attestation
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
    role_key = "production_replay" if args.production_replay else "scenario"
    role_contract = case_contract.get(role_key)
    if not isinstance(role_contract, dict) or not isinstance(role_contract.get("role_id"), str):
        raise RunnerError("adapter_contract_invalid", f"{case_id} role contract is unavailable")
    admit_case_rig_adapter(
        args,
        case_contract=case_contract,
        role_id=role_contract["role_id"],
    )
    raise RunnerError(
        "case_rig_adapter_unavailable",
        f"{case_id} physical execution remains blocked until its tracked rig adapter exists",
    )


def run_bsc07_case(args: argparse.Namespace) -> int:
    case_descriptor = load_bsc07_case_descriptor()
    role_descriptor = case_descriptor["scenario"]
    assert isinstance(role_descriptor, dict)
    if args.runs != BSC07_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-07 requires exactly one run")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-07 requires an opaque local rig alias")
    if args.resume or args.production_replay:
        raise RunnerError("unsupported_mode", "BSC-07 has one atomic production collection role")
    if not args.ack_vbus_isolated:
        raise RunnerError(
            "operator_preconditions_incomplete",
            "BSC-07 requires explicit VBUS-isolation acknowledgement",
        )
    admission = admit_case_rig_adapter(
        args,
        case_contract=case_descriptor,
        role_id=str(role_descriptor["role_id"]),
    )
    if args.case_adapter is None:
        raise RunnerError("case_adapter_required", "BSC-07 test execution requires a mocked adapter")

    repository = args.repo_root.resolve()
    git_state = read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-07 requires a clean target worktree")
    adapter = args.case_adapter.resolve()
    if not adapter.is_file() or adapter.is_symlink():
        raise RunnerError("case_adapter_unavailable", "BSC-07 adapter must be a regular file")
    adapter_sha = sha256_file(adapter)
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc07_hardware(
        args, Path(args.pio_command), case_descriptor
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-07 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-07 serial endpoint is not present")

    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc07-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / run_id
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-07 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)
    raw_directory = run_root / "raw"
    raw_directory.mkdir()
    raw_manifest_path = run_root / "raw-artifact-manifest.json"

    session_id = f"bsc07-{secrets.token_hex(16)}"
    attempt_id = f"attempt-{secrets.token_hex(16)}"
    descriptor_sha = bsc07_descriptor_commitment(case_descriptor)
    role_contract = next(
        role for role in admission.adapter.roles if role.role_id == role_descriptor["role_id"]
    )
    raw_request_payload = {
        "case_id": BSC07_CASE_ID,
        "role_id": role_descriptor["role_id"],
        "session_id": session_id,
        "attempt_id": attempt_id,
        "target_sha": git_state.head_sha,
        "adapter_sha256": adapter_sha,
        "case_descriptor_sha256": descriptor_sha,
        "raw_artifacts": [
            {
                "role": item.role,
                "filename": item.filename,
                "maximum_bytes": item.maximum_bytes,
            }
            for item in role_contract.raw_artifacts
        ],
    }
    raw_request_sha = canonical_case_commitment(
        "v1simple.bsc07.raw-artifact-request.v1", raw_request_payload
    )
    expected: dict[str, object] = {
        "case_id": BSC07_CASE_ID,
        "role_id": role_descriptor["role_id"],
        "session_id": session_id,
        "attempt_id": attempt_id,
        "target_sha": git_state.head_sha,
        "dut_alias": args.board,
        "rig_alias": args.rig,
        "execution_mode": "simulated",
        "hardware_observed": False,
        "case_descriptor": case_descriptor,
        "case_descriptor_sha256": descriptor_sha,
        "raw_artifact_request_sha256": raw_request_sha,
    }
    require_unchanged_git_state(repository, git_state)
    record, raw_manifest = run_bsc07_adapter(
        adapter=adapter,
        repository=repository,
        serial_port=serial_port,
        expected=expected,
        environment=os.environ.copy(),
        raw_directory=raw_directory,
        raw_manifest_path=raw_manifest_path,
        role_contract=role_contract,
    )
    require_unchanged_git_state(repository, git_state)
    attempt_path = run_root / "attempt.json"
    write_json_atomic(attempt_path, record)
    firmware = record["firmware"]
    assert isinstance(firmware, dict)
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc07-maintenance-power-safety",
        "case_id": BSC07_CASE_ID,
        "collection_role": role_descriptor["role_id"],
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
            case_drivers.get_case_driver(BSC07_CASE_ID).qualification_blockers
        ),
        "artifact_role": "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC07_REQUIRED_RUNS,
        "runs_completed": 1,
        "production_replay_required": False,
        "firmware_target": {
            "environment": firmware["environment"],
            "build_kind": firmware["build_kind"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
        },
        "evidence_binding_sha256": record["evidence_binding_sha256"],
        "raw_artifact_manifest_commitment_sha256": raw_manifest[
            "manifest_commitment_sha256"
        ],
        "artifact_sha256": {
            "adapter_record": sha256_file(attempt_path),
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc07.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc07.rig-attestation.v1", rig_attestation
            ),
            "raw_artifact_manifest": sha256_file(raw_manifest_path),
        },
    }
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def run_bsc08_case(args: argparse.Namespace) -> int:
    descriptor = bsc08_profile_descriptor()
    if args.runs != BSC08_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-08 requires exactly three physical runs")
    if args.production_replay:
        raise RunnerError("unsupported_mode", "BSC-08 has no production-replay role")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-08 requires an opaque local rig alias")
    admission = admit_case_rig_adapter(
        args,
        case_contract={
            "id": BSC08_CASE_ID,
            "minimum_runs": BSC08_REQUIRED_RUNS,
            "required_dut_capabilities": list(BSC08_DUT_CAPABILITIES),
            "required_rig_capabilities": list(BSC08_RIG_CAPABILITIES),
            "scenario": descriptor,
            "production_replay": None,
        },
        role_id="proxy-epoch-teardown",
    )
    pio_executable = validate_runtime_arguments(args)
    repository = args.repo_root.resolve()
    if admission.simulated:
        if args.case_adapter is None:
            raise RunnerError("case_adapter_required", "BSC-08 simulation requires a test adapter")
        git_state = read_git_state(repository)
        if not git_state.tracked_clean:
            raise RunnerError("dirty_target", "BSC-08 requires a clean target worktree")
        adapter_path = args.case_adapter.resolve()
        if not adapter_path.is_file() or adapter_path.is_symlink():
            raise RunnerError("case_adapter_unavailable", "BSC-08 adapter must be a regular file")
        adapter_sha = sha256_file(adapter_path)
        execution_adapter = _bsc08_test_adapter_descriptor(admission.adapter)
    else:
        if (
            admission.git_state is None
            or admission.source_sha256 is None
            or admission.adapter.source_path is None
            or not admission.adapter.implemented
        ):
            raise RunnerError("case_rig_adapter_unavailable", "BSC-08 tracked adapter is incomplete")
        git_state = admission.git_state
        adapter_sha = admission.source_sha256
        execution_adapter = admission.adapter
        adapter_path = repository / admission.adapter.source_path
    environment = (
        os.environ.copy()
        if admission.simulated
        else authoritative_child_environment(pio_executable)
    )
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc08_hardware(
        args,
        pio_executable,
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-08 DUT has no serial endpoint")
    if not Path(endpoints["serial_port"]).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-08 serial endpoint is not present")
    require_unchanged_git_state(repository, git_state)
    _, firmware_sha = build_bsc08_production_firmware(
        repository=repository,
        pio_executable=pio_executable,
        environment=environment,
    )
    require_unchanged_git_state(repository, git_state)

    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc08-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / run_id
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-08 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    profile, profile_errors = qualification.load_pinned_profile()
    if profile is None or profile_errors:
        raise RunnerError("qualification_profile_invalid", "BSC-08 pinned profile is invalid")
    session_id = f"bsc08-{secrets.token_hex(16)}"
    records: list[dict[str, object]] = []
    manifest_paths: list[Path] = []
    for run_index in range(1, BSC08_REQUIRED_RUNS + 1):
        attempt_id = f"attempt-{secrets.token_hex(16)}"
        attempt_root = run_root / f"attempt-{run_index}"
        raw_directory = attempt_root / "raw"
        raw_directory.mkdir(parents=True)
        manifest_path = attempt_root / "raw-artifact-manifest.json"
        request = adapter_protocol.build_adapter_request(
            adapter=execution_adapter,
            target_sha=git_state.head_sha,
            profile_id=str(profile["profile_id"]),
            profile_version=int(profile["profile_version"]),
            profile_sha256=qualification.PINNED_PROFILE_SHA256,
            adapter_source_sha256=adapter_sha,
            role_id="proxy-epoch-teardown",
            run_index=run_index,
            session_id=session_id,
            attempt_id=attempt_id,
            nonce=secrets.token_hex(16),
            dut_alias=args.board,
            dut_capabilities=BSC08_DUT_CAPABILITIES,
            rig_alias=str(args.rig),
            rig_capabilities=BSC08_RIG_CAPABILITIES,
            firmware_binary_sha256=firmware_sha,
        )
        expected: dict[str, object] = {
            "case_id": BSC08_CASE_ID,
            "role_id": "proxy-epoch-teardown",
            "session_id": session_id,
            "attempt_id": attempt_id,
            "run_index": run_index,
            "target_sha": git_state.head_sha,
            "dut_alias": args.board,
            "rig_alias": args.rig,
            "execution_mode": "simulated" if admission.simulated else "physical",
            "hardware_observed": not admission.simulated,
            "request_commitment_sha256": request["request_commitment_sha256"],
        }
        write_json_atomic(attempt_root / "adapter-request.json", request)
        require_unchanged_git_state(repository, git_state)
        record, _ = run_bsc08_adapter(
            adapter_path=adapter_path,
            adapter=execution_adapter,
            repository=repository,
            request=request,
            expected=expected,
            raw_directory=raw_directory,
            raw_manifest_path=manifest_path,
            environment=environment,
        )
        require_unchanged_git_state(repository, git_state)
        write_json_atomic(attempt_root / "attempt.json", record)
        records.append(record)
        manifest_paths.append(manifest_path)
    validate_bsc08_distinct_runs(records)

    physical = not admission.simulated
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc08-proxy-epoch-teardown",
        "case_id": BSC08_CASE_ID,
        "collection_role": "proxy-epoch-teardown",
        "target_sha": git_state.head_sha,
        "session_sha256": hashlib.sha256(session_id.encode("ascii")).hexdigest(),
        "execution_mode": "physical" if physical else "simulated",
        "hardware_observed": physical,
        "authoritative": physical,
        "physical_collection_completed": physical,
        "non_qualifying": not physical,
        "qualification_status": "PASS" if physical else "BLOCKED",
        "qualification_blockers": []
        if physical
        else list(case_drivers.get_case_driver(BSC08_CASE_ID).qualification_blockers),
        "artifact_role": "case-qualification" if physical else "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC08_REQUIRED_RUNS,
        "runs_completed": len(records),
        "production_replay_required": False,
        "firmware_target": {
            "environment": BSC08_PRODUCTION_ENVIRONMENT,
            "target_sha": git_state.head_sha,
            "binary_sha256": firmware_sha,
            "hil_fault_control_active": False,
        },
        "run_evidence_binding_sha256": [record["evidence_binding_sha256"] for record in records],
        "artifact_sha256": {
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "rig_adapter_registry": sha256_file(ROOT / rig_adapters.REGISTRY_SOURCE_PATH),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc08.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc08.rig-attestation.v1", rig_attestation
            ),
            **{
                f"raw_artifact_manifest_{index}": sha256_file(path)
                for index, path in enumerate(manifest_paths, start=1)
            },
        },
    }
    write_json_atomic(run_root / "collection_result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def load_bsc09_case_descriptor() -> dict[str, object]:
    profile, errors = qualification.load_pinned_profile()
    if (
        profile is None
        or errors
        or profile.get("profile_id") != BSC09_PROFILE_ID
        or profile.get("profile_version") != BSC09_PROFILE_VERSION
    ):
        raise RunnerError(
            "qualification_profile_invalid",
            "BSC-09 requires the exact pinned profile-v5 contract",
        )
    descriptor = next(
        (case for case in profile["required_cases"] if case.get("id") == BSC09_CASE_ID),
        None,
    )
    expected_facts = [
        {"id": fact_id, "type": "boolean", "expected": fact_id != BSC09_FACT_IDS[-1]}
        for fact_id in BSC09_FACT_IDS
    ]
    if not isinstance(descriptor, dict):
        raise RunnerError("case_driver_contract_invalid", "BSC-09 descriptor is unavailable")
    role = descriptor.get("scenario")
    if (
        set(descriptor)
        != {
            "id",
            "minimum_runs",
            "fault_build_required",
            "production_replay_required",
            "required_dut_capabilities",
            "required_rig_capabilities",
            "scenario",
            "production_replay",
        }
        or descriptor.get("id") != BSC09_CASE_ID
        or type(descriptor.get("minimum_runs")) is not int
        or descriptor.get("minimum_runs") != BSC09_REQUIRED_RUNS
        or descriptor.get("fault_build_required") is not False
        or descriptor.get("production_replay_required") is not False
        or descriptor.get("production_replay") is not None
        or descriptor.get("required_dut_capabilities") != list(BSC09_DUT_CAPABILITIES)
        or descriptor.get("required_rig_capabilities") != list(BSC09_RIG_CAPABILITIES)
        or not isinstance(role, dict)
        or set(role)
        != {
            "role_id",
            "schema",
            "build_kind",
            "stimulus_ids",
            "fault_ids",
            "barrier_ids",
            "vbus_isolation_required",
            "reset_contract",
            "facts",
        }
        or role.get("role_id") != "wifi-dual-consumer-scan"
        or role.get("schema") != "case-observation-v1"
        or role.get("build_kind") != "production"
        or role.get("stimulus_ids") != list(BSC09_STIMULUS_IDS)
        or role.get("fault_ids") != []
        or role.get("barrier_ids") != list(BSC09_BARRIER_IDS)
        or role.get("vbus_isolation_required") is not False
        or role.get("reset_contract")
        != {"expected_kind": "none", "expected_count": 0, "unexpected_count": 0}
        or role.get("facts") != expected_facts
    ):
        raise RunnerError("case_driver_contract_invalid", "BSC-09 profile-v5 contract drifted")
    return descriptor


def bsc09_descriptor_commitment(case_descriptor: Mapping[str, object]) -> str:
    return canonical_case_commitment("v1simple.bsc09.case-descriptor.v1", case_descriptor)


def bsc09_record_commitment(record: Mapping[str, object]) -> str:
    committed = dict(record)
    committed.pop("evidence_binding_sha256", None)
    return canonical_case_commitment("v1simple.bsc09.case-record.v1", committed)


def bsc09_adapter_request_commitment(expected: Mapping[str, object]) -> str:
    committed = dict(expected)
    committed.pop("adapter_request_sha256", None)
    return canonical_case_commitment("v1simple.bsc09.adapter-request.v1", committed)


def bsc09_raw_manifest_commitment(manifest: Mapping[str, object]) -> str:
    committed = dict(manifest)
    committed.pop("manifest_commitment_sha256", None)
    return adapter_protocol.canonical_commitment(adapter_protocol.MANIFEST_DOMAIN, committed)


def resolve_bsc09_hardware(
    args: argparse.Namespace,
    pio_executable: Path,
    case_descriptor: Mapping[str, object],
) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    inventory_path = args.inventory.resolve()
    if not inventory_path.is_file() or inventory_path.is_symlink():
        raise RunnerError(
            "local_inventory_missing",
            "BSC-09 requires the ignored local hardware inventory",
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
        raise RunnerError("case_alias_reused", "BSC-09 requires distinct DUT and rig aliases")
    return dut_resolution, dut_attestation, rig_attestation


def validate_bsc09_stimuli(value: object, *, duration_ms: int) -> dict[str, int]:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != len(BSC09_STIMULUS_IDS):
        raise RunnerError(code, "BSC-09 stimulus sequence is incomplete")
    elapsed_by_id: dict[str, int] = {}
    previous = -1
    for sequence, (raw, stimulus_id) in enumerate(
        zip(value, BSC09_STIMULUS_IDS, strict=True), start=1
    ):
        row = require_exact_object(
            raw,
            {"id", "sequence", "elapsed_ms", "result"},
            code=code,
            label="BSC-09 stimulus",
        )
        elapsed = row.get("elapsed_ms")
        if (
            row.get("id") != stimulus_id
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(elapsed) is not int
            or not 0 <= elapsed <= duration_ms
            or elapsed <= previous
            or row.get("result") != "pass"
        ):
            raise RunnerError(code, "BSC-09 stimulus identity, order, or timing is invalid")
        elapsed_by_id[stimulus_id] = elapsed
        previous = elapsed
    return elapsed_by_id


def validate_bsc09_lifecycle(value: object, *, duration_ms: int) -> dict[str, object]:
    code = "case_record_invalid"
    trace = require_exact_object(
        value,
        {"source", "capture_sha256", "events"},
        code=code,
        label="BSC-09 scan lifecycle trace",
    )
    if trace.get("source") != "WiFiScanTrace":
        raise RunnerError(code, "BSC-09 scan lifecycle source is not the product trace")
    capture = require_sha256(
        trace.get("capture_sha256"), code=code, label="BSC-09 scan lifecycle capture"
    )
    events = trace.get("events")
    if not isinstance(events, list) or len(events) != len(BSC09_LIFECYCLE_EVENTS):
        raise RunnerError(code, "BSC-09 scan lifecycle event coverage is incomplete")
    generation: int | None = None
    network_count: int | None = None
    previous = -1
    elapsed: list[int] = []
    release_count = 0
    for sequence, (raw, expected) in enumerate(
        zip(events, BSC09_LIFECYCLE_EVENTS, strict=True), start=1
    ):
        row = require_exact_object(
            raw,
            {
                "event",
                "consumer",
                "generation",
                "network_count",
                "pending_consumer_mask",
                "snapshot_consumer_mask",
                "running",
                "released",
                "aborted",
                "sequence",
                "elapsed_ms",
            },
            code=code,
            label="BSC-09 scan lifecycle event",
        )
        event, consumer, pending_mask, snapshot_mask, running, released, aborted = expected
        current_generation = row.get("generation")
        current_network_count = row.get("network_count")
        current_elapsed = row.get("elapsed_ms")
        if (
            row.get("event") != event
            or row.get("consumer") != consumer
            or type(current_generation) is not int
            or current_generation <= 0
            or type(current_network_count) is not int
            or type(row.get("pending_consumer_mask")) is not int
            or row.get("pending_consumer_mask") != pending_mask
            or type(row.get("snapshot_consumer_mask")) is not int
            or row.get("snapshot_consumer_mask") != snapshot_mask
            or type(row.get("running")) is not bool
            or row.get("running") is not running
            or type(row.get("released")) is not bool
            or row.get("released") is not released
            or type(row.get("aborted")) is not bool
            or row.get("aborted") is not aborted
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(current_elapsed) is not int
            or not 0 <= current_elapsed <= duration_ms
            or current_elapsed <= previous
        ):
            raise RunnerError(code, "BSC-09 scan lifecycle identity or state is invalid")
        if generation is None:
            generation = current_generation
        elif current_generation != generation:
            raise RunnerError(code, "BSC-09 scan generation changed inside one physical scan")
        if event in {"request-started", "request-joined"}:
            if current_network_count != -1:
                raise RunnerError(code, "BSC-09 request event exposed an invalid result count")
        elif event == "harvest-completed":
            if not 2 <= current_network_count <= 64:
                raise RunnerError(code, "BSC-09 physical scan did not observe multiple access points")
            network_count = current_network_count
        elif current_network_count != network_count:
            raise RunnerError(code, "BSC-09 consumer snapshots do not match the harvested scan")
        if released:
            release_count += 1
        elapsed.append(current_elapsed)
        previous = current_elapsed
    if generation is None or network_count is None or release_count != 1:
        raise RunnerError(code, "BSC-09 scan release or snapshot cardinality is invalid")
    return {
        "capture_sha256": capture,
        "generation": generation,
        "network_count": network_count,
        "elapsed_ms": elapsed,
        "maintenance_snapshot_stable": True,
        "ui_snapshot_stable": True,
    }


def validate_bsc09_browser_trace(value: object, *, duration_ms: int) -> dict[str, object]:
    code = "case_record_invalid"
    trace = require_exact_object(
        value,
        {"capture_sha256", "steps"},
        code=code,
        label="BSC-09 browser trace",
    )
    capture = require_sha256(
        trace.get("capture_sha256"), code=code, label="BSC-09 browser capture"
    )
    steps = trace.get("steps")
    if not isinstance(steps, list) or len(steps) != len(BSC09_BROWSER_ACTIONS):
        raise RunnerError(code, "BSC-09 browser trace is incomplete")
    run_ids = ((1, 1), (1, 2), (3, 3), (3, 4), (3, 4), (5, 5), (5, 5), (5, 5))
    previous = -1
    elapsed: list[int] = []
    for sequence, (raw, expected, expected_runs) in enumerate(
        zip(steps, BSC09_BROWSER_ACTIONS, run_ids, strict=True), start=1
    ):
        row = require_exact_object(
            raw,
            {
                "action",
                "sequence",
                "elapsed_ms",
                "request_run_id",
                "current_run_id",
                "http_method",
                "path",
                "response_status",
                "response_scanning",
                "modal_open",
                "spinner_active",
                "error_visible",
                "response_accepted",
            },
            code=code,
            label="BSC-09 browser step",
        )
        (
            action,
            method,
            path,
            status,
            scanning,
            modal,
            spinner,
            error,
            accepted,
        ) = expected
        current_elapsed = row.get("elapsed_ms")
        if (
            row.get("action") != action
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(current_elapsed) is not int
            or not 0 <= current_elapsed <= duration_ms
            or current_elapsed <= previous
            or type(row.get("request_run_id")) is not int
            or type(row.get("current_run_id")) is not int
            or (row.get("request_run_id"), row.get("current_run_id")) != expected_runs
            or row.get("http_method") != method
            or row.get("path") != path
            or type(row.get("response_status")) is not int
            or row.get("response_status") != status
            or type(row.get("response_scanning")) is not bool
            or row.get("response_scanning") is not scanning
            or type(row.get("modal_open")) is not bool
            or row.get("modal_open") is not modal
            or type(row.get("spinner_active")) is not bool
            or row.get("spinner_active") is not spinner
            or type(row.get("error_visible")) is not bool
            or row.get("error_visible") is not error
            or type(row.get("response_accepted")) is not bool
            or row.get("response_accepted") is not accepted
        ):
            raise RunnerError(code, "BSC-09 browser retry state machine is invalid")
        elapsed.append(current_elapsed)
        previous = current_elapsed
    return {
        "capture_sha256": capture,
        "elapsed_ms": elapsed,
        "consumer_generations_isolated": True,
        "retry_succeeded": True,
        "spinner_terminated": True,
    }


def validate_bsc09_barriers(
    value: object,
    *,
    lifecycle: Mapping[str, object],
) -> None:
    code = "case_record_invalid"
    if not isinstance(value, list) or len(value) != 1:
        raise RunnerError(code, "BSC-09 overlap barrier is missing")
    row = require_exact_object(
        value[0],
        {
            "id",
            "sequence",
            "generation",
            "ready_elapsed_ms",
            "released_elapsed_ms",
            "source_event_sequences",
            "timed_out",
        },
        code=code,
        label="BSC-09 overlap barrier",
    )
    elapsed = lifecycle["elapsed_ms"]
    assert isinstance(elapsed, list)
    if (
        row.get("id") != BSC09_BARRIER_IDS[0]
        or type(row.get("sequence")) is not int
        or row.get("sequence") != 1
        or type(row.get("generation")) is not int
        or row.get("generation") != lifecycle["generation"]
        or type(row.get("ready_elapsed_ms")) is not int
        or row.get("ready_elapsed_ms") != elapsed[1]
        or type(row.get("released_elapsed_ms")) is not int
        or row.get("released_elapsed_ms") != elapsed[4]
        or row.get("source_event_sequences") != [1, 2, 5]
        or type(row.get("timed_out")) is not bool
        or row.get("timed_out") is not False
    ):
        raise RunnerError(code, "BSC-09 overlap barrier is not derived from the lifecycle trace")


def validate_bsc09_heap(
    value: object,
    *,
    lifecycle: Mapping[str, object],
    duration_ms: int,
) -> dict[str, object]:
    code = "case_record_invalid"
    trace = require_exact_object(
        value,
        {"capture_sha256", "integrity_ok", "samples"},
        code=code,
        label="BSC-09 heap trace",
    )
    capture = require_sha256(
        trace.get("capture_sha256"), code=code, label="BSC-09 heap capture"
    )
    samples = trace.get("samples")
    if not isinstance(samples, list) or len(samples) != 3:
        raise RunnerError(code, "BSC-09 heap phase coverage is incomplete")
    phases = ("before-scan", "overlapped-scan", "released")
    values: list[tuple[int, int, int]] = []
    previous = -1
    for sequence, (raw, phase) in enumerate(zip(samples, phases, strict=True), start=1):
        row = require_exact_object(
            raw,
            {"phase", "sequence", "elapsed_ms", "free_heap_bytes", "largest_block_bytes"},
            code=code,
            label="BSC-09 heap sample",
        )
        current_elapsed = row.get("elapsed_ms")
        free_heap = row.get("free_heap_bytes")
        largest = row.get("largest_block_bytes")
        if (
            row.get("phase") != phase
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(current_elapsed) is not int
            or not 0 <= current_elapsed <= duration_ms
            or current_elapsed <= previous
            or type(free_heap) is not int
            or type(largest) is not int
            or not 0 < largest <= free_heap
        ):
            raise RunnerError(code, "BSC-09 heap sample identity, ordering, or bounds are invalid")
        values.append((current_elapsed, free_heap, largest))
        previous = current_elapsed
    lifecycle_elapsed = lifecycle["elapsed_ms"]
    assert isinstance(lifecycle_elapsed, list)
    before, overlap, released = values
    if (
        type(trace.get("integrity_ok")) is not bool
        or trace.get("integrity_ok") is not True
        or not before[0] <= lifecycle_elapsed[0]
        or not lifecycle_elapsed[1] <= overlap[0] < lifecycle_elapsed[4]
        or released[0] < lifecycle_elapsed[-1]
        or overlap[1] >= before[1]
        or overlap[2] >= before[2]
        or released[1] < before[1]
        or released[2] < before[2]
    ):
        raise RunnerError(code, "BSC-09 heap release or integrity evidence is invalid")
    return {
        "capture_sha256": capture,
        "before_free_bytes": before[1],
        "released_free_bytes": released[1],
        "accumulating_heap_loss_observed": False,
    }


def validate_bsc09_wifi_mode(
    value: object,
    *,
    lifecycle: Mapping[str, object],
    duration_ms: int,
) -> dict[str, object]:
    code = "case_record_invalid"
    trace = require_exact_object(
        value,
        {"capture_sha256", "samples"},
        code=code,
        label="BSC-09 WiFi mode trace",
    )
    capture = require_sha256(
        trace.get("capture_sha256"), code=code, label="BSC-09 WiFi mode capture"
    )
    samples = trace.get("samples")
    if not isinstance(samples, list) or len(samples) != 3:
        raise RunnerError(code, "BSC-09 WiFi mode samples are incomplete")
    phases = ("maintenance-start", "scan-overlap", "settled")
    expected_settled = (False, False, True)
    elapsed: list[int] = []
    previous = -1
    for sequence, (raw, phase, settled) in enumerate(
        zip(samples, phases, expected_settled, strict=True), start=1
    ):
        row = require_exact_object(
            raw,
            {"phase", "sequence", "elapsed_ms", "mode", "settled"},
            code=code,
            label="BSC-09 WiFi mode sample",
        )
        current_elapsed = row.get("elapsed_ms")
        if (
            row.get("phase") != phase
            or type(row.get("sequence")) is not int
            or row.get("sequence") != sequence
            or type(current_elapsed) is not int
            or not 0 <= current_elapsed <= duration_ms
            or current_elapsed <= previous
            or row.get("mode") != "AP_STA"
            or type(row.get("settled")) is not bool
            or row.get("settled") is not settled
        ):
            raise RunnerError(code, "BSC-09 WiFi mode did not settle through the scan")
        elapsed.append(current_elapsed)
        previous = current_elapsed
    lifecycle_elapsed = lifecycle["elapsed_ms"]
    assert isinstance(lifecycle_elapsed, list)
    if elapsed[0] > lifecycle_elapsed[0] or not lifecycle_elapsed[1] <= elapsed[1] < lifecycle_elapsed[4]:
        raise RunnerError(code, "BSC-09 WiFi mode samples are not aligned to the scan")
    if elapsed[2] < lifecycle_elapsed[-1]:
        raise RunnerError(code, "BSC-09 WiFi mode settled before consumer completion")
    return {
        "capture_sha256": capture,
        "settled_elapsed_ms": elapsed[2],
        "wifi_mode_settled": True,
    }


def validate_bsc09_health(value: object) -> dict[str, object]:
    code = "case_record_invalid"
    health = require_exact_object(
        value,
        {
            "expected_kind",
            "planned_count",
            "observed_count",
            "unexpected_count",
            "panic_observed",
            "watchdog_reset_observed",
            "heap_corruption_observed",
            "capture_sha256",
        },
        code=code,
        label="BSC-09 reset and health observation",
    )
    capture = require_sha256(
        health.get("capture_sha256"), code=code, label="BSC-09 serial health capture"
    )
    if (
        health.get("expected_kind") != "none"
        or type(health.get("planned_count")) is not int
        or health.get("planned_count") != 0
        or type(health.get("observed_count")) is not int
        or health.get("observed_count") != 0
        or type(health.get("unexpected_count")) is not int
        or health.get("unexpected_count") != 0
        or type(health.get("panic_observed")) is not bool
        or health.get("panic_observed") is not False
        or type(health.get("watchdog_reset_observed")) is not bool
        or health.get("watchdog_reset_observed") is not False
        or type(health.get("heap_corruption_observed")) is not bool
        or health.get("heap_corruption_observed") is not False
    ):
        raise RunnerError(code, "BSC-09 reset, panic, watchdog, or heap health is invalid")
    return {"capture_sha256": capture}


def validate_bsc09_raw_manifest(
    value: object,
    *,
    request_sha256: str,
) -> dict[str, str]:
    code = "case_record_invalid"
    manifest = require_exact_object(
        value,
        {
            "schema_version",
            "protocol_version",
            "request_commitment_sha256",
            "artifacts",
            "manifest_commitment_sha256",
        },
        code=code,
        label="BSC-09 raw artifact manifest",
    )
    artifacts = manifest.get("artifacts")
    contract = rig_adapters.get_rig_adapter(BSC09_CASE_ID).roles[0].raw_artifacts
    if not isinstance(artifacts, list) or len(artifacts) != len(contract):
        raise RunnerError(code, "BSC-09 raw artifact manifest is incomplete")
    digests: dict[str, str] = {}
    for raw, expected in zip(artifacts, contract, strict=True):
        row = require_exact_object(
            raw,
            {"role", "filename", "size_bytes", "sha256"},
            code=code,
            label="BSC-09 raw artifact",
        )
        size_bytes = row.get("size_bytes")
        digest = require_sha256(
            row.get("sha256"), code=code, label=f"BSC-09 {expected.role} raw artifact"
        )
        if (
            row.get("role") != expected.role
            or row.get("filename") != expected.filename
            or type(size_bytes) is not int
            or not 1 <= size_bytes <= expected.maximum_bytes
        ):
            raise RunnerError(code, "BSC-09 raw artifact role, filename, or size is invalid")
        digests[expected.role] = digest
    manifest_commitment = require_sha256(
        manifest.get("manifest_commitment_sha256"),
        code=code,
        label="BSC-09 raw artifact manifest",
    )
    if (
        type(manifest.get("schema_version")) is not int
        or manifest.get("schema_version") != 1
        or type(manifest.get("protocol_version")) is not int
        or manifest.get("protocol_version") != rig_adapters.ADAPTER_PROTOCOL_VERSION
        or manifest.get("request_commitment_sha256") != request_sha256
        or len(set(digests.values())) != len(digests)
        or not secrets.compare_digest(manifest_commitment, bsc09_raw_manifest_commitment(manifest))
    ):
        raise RunnerError(code, "BSC-09 raw artifact manifest binding is invalid")
    return digests


def validate_bsc09_facts(
    value: object,
    *,
    lifecycle: Mapping[str, object],
    browser: Mapping[str, object],
    heap: Mapping[str, object],
    wifi_mode: Mapping[str, object],
) -> None:
    code = "case_record_invalid"
    facts = require_exact_object(
        value,
        set(BSC09_FACT_IDS),
        code=code,
        label="BSC-09 derived facts",
    )
    derived = {
        "maintenance-snapshot-stable": lifecycle.get("maintenance_snapshot_stable"),
        "ui-snapshot-stable": lifecycle.get("ui_snapshot_stable"),
        "consumer-generations-isolated": browser.get("consumer_generations_isolated"),
        "retry-succeeded": browser.get("retry_succeeded"),
        "spinner-terminated": browser.get("spinner_terminated"),
        "wifi-mode-settled": wifi_mode.get("wifi_mode_settled"),
        "accumulating-scan-heap-loss-observed": heap.get("accumulating_heap_loss_observed"),
    }
    if any(type(facts.get(fact_id)) is not bool for fact_id in BSC09_FACT_IDS):
        raise RunnerError(code, "BSC-09 facts must be typed booleans")
    if facts != derived:
        raise RunnerError(code, "BSC-09 facts do not match the typed observations")
    lifecycle_elapsed = lifecycle.get("elapsed_ms")
    browser_elapsed = browser.get("elapsed_ms")
    if (
        not isinstance(lifecycle_elapsed, list)
        or not isinstance(browser_elapsed, list)
        or browser_elapsed[0] != lifecycle_elapsed[1]
        or browser_elapsed[2] != lifecycle_elapsed[2]
        or browser_elapsed[5] != lifecycle_elapsed[3]
        or browser_elapsed[-1] < lifecycle_elapsed[-1]
        or heap.get("released_free_bytes", 0) < heap.get("before_free_bytes", 1)
        or type(wifi_mode.get("settled_elapsed_ms")) is not int
        or wifi_mode["settled_elapsed_ms"] < lifecycle_elapsed[-1]
    ):
        raise RunnerError(code, "BSC-09 aggregate facts are not traceable to the captured events")


_BSC09_PRIVATE_PROJECTION_KEYS = frozenset(
    {
        "address",
        "bssid",
        "credential",
        "credentials",
        "ip_address",
        "mac",
        "mac_address",
        "network_hash",
        "network_id",
        "network_sha256",
        "password",
        "ssid",
    }
)
_BSC09_MAC_ADDRESS_RE = re.compile(r"(?i)(?<![0-9a-f])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f])")
_BSC09_IPV4_ADDRESS_RE = re.compile(
    r"(?<![0-9])(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})(?:\.(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})){3}(?![0-9])"
)


def reject_bsc09_private_projection_content(value: object) -> None:
    """Reject network identifiers from the only raw formats BSC-09 will commit."""

    if isinstance(value, dict):
        for key, item in value.items():
            normalized = key.lower().replace("-", "_")
            if normalized in _BSC09_PRIVATE_PROJECTION_KEYS:
                raise RunnerError(
                    "case_projection_private",
                    "BSC-09 raw projection contains a forbidden private identifier field",
                )
            reject_bsc09_private_projection_content(item)
        return
    if isinstance(value, list):
        for item in value:
            reject_bsc09_private_projection_content(item)
        return
    if isinstance(value, str):
        lowered = value.lower()
        if (
            "ssid=" in lowered
            or "bssid=" in lowered
            or "password=" in lowered
            or "credential=" in lowered
            or _BSC09_MAC_ADDRESS_RE.search(value) is not None
            or _BSC09_IPV4_ADDRESS_RE.search(value) is not None
        ):
            raise RunnerError(
                "case_projection_private",
                "BSC-09 raw projection contains a forbidden private identifier value",
            )


def parse_bsc09_projection(
    raw: bytes,
    *,
    role: str,
    fields: set[str],
) -> dict[str, object]:
    try:
        payload = json.loads(raw.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys)
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RunnerError(
            "case_projection_invalid", f"BSC-09 {role} projection is not strict JSON"
        ) from exc
    reject_bsc09_private_projection_content(payload)
    projection = require_exact_object(
        payload,
        {"schema_version", *fields},
        code="case_projection_invalid",
        label=f"BSC-09 {role} projection",
    )
    if type(projection.get("schema_version")) is not int or projection["schema_version"] != 1:
        raise RunnerError("case_projection_invalid", f"BSC-09 {role} projection version is invalid")
    return projection


def build_bsc09_record_from_projections(
    raw_by_role: Mapping[str, bytes],
    *,
    raw_manifest: Mapping[str, object],
    expected: Mapping[str, object],
    command_started: datetime | None,
    command_completed: datetime | None,
) -> dict[str, object]:
    if set(raw_by_role) != set(BSC09_RAW_CAPTURE_FIELDS):
        raise RunnerError("case_projection_invalid", "BSC-09 raw projection roles are incomplete")
    request_sha = bsc09_adapter_request_commitment(expected)
    raw_digests = validate_bsc09_raw_manifest(
        raw_manifest,
        request_sha256=request_sha,
    )
    captures = {
        capture_field: raw_digests[raw_role]
        for raw_role, capture_field in BSC09_RAW_CAPTURE_FIELDS.items()
    }

    case_projection = parse_bsc09_projection(
        raw_by_role["case-observation"],
        role="case observation",
        fields={
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
        },
    )
    identity_fields = {
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
        "dut_capabilities",
        "rig_capabilities",
    }
    if any(case_projection.get(field) != expected.get(field) for field in identity_fields):
        raise RunnerError(
            "case_projection_invalid", "BSC-09 case projection does not bind the runner request"
        )

    firmware_projection = parse_bsc09_projection(
        raw_by_role["firmware-build"],
        role="firmware build",
        fields={
            "request_commitment_sha256",
            "environment",
            "build_kind",
            "target_sha",
            "binary_sha256",
            "hil_fault_control_active",
        },
    )
    lifecycle_projection = parse_bsc09_projection(
        raw_by_role["wifi-scan-projection"],
        role="WiFi scan",
        fields={"request_commitment_sha256", "source", "events"},
    )
    browser_projection = parse_bsc09_projection(
        raw_by_role["browser-projection"],
        role="browser",
        fields={"request_commitment_sha256", "steps"},
    )
    heap_projection = parse_bsc09_projection(
        raw_by_role["heap-projection"],
        role="heap",
        fields={"request_commitment_sha256", "integrity_ok", "samples"},
    )
    mode_projection = parse_bsc09_projection(
        raw_by_role["wifi-mode-projection"],
        role="WiFi mode",
        fields={"request_commitment_sha256", "samples"},
    )
    health_projection = parse_bsc09_projection(
        raw_by_role["health-projection"],
        role="health",
        fields={
            "request_commitment_sha256",
            "expected_kind",
            "planned_count",
            "observed_count",
            "unexpected_count",
            "panic_observed",
            "watchdog_reset_observed",
            "heap_corruption_observed",
        },
    )
    bound_projections = (
        firmware_projection,
        lifecycle_projection,
        browser_projection,
        heap_projection,
        mode_projection,
        health_projection,
    )
    if any(item.get("request_commitment_sha256") != request_sha for item in bound_projections):
        raise RunnerError(
            "case_projection_invalid", "BSC-09 projection does not bind the runner request"
        )

    def projection_payload(projection: Mapping[str, object]) -> dict[str, object]:
        return {
            key: value
            for key, value in projection.items()
            if key not in {"schema_version", "request_commitment_sha256"}
        }

    record = dict(case_projection)
    record["case_descriptor"] = expected["case_descriptor"]
    record["firmware"] = {
        **projection_payload(firmware_projection),
        "capture_sha256": captures["firmware_build_sha256"],
    }
    record["scan_lifecycle"] = {
        **projection_payload(lifecycle_projection),
        "capture_sha256": captures["wifi_scan_projection_sha256"],
    }
    record["browser_trace"] = {
        **projection_payload(browser_projection),
        "capture_sha256": captures["browser_projection_sha256"],
    }
    record["heap_trace"] = {
        **projection_payload(heap_projection),
        "capture_sha256": captures["heap_projection_sha256"],
    }
    record["wifi_mode_trace"] = {
        **projection_payload(mode_projection),
        "capture_sha256": captures["wifi_mode_projection_sha256"],
    }
    record["health"] = {
        **projection_payload(health_projection),
        "capture_sha256": captures["health_projection_sha256"],
    }
    record["capture_commitments"] = captures
    record["raw_artifact_manifest"] = dict(raw_manifest)
    record["evidence_binding_sha256"] = bsc09_record_commitment(record)
    return validate_bsc09_adapter_record(
        record,
        expected=expected,
        command_started=command_started,
        command_completed=command_completed,
    )


def validate_bsc09_adapter_record(
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
            "run_index",
            "session_id",
            "attempt_id",
            "target_sha",
            "dut_alias",
            "rig_alias",
            "execution_mode",
            "hardware_observed",
            "qualification_profile",
            "case_descriptor",
            "case_descriptor_sha256",
            "adapter_request_sha256",
            "started_at_utc",
            "completed_at_utc",
            "duration_ms",
            "dut_capabilities",
            "rig_capabilities",
            "firmware",
            "stimuli",
            "scan_lifecycle",
            "browser_trace",
            "barriers",
            "heap_trace",
            "wifi_mode_trace",
            "health",
            "facts",
            "capture_commitments",
            "raw_artifact_manifest",
            "evidence_binding_sha256",
        },
        code=code,
        label="BSC-09 adapter record",
    )
    for field in (
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
        "case_descriptor",
        "case_descriptor_sha256",
        "adapter_request_sha256",
        "dut_capabilities",
        "rig_capabilities",
    ):
        if record.get(field) != expected.get(field):
            raise RunnerError(code, f"BSC-09 {field} does not match the runner invocation")
    if (
        type(record.get("schema_version")) is not int
        or record.get("schema_version") != 1
        or type(record.get("run_index")) is not int
        or not 1 <= record["run_index"] <= BSC09_REQUIRED_RUNS
        or type(record.get("hardware_observed")) is not bool
        or record.get("execution_mode") not in {"simulated", "physical"}
        or not isinstance(record.get("dut_capabilities"), list)
        or not isinstance(record.get("rig_capabilities"), list)
    ):
        raise RunnerError(code, "BSC-09 record identity types are invalid")
    case_descriptor = expected.get("case_descriptor")
    if not isinstance(case_descriptor, dict):
        raise RunnerError(code, "BSC-09 expected descriptor is invalid")
    descriptor_sha = bsc09_descriptor_commitment(case_descriptor)
    request_sha = bsc09_adapter_request_commitment(expected)
    if (
        expected.get("case_descriptor_sha256") != descriptor_sha
        or record.get("case_descriptor_sha256") != descriptor_sha
        or expected.get("adapter_request_sha256") != request_sha
        or record.get("adapter_request_sha256") != request_sha
    ):
        raise RunnerError(code, "BSC-09 profile, descriptor, or request binding is invalid")
    started = parse_runner_utc(record.get("started_at_utc"), code=code, label="BSC-09 start")
    completed = parse_runner_utc(record.get("completed_at_utc"), code=code, label="BSC-09 completion")
    duration_ms = record.get("duration_ms")
    actual_duration_ms = int((completed - started).total_seconds() * 1_000)
    if (
        completed <= started
        or type(duration_ms) is not int
        or duration_ms != actual_duration_ms
        or not 1 <= duration_ms <= BSC09_ADAPTER_TIMEOUT_SECONDS * 1_000
    ):
        raise RunnerError(code, "BSC-09 UTC duration binding is invalid")
    if command_started is not None and started < command_started.replace(microsecond=0):
        raise RunnerError(code, "BSC-09 physical record predates adapter execution")
    if command_completed is not None and completed > command_completed.replace(microsecond=999999):
        raise RunnerError(code, "BSC-09 physical record postdates adapter execution")
    captures = require_exact_object(
        record.get("capture_commitments"),
        set(BSC09_CAPTURE_COMMITMENTS),
        code=code,
        label="BSC-09 capture commitments",
    )
    capture_digests = {
        field: require_sha256(captures.get(field), code=code, label=f"BSC-09 {field}")
        for field in BSC09_CAPTURE_COMMITMENTS
    }
    if len(set(capture_digests.values())) != len(capture_digests):
        raise RunnerError(code, "BSC-09 capture roles reused the same artifact")
    raw_digests = validate_bsc09_raw_manifest(
        record.get("raw_artifact_manifest"),
        request_sha256=request_sha,
    )
    if any(
        capture_digests[capture_field] != raw_digests[raw_role]
        for raw_role, capture_field in BSC09_RAW_CAPTURE_FIELDS.items()
    ):
        raise RunnerError(code, "BSC-09 capture commitments are not bound to the raw manifest")
    firmware = require_exact_object(
        record.get("firmware"),
        {
            "environment",
            "build_kind",
            "target_sha",
            "binary_sha256",
            "hil_fault_control_active",
            "capture_sha256",
        },
        code=code,
        label="BSC-09 firmware",
    )
    if (
        firmware.get("environment") != BSC09_PRODUCTION_ENVIRONMENT
        or firmware.get("build_kind") != "production"
        or firmware.get("target_sha") != expected.get("target_sha")
        or type(firmware.get("hil_fault_control_active")) is not bool
        or firmware.get("hil_fault_control_active") is not False
        or require_sha256(
            firmware.get("capture_sha256"), code=code, label="BSC-09 firmware build capture"
        )
        != capture_digests["firmware_build_sha256"]
    ):
        raise RunnerError(code, "BSC-09 firmware build, target, or capture is invalid")
    require_sha256(firmware.get("binary_sha256"), code=code, label="BSC-09 firmware binary")
    stimuli = validate_bsc09_stimuli(record.get("stimuli"), duration_ms=duration_ms)
    lifecycle = validate_bsc09_lifecycle(record.get("scan_lifecycle"), duration_ms=duration_ms)
    browser = validate_bsc09_browser_trace(record.get("browser_trace"), duration_ms=duration_ms)
    validate_bsc09_barriers(record.get("barriers"), lifecycle=lifecycle)
    heap = validate_bsc09_heap(
        record.get("heap_trace"), lifecycle=lifecycle, duration_ms=duration_ms
    )
    wifi_mode = validate_bsc09_wifi_mode(
        record.get("wifi_mode_trace"), lifecycle=lifecycle, duration_ms=duration_ms
    )
    health = validate_bsc09_health(record.get("health"))
    lifecycle_elapsed = lifecycle["elapsed_ms"]
    browser_elapsed = browser["elapsed_ms"]
    assert isinstance(lifecycle_elapsed, list) and isinstance(browser_elapsed, list)
    if (
        stimuli["start-maintenance-autoconnect"] != lifecycle_elapsed[0]
        or stimuli["open-scan-modal"] != browser_elapsed[0]
        or stimuli["close-scan-modal"] != browser_elapsed[1]
        or stimuli["reopen-scan-modal"] != browser_elapsed[2]
        or stimuli["drop-one-status-poll"] != browser_elapsed[3]
        or stimuli["retry-status-poll"] != browser_elapsed[5]
        or lifecycle["capture_sha256"] != capture_digests["wifi_scan_projection_sha256"]
        or browser["capture_sha256"] != capture_digests["browser_projection_sha256"]
        or heap["capture_sha256"] != capture_digests["heap_projection_sha256"]
        or wifi_mode["capture_sha256"] != capture_digests["wifi_mode_projection_sha256"]
        or health["capture_sha256"] != capture_digests["health_projection_sha256"]
    ):
        raise RunnerError(code, "BSC-09 stimuli or observations are not bound to their captures")
    validate_bsc09_facts(
        record.get("facts"),
        lifecycle=lifecycle,
        browser=browser,
        heap=heap,
        wifi_mode=wifi_mode,
    )
    binding = require_sha256(
        record.get("evidence_binding_sha256"), code=code, label="BSC-09 evidence binding"
    )
    if not secrets.compare_digest(binding, bsc09_record_commitment(record)):
        raise RunnerError(code, "BSC-09 exact record commitment is stale")
    return record


def validate_bsc09_runs(records: Sequence[Mapping[str, object]]) -> None:
    if len(records) != BSC09_REQUIRED_RUNS:
        raise RunnerError("case_runs_incomplete", "BSC-09 requires exactly three records")
    if [record.get("run_index") for record in records] != [1, 2, 3]:
        raise RunnerError("case_runs_reused", "BSC-09 run indices are incomplete or reused")
    if len({record.get("session_id") for record in records}) != 1:
        raise RunnerError("case_runs_reused", "BSC-09 records escaped their collection session")
    for field in ("attempt_id", "evidence_binding_sha256"):
        if len({record.get(field) for record in records}) != BSC09_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", f"BSC-09 {field} values must be distinct")
    generations: list[object] = []
    manifest_bindings: list[object] = []
    before_heap: list[int] = []
    released_heap: list[int] = []
    capture_values: dict[str, list[object]] = {field: [] for field in BSC09_CAPTURE_COMMITMENTS}
    for record in records:
        lifecycle = record.get("scan_lifecycle")
        manifest = record.get("raw_artifact_manifest")
        captures = record.get("capture_commitments")
        if not isinstance(lifecycle, dict) or not isinstance(manifest, dict) or not isinstance(captures, dict):
            raise RunnerError("case_runs_reused", "BSC-09 aggregate record shape is invalid")
        events = lifecycle.get("events")
        if not isinstance(events, list) or not events or not isinstance(events[0], dict):
            raise RunnerError("case_runs_reused", "BSC-09 aggregate lifecycle shape is invalid")
        generations.append(events[0].get("generation"))
        manifest_bindings.append(manifest.get("manifest_commitment_sha256"))
        for field in BSC09_CAPTURE_COMMITMENTS:
            capture_values[field].append(captures.get(field))
        heap_trace = record.get("heap_trace")
        if not isinstance(heap_trace, dict) or not isinstance(heap_trace.get("samples"), list):
            raise RunnerError("case_runs_reused", "BSC-09 aggregate heap shape is invalid")
        heap_samples = heap_trace["samples"]
        if (
            len(heap_samples) != 3
            or not isinstance(heap_samples[0], dict)
            or not isinstance(heap_samples[2], dict)
            or type(heap_samples[0].get("free_heap_bytes")) is not int
            or type(heap_samples[2].get("free_heap_bytes")) is not int
        ):
            raise RunnerError("case_runs_reused", "BSC-09 aggregate heap samples are invalid")
        before_heap.append(heap_samples[0]["free_heap_bytes"])
        released_heap.append(heap_samples[2]["free_heap_bytes"])
    if len(set(generations)) != BSC09_REQUIRED_RUNS:
        raise RunnerError("case_runs_reused", "BSC-09 physical scan generations must be distinct")
    if len(set(manifest_bindings)) != BSC09_REQUIRED_RUNS:
        raise RunnerError("case_runs_reused", "BSC-09 raw manifests must be distinct")
    for field, values in capture_values.items():
        if len(set(values)) != BSC09_REQUIRED_RUNS:
            raise RunnerError("case_runs_reused", f"BSC-09 {field} captures must be distinct")
    if any(released < before for before, released in zip(before_heap, released_heap, strict=True)):
        raise RunnerError("case_runs_reused", "BSC-09 a scan run retained heap after release")
    if released_heap[-1] < released_heap[0]:
        raise RunnerError("case_runs_reused", "BSC-09 scan heap loss accumulated across runs")


def run_bsc09_adapter(
    *,
    adapter: Path,
    adapter_entrypoint: str,
    repository: Path,
    serial_port: str,
    expected: Mapping[str, object],
    environment: Mapping[str, str],
    raw_directory: Path,
    manifest_path: Path,
    raw_role: rig_adapters.AdapterRoleContract,
) -> dict[str, object]:
    profile = expected["qualification_profile"]
    assert isinstance(profile, dict)
    command = [
        sys.executable,
        "-B",
        str(adapter),
        "--entrypoint",
        adapter_entrypoint,
        "--case",
        BSC09_CASE_ID,
        "--role-id",
        str(expected["role_id"]),
        "--run-index",
        str(expected["run_index"]),
        "--profile-id",
        str(profile["profile_id"]),
        "--profile-version",
        str(profile["profile_version"]),
        "--profile-sha256",
        str(profile["profile_sha256"]),
        "--case-descriptor-sha256",
        str(expected["case_descriptor_sha256"]),
        "--adapter-request-sha256",
        str(expected["adapter_request_sha256"]),
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
        "--raw-artifact-dir",
        str(raw_directory),
    ]
    command_started = datetime.now(timezone.utc)
    try:
        completed = subprocess.run(
            command,
            cwd=repository,
            env=dict(environment),
            capture_output=True,
            check=False,
            timeout=BSC09_ADAPTER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise RunnerError("case_adapter_timeout", "BSC-09 adapter exceeded its timeout") from exc
    except OSError as exc:
        raise RunnerError("case_adapter_unavailable", "BSC-09 adapter could not start") from exc
    command_completed = datetime.now(timezone.utc)
    if completed.returncode != 0:
        raise RunnerError("case_adapter_failed", "BSC-09 adapter did not complete successfully")
    if not completed.stdout or len(completed.stdout) > BSC09_MAX_RECORD_BYTES:
        raise RunnerError("case_record_invalid", "BSC-09 adapter response size is invalid")
    try:
        adapter_protocol.parse_adapter_response(
            completed.stdout,
            request={
                "request_commitment_sha256": expected["adapter_request_sha256"],
                "nonce": expected["attempt_id"],
            },
        )
        manifest = adapter_protocol.collect_raw_artifacts(
            raw_directory=raw_directory,
            role=raw_role,
            request_commitment_sha256=str(expected["adapter_request_sha256"]),
            manifest_path=manifest_path,
        )
        raw_by_role = adapter_protocol.read_collected_raw_artifacts(
            raw_directory=raw_directory,
            role=raw_role,
            manifest=manifest,
        )
    except adapter_protocol.AdapterProtocolError as exc:
        raise RunnerError(exc.code, exc.message) from exc
    physical = expected.get("execution_mode") == "physical"
    return build_bsc09_record_from_projections(
        raw_by_role,
        raw_manifest=manifest,
        expected=expected,
        command_started=command_started if physical else None,
        command_completed=command_completed if physical else None,
    )


def resolve_bsc09_adapter_execution(
    args: argparse.Namespace,
    *,
    admission: RigAdapterAdmission,
    repository: Path,
) -> tuple[Path, str, str, bool]:
    """Resolve only the mocked test adapter or the admitted tracked source."""

    if admission.simulated:
        if args.case_adapter is None:
            raise RunnerError("case_adapter_required", "BSC-09 test execution requires a mocked adapter")
        adapter = args.case_adapter.resolve()
        entrypoint = "main"
        source_sha256 = sha256_file(adapter) if adapter.is_file() and not adapter.is_symlink() else ""
        physical = False
    else:
        if args.case_adapter is not None:
            raise RunnerError("untrusted_override", "authoritative BSC-09 forbids an untracked rig adapter")
        descriptor = admission.adapter
        if (
            admission.git_state is None
            or admission.source_sha256 is None
            or descriptor.source_path is None
            or descriptor.entrypoint is None
        ):
            raise RunnerError("case_rig_adapter_unavailable", "BSC-09 tracked adapter admission is incomplete")
        adapter = repository / descriptor.source_path
        entrypoint = descriptor.entrypoint
        source_sha256 = admission.source_sha256
        physical = True
    if not adapter.is_file() or adapter.is_symlink() or not source_sha256:
        raise RunnerError("case_adapter_unavailable", "BSC-09 adapter must be a regular file")
    return adapter, entrypoint, source_sha256, physical


def run_bsc09_case(args: argparse.Namespace) -> int:
    case_descriptor = load_bsc09_case_descriptor()
    role_descriptor = case_descriptor["scenario"]
    assert isinstance(role_descriptor, dict)
    if args.runs != BSC09_REQUIRED_RUNS:
        raise RunnerError("invalid_runs", "BSC-09 requires exactly three runs")
    if args.rig is None:
        raise RunnerError("rig_alias_required", "BSC-09 requires an opaque local rig alias")
    if args.resume or args.production_replay:
        raise RunnerError("unsupported_mode", "BSC-09 has one atomic production collection role")
    admission = admit_case_rig_adapter(
        args,
        case_contract=case_descriptor,
        role_id=str(role_descriptor["role_id"]),
    )

    repository = args.repo_root.resolve()
    git_state = admission.git_state or read_git_state(repository)
    if not git_state.tracked_clean:
        raise RunnerError("dirty_target", "BSC-09 requires a clean target worktree")
    adapter, adapter_entrypoint, adapter_sha, physical = resolve_bsc09_adapter_execution(
        args,
        admission=admission,
        repository=repository,
    )
    dut_resolution, dut_attestation, rig_attestation = resolve_bsc09_hardware(
        args, Path(args.pio_command), case_descriptor
    )
    endpoints = dut_resolution.get("endpoints")
    if not isinstance(endpoints, dict) or not isinstance(endpoints.get("serial_port"), str):
        raise RunnerError("case_board_resolution_failed", "BSC-09 DUT has no serial endpoint")
    serial_port = endpoints["serial_port"]
    if not Path(serial_port).exists():
        raise RunnerError("case_board_resolution_failed", "BSC-09 serial endpoint is not present")

    if args.out_dir is None:
        run_id = datetime.now(timezone.utc).strftime("bsc09-%Y%m%dT%H%M%SZ")
        run_root = ROOT / ".artifacts" / "hil" / "bug_squash_closeout" / run_id
    else:
        run_root = Path(os.path.abspath(args.out_dir))
    require_no_symlink_components(run_root, boundary=Path(os.path.abspath(args.repo_root)).parent)
    if run_root.exists() and (not run_root.is_dir() or any(run_root.iterdir())):
        raise RunnerError("output_not_empty", "BSC-09 output must be new")
    run_root.mkdir(parents=True, exist_ok=True)

    session_id = f"bsc09-{secrets.token_hex(16)}"
    descriptor_sha = bsc09_descriptor_commitment(case_descriptor)
    profile_binding: dict[str, object] = {
        "profile_id": BSC09_PROFILE_ID,
        "profile_version": BSC09_PROFILE_VERSION,
        "profile_sha256": qualification.PINNED_PROFILE_SHA256,
    }
    records: list[dict[str, object]] = []
    attempts: list[dict[str, object]] = []
    for run_index in range(1, BSC09_REQUIRED_RUNS + 1):
        attempt_id = f"attempt-{secrets.token_hex(16)}"
        expected: dict[str, object] = {
            "case_id": BSC09_CASE_ID,
            "role_id": role_descriptor["role_id"],
            "run_index": run_index,
            "session_id": session_id,
            "attempt_id": attempt_id,
            "target_sha": git_state.head_sha,
            "dut_alias": args.board,
            "rig_alias": args.rig,
            "execution_mode": "physical" if physical else "simulated",
            "hardware_observed": physical,
            "qualification_profile": profile_binding,
            "case_descriptor": case_descriptor,
            "case_descriptor_sha256": descriptor_sha,
            "dut_capabilities": list(BSC09_DUT_CAPABILITIES),
            "rig_capabilities": list(BSC09_RIG_CAPABILITIES),
        }
        expected["adapter_request_sha256"] = bsc09_adapter_request_commitment(expected)
        require_unchanged_git_state(repository, git_state)
        raw_directory = run_root / f"raw-attempt-{run_index}"
        raw_directory.mkdir(mode=0o700)
        manifest_path = run_root / f"raw-manifest-{run_index}.json"
        record = run_bsc09_adapter(
            adapter=adapter,
            adapter_entrypoint=adapter_entrypoint,
            repository=repository,
            serial_port=serial_port,
            expected=expected,
            environment=os.environ.copy(),
            raw_directory=raw_directory,
            manifest_path=manifest_path,
            raw_role=admission.adapter.roles[0],
        )
        require_unchanged_git_state(repository, git_state)
        attempt_path = run_root / f"attempt-{run_index}.json"
        write_json_atomic(attempt_path, record)
        records.append(record)
        attempts.append(
            {
                "run_index": run_index,
                "attempt_sha256": hashlib.sha256(attempt_id.encode("ascii")).hexdigest(),
                "record_sha256": sha256_file(attempt_path),
                "scan_generation": record["scan_lifecycle"]["events"][0]["generation"],
            }
        )
    validate_bsc09_runs(records)
    firmware = records[0]["firmware"]
    assert isinstance(firmware, dict)
    blockers = [
        blocker
        for blocker in case_drivers.get_case_driver(BSC09_CASE_ID).qualification_blockers
        if not (physical and blocker == "tracked-rig-adapter-not-implemented")
    ]
    authoritative = physical and not blockers
    result: dict[str, object] = {
        "schema_version": 1,
        "run_kind": "bug-squash-bsc09-wifi-dual-consumer-scan",
        "case_id": BSC09_CASE_ID,
        "collection_role": role_descriptor["role_id"],
        "case_descriptor_sha256": descriptor_sha,
        "target_sha": git_state.head_sha,
        "session_sha256": hashlib.sha256(session_id.encode("ascii")).hexdigest(),
        "execution_mode": "physical" if physical else "simulated",
        "hardware_observed": physical,
        "authoritative": authoritative,
        "physical_collection_completed": physical,
        "non_qualifying": not authoritative,
        "qualification_status": "PASS" if authoritative else "BLOCKED",
        "qualification_blockers": blockers,
        "artifact_role": "authoritative-case-collection" if authoritative else "non-qualifying-case-collection",
        "result": "TEST_PASS",
        "runs_required": BSC09_REQUIRED_RUNS,
        "runs_completed": len(records),
        "production_replay_required": False,
        "firmware_target": {
            "environment": firmware["environment"],
            "build_kind": firmware["build_kind"],
            "target_sha": firmware["target_sha"],
            "binary_sha256": firmware["binary_sha256"],
            "hil_fault_control_active": firmware["hil_fault_control_active"],
        },
        "attempts": attempts,
        "artifact_sha256": {
            "adapter": adapter_sha,
            "runner": sha256_file(Path(__file__)),
            "inventory": sha256_file(args.inventory.resolve()),
            "dut_attestation": canonical_case_commitment(
                "v1simple.bsc09.dut-attestation.v1", dut_attestation
            ),
            "rig_attestation": canonical_case_commitment(
                "v1simple.bsc09.rig-attestation.v1", rig_attestation
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
