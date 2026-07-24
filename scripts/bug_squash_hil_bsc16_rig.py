#!/usr/bin/env python3
"""Supervised physical POWER-rig adapter for the BSC-16 qualification case.

The adapter owns firmware installation, serial capture, ordered operator
checkpoints, and import of a real GPIO16 logic-analyzer export.  It emits only
the strict record consumed by ``run_bug_squash_hil.py``; raw operational data
stays in the runner-owned ignored artifact directory.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time
from typing import Callable, Mapping, Sequence

import serial
import resolve_hil_board


ROOT = Path(__file__).resolve().parents[1]
FULL_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
OPAQUE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
HIL_SERIAL_BURST_MAX_BYTES = 512
BOOT_CLASSIFICATION_RE = re.compile(
    r"\[Battery\] Power detection: classification=(battery|usb|unknown) "
    r"reported=(battery|usb|unknown)",
    re.IGNORECASE,
)
CHANGE_CLASSIFICATION_RE = re.compile(
    r"\[Battery\] Power source changed: (battery|usb|unknown)",
    re.IGNORECASE,
)
STABLE_CLASSIFICATION_RE = re.compile(
    r"\[Battery\] Power source stable: (battery|usb|unknown)",
    re.IGNORECASE,
)
# This board has no USB/VBUS sense independent of battery presence: GPIO16 reads
# HIGH whenever the 18650 is installed, so a USB cold boot is legitimately
# classified BATTERY and never emits a "usb" line (confirmed on the wire,
# 2026-07-24). usb-cold-boot therefore verifies a real cold boot to the idle-ready
# gate, not an impossible source reclassification.
BOOT_BANNER_RE = re.compile(r"V1 Gen2 Simple Display", re.IGNORECASE)
IDLE_READY_RE = re.compile(
    r"(?:\[Boot\] Ready gate opened|Setup complete - BLE scanning)",
    re.IGNORECASE,
)
FAULT_SESSION_DURATION_MS = 180000
FAULT_EVENTS = ("ready", "fired", "released")
FAULT_STIMULI = (
    "pwr-wake-on-battery",
    "usb-cold-boot",
    "force-adc-init-failure",
    "hold-power-button",
    "transition-battery-to-usb",
    "transition-usb-to-battery",
)
PRODUCTION_STIMULI = (
    "pwr-wake-on-battery",
    "usb-cold-boot",
    "hold-power-button",
    "transition-battery-to-usb",
    "transition-usb-to-battery",
)
ARTIFACT_FILENAMES = {
    "build_evidence_sha256": "build-evidence.json",
    "logic_analyzer_sha256": "logic-analyzer-capture",
    "poweroff_log_sha256": "poweroff.log",
    "serial_log_sha256": "serial.log",
    "source_transitions_sha256": "source-transitions.ndjson",
}


@dataclass(frozen=True)
class Checkpoint:
    target_state: str
    setup_instruction: str
    action_instruction: str
    observed_pass: str
    duration_seconds: float


CHECKPOINTS = {
    "pwr-wake-on-battery": Checkpoint(
        target_state=(
            "cable: DATA | battery rail: ON | "
            "DUT: HARD OFF (PWR cold-boots) | analyzer: ARMED"
        ),
        setup_instruction=(
            "Begin with the uploaded DUT running on FULL. While it is still running, "
            "replace FULL with DATA before shutdown. Keep the battery rail ON and wait at "
            "least two seconds for battery classification, then hold PWR until the screen "
            "goes dark and release promptly. This battery-only shutdown should hard-cut the "
            "latch. Arm the analyzer."
        ),
        action_instruction="Press PWR once to cold-boot the DUT from battery.",
        observed_pass="The screen lit and the DUT reached the idle screen within the window.",
        duration_seconds=7.0,
    ),
    "usb-cold-boot": Checkpoint(
        target_state=(
            "cable: NONE | battery rail: ON | "
            "DUT: HARD OFF (battery latch open) | FULL aligned at port"
        ),
        setup_instruction=(
            "With the DUT running on DATA, hold PWR until the screen goes dark and release. "
            "Keep the 18650 installed and the battery rail ON; the open latch already leaves "
            "the board unpowered. Disconnect DATA and hold FULL aligned at the USB port."
        ),
        action_instruction=(
            "Connect FULL to cold-boot the DUT; do not press PWR. It boots to the idle "
            "screen in a few seconds. With the 18650 installed the board correctly "
            "reports BATTERY (it has no separate USB power sense), so the adapter checks "
            "for a clean cold boot to idle, not a USB reclassification."
        ),
        observed_pass="The DUT booted from the FULL-cable connection and reached the idle screen.",
        # Verifies a cold boot reaching the idle-ready gate. Boot-to-idle lands ~2-3 s
        # after the plug, plus USB-CDC enumeration (~1-3 s); a 10 s window with an
        # evidence-gated early stop covers it comfortably.
        duration_seconds=10.0,
    ),
    "force-adc-init-failure": Checkpoint(
        target_state=(
            "cable: DATA | battery rail: OFF | "
            "DUT: HARD OFF | fault-session clock: RUNNING"
        ),
        setup_instruction=(
            "The one-shot fault is staged and its bounded clock is running. Replace FULL with "
            "DATA, then turn the battery rail OFF. Leave the DUT dark and do not press PWR yet."
        ),
        action_instruction=(
            "Turn the battery rail ON, then immediately press PWR once to cold-boot the DUT."
        ),
        observed_pass=(
            "The screen lit and the DUT remained operable; a degraded battery reading is "
            "expected for this injected ADC failure."
        ),
        duration_seconds=9.0,
    ),
    "hold-power-button": Checkpoint(
        target_state="cable: DATA | battery rail: ON | DUT: ON BATTERY",
        setup_instruction=(
            "Ensure DATA is connected and the battery rail is ON. If FULL is still attached "
            "after the production cold boot, replace it with DATA, then wait at least two "
            "seconds for battery classification. Confirm the DUT is running before continuing."
        ),
        action_instruction=(
            "Hold PWR until the screen goes dark, then release it promptly."
        ),
        observed_pass="The screen went dark as a direct result of the PWR hold.",
        duration_seconds=7.0,
    ),
    "transition-battery-to-usb": Checkpoint(
        target_state="cable: DATA | battery rail: ON | DUT: ON BATTERY | FULL ready",
        setup_instruction=(
            "The prior checkpoint left the DUT HARD OFF. Press PWR once to cold-boot it from "
            "battery and wait for the idle screen. Keep DATA connected and hold FULL ready."
        ),
        action_instruction=(
            "Replace DATA with FULL in a single motion while keeping the battery rail ON."
        ),
        observed_pass="The DUT stayed running through the swap and remained on the idle screen.",
        duration_seconds=7.0,
    ),
    "transition-usb-to-battery": Checkpoint(
        target_state="cable: FULL | battery rail: ON | DUT: ON USB | DATA ready",
        setup_instruction=(
            "Confirm the DUT is running with FULL and the battery rail ON. Hold DATA aligned "
            "and ready, but do not unplug FULL yet."
        ),
        action_instruction=(
            "Replace FULL with DATA in a single motion while keeping the battery rail ON."
        ),
        observed_pass="The DUT stayed running through the swap and remained on the idle screen.",
        duration_seconds=7.0,
    ),
}


class AdapterError(RuntimeError):
    """Fail-closed physical adapter error safe to summarize."""


class SerialEndpointResolver:
    """Re-resolve the exact USB identity after every destructive power action."""

    def __init__(
        self,
        *,
        template: Path,
        inventory: Path,
        dut_alias: str,
        pio_command: str,
    ) -> None:
        self.template = template
        self.inventory = inventory
        self.dut_alias = dut_alias
        self.pio_command = pio_command

    def __call__(self) -> str:
        try:
            inventory = resolve_hil_board.load_inventory(
                self.template,
                self.inventory,
            )
            resolution = resolve_hil_board.resolve_board(
                inventory,
                self.dut_alias,
                ("serial",),
                port_records=resolve_hil_board.enumerate_serial_ports(
                    self.pio_command
                ),
            )
        except resolve_hil_board.ResolverError as exc:
            raise AdapterError("exact USB-serial endpoint is not currently resolvable") from exc
        endpoints = resolution.get("endpoints")
        if not isinstance(endpoints, dict) or not isinstance(
            endpoints.get("serial_port"), str
        ):
            raise AdapterError("exact USB-serial resolution lacks a serial endpoint")
        return endpoints["serial_port"]


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def canonical_json_bytes(payload: object) -> bytes:
    return json.dumps(
        payload,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def commitment(payload: Mapping[str, object]) -> str:
    return hashlib.sha256(
        b"v1simple.bsc16.case-record.v1\0" + canonical_json_bytes(payload)
    ).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_bytes_exclusive(path: Path, content: bytes) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags, 0o600)
    try:
        written = 0
        while written < len(content):
            count = os.write(descriptor, content[written:])
            if count <= 0:
                raise AdapterError("artifact write was incomplete")
            written += count
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_json_exclusive(path: Path, payload: object) -> None:
    write_bytes_exclusive(path, canonical_json_bytes(payload) + b"\n")


def require_opaque(value: str, label: str) -> str:
    if OPAQUE_RE.fullmatch(value) is None:
        raise AdapterError(f"{label} is not a safe opaque identity")
    return value


def prompt_install_ready(role: str) -> None:
    environment = "fault-instrumented" if role == "fault-collection" else "production"
    print(
        f"\nBSC-16 INSTALL PREP — {environment} build\n"
        "TARGET STATE: cable: FULL | battery rail: ON | DUT: ON (screen lit)\n"
        "The adapter will build and upload firmware. This normally takes 2–3 minutes and "
        "may be silent because build output is captured as evidence. Touch nothing until "
        "the first checkpoint appears.\n"
        "Type READY only when the target state is true.",
        file=sys.stderr,
        flush=True,
    )
    if input().strip() != "READY":
        raise AdapterError("firmware-install setup was not acknowledged exactly")


def prompt_fault_staging_ready() -> None:
    duration_seconds = FAULT_SESSION_DURATION_MS // 1000
    print(
        "\nBSC-16 FAULT STAGING PREP\n"
        "Keep the FULL cable connected and the DUT running. Put the DATA cable within reach "
        "and identify the battery-rail OFF/ON control now. After staging, the "
        f"{duration_seconds}-second fault-session clock will already be running.\n"
        "Type READY only when you can perform the CP3 cable swap and rail action without delay.",
        file=sys.stderr,
        flush=True,
    )
    if input().strip() != "READY":
        raise AdapterError("fault-staging setup was not acknowledged exactly")


def announce_fault_staging() -> None:
    print(
        "\nDO NOT TOUCH — staging the one-shot fault over FULL serial now. "
        "Wait for the CP3 target-state prompt.",
        file=sys.stderr,
        flush=True,
    )


def prompt_ready(
    *,
    target_state: str,
    setup_instruction: str,
    action_instruction: str,
    duration_seconds: float,
) -> None:
    duration_text = (
        str(int(duration_seconds))
        if duration_seconds.is_integer()
        else str(duration_seconds)
    )
    print(
        f"\nTARGET STATE: {target_state}\nCAPTURE WINDOW: {duration_text} s",
        file=sys.stderr,
        flush=True,
    )
    print(
        f"\nBSC-16 SETUP ONLY: {setup_instruction}",
        file=sys.stderr,
        flush=True,
    )
    print(
        "Do not perform the timed action yet. Type READY only when this setup is complete.",
        file=sys.stderr,
        flush=True,
    )
    if input().strip() != "READY":
        raise AdapterError("operator checkpoint was not acknowledged exactly")
    print(
        f"\nNEXT TIMED ACTION: {action_instruction}",
        file=sys.stderr,
        flush=True,
    )
    print(
        "Get physically ready. Type START, then perform that exact action immediately "
        "after sending START; do not wait for another message.",
        file=sys.stderr,
        flush=True,
    )
    if input().strip() != "START":
        raise AdapterError("timed action was not acknowledged exactly")
    print(
        f"CAPTURE STARTED — ACTION NOW: {action_instruction}",
        file=sys.stderr,
        flush=True,
    )


def prompt_pass(question: str) -> None:
    print(question, file=sys.stderr, flush=True)
    print("Type PASS only for a directly observed pass.", file=sys.stderr, flush=True)
    if input().strip() != "PASS":
        raise AdapterError("physical observation did not pass")


def open_serial_endpoint(serial_port: str, timeout: float) -> serial.Serial:
    handle = serial.Serial(port=None, baudrate=115200, timeout=timeout)
    handle.dtr = False
    handle.rts = False
    handle.port = serial_port
    handle.open()
    return handle


def capture_serial(
    resolve_serial_port: Callable[[], str],
    *,
    duration_seconds: float,
    run_started: float,
    stimulus_id: str,
    stop_when: Callable[[Sequence[Mapping[str, object]]], bool] | None = None,
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    deadline = time.monotonic() + duration_seconds
    opened = False
    while time.monotonic() < deadline:
        try:
            serial_port = resolve_serial_port()
            handle = open_serial_endpoint(serial_port, 0.2)
        except (AdapterError, OSError, serial.SerialException):
            time.sleep(0.5)
            continue
        opened = True
        with handle:
            while time.monotonic() < deadline:
                try:
                    raw = handle.readline()
                except (OSError, serial.SerialException):
                    break
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                rows.append(
                    {
                        "observed_at_utc": utc_now(),
                        "elapsed_ms": int((time.monotonic() - run_started) * 1000),
                        "stimulus_id": stimulus_id,
                        "line": text,
                    }
                )
                # Stop as soon as the required evidence is captured so the window is a
                # bounded cap, not a forced wait, and operator plug latency inside it no
                # longer decides pass/fail. Only positive, presence-based predicates are
                # passed here — never absence checks, which a later line could still fail.
                if stop_when is not None and stop_when(rows):
                    return rows
        if time.monotonic() < deadline:
            time.sleep(0.2)
    if not opened:
        raise AdapterError("serial endpoint could not be opened for physical capture")
    return rows


def send_hil_commands(
    resolve_serial_port: Callable[[], str],
    commands: Sequence[str],
    *,
    run_started: float,
    serial_rows: list[dict[str, object]],
) -> float:
    if not commands:
        raise AdapterError("HIL serial command sequence is empty")
    content = b"".join(command.encode("ascii") + b"\n" for command in commands)
    if len(content) > HIL_SERIAL_BURST_MAX_BYTES:
        raise AdapterError("HIL serial command sequence exceeds the bounded RX budget")
    try:
        serial_port = resolve_serial_port()
        handle = open_serial_endpoint(serial_port, 0.2)
    except (AdapterError, OSError, serial.SerialException) as exc:
        raise AdapterError("HIL serial endpoint could not be opened") from exc
    accepted = 0
    with handle:
        time.sleep(1.0)
        handle.reset_input_buffer()
        sent_at = time.monotonic()
        handle.write(content)
        handle.flush()
        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline and accepted < len(commands):
            raw = handle.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            serial_rows.append(
                {
                    "observed_at_utc": utc_now(),
                    "elapsed_ms": int((time.monotonic() - run_started) * 1000),
                    "stimulus_id": "force-adc-init-failure",
                    "line": text,
                }
            )
            try:
                payload = json.loads(text)
            except json.JSONDecodeError:
                continue
            if (
                isinstance(payload, dict)
                and payload.get("ok") is True
                and payload.get("parse") == "ok"
                and payload.get("result") == "ok"
            ):
                accepted += 1
    if accepted != len(commands):
        raise AdapterError("HIL serial command sequence was not positively acknowledged")
    return sent_at


def run_firmware_install(
    environment: str,
    resolve_serial_port: Callable[[], str],
    target_sha: str,
) -> tuple[str, dict[str, object]]:
    command_evidence: list[dict[str, object]] = []

    def run_command(command: list[str]) -> None:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            check=False,
            timeout=1_800,
        )
        combined = completed.stdout + completed.stderr
        command_evidence.append(
            {
                "argv": command[:4] + (["<local-port>"] if "--upload-port" in command else []),
                "returncode": completed.returncode,
                "output_sha256": hashlib.sha256(combined).hexdigest(),
            }
        )
        if completed.returncode != 0:
            raise AdapterError("firmware build or upload failed")

    run_command(["pio", "run", "-e", environment])
    serial_port = resolve_serial_port()
    run_command(
        [
            "pio",
            "run",
            "-e",
            environment,
            "-t",
            "upload",
            "--upload-port",
            serial_port,
        ]
    )
    binary = ROOT / ".pio" / "build" / environment / "firmware.bin"
    if not binary.is_file() or binary.is_symlink() or binary.stat().st_size <= 0:
        raise AdapterError("firmware binary is unavailable after upload")
    binary_sha = sha256_file(binary)
    return binary_sha, {
        "schema_version": 1,
        "case_id": "BSC-16",
        "target_sha": target_sha,
        "environment": environment,
        "binary_sha256": binary_sha,
        "fault_instrumented": environment == "waveshare-349-hil",
        "commands": command_evidence,
    }


def classifications(rows: Sequence[Mapping[str, object]], stimulus_id: str) -> list[tuple[int, str]]:
    found: list[tuple[int, str]] = []
    for row in rows:
        if row.get("stimulus_id") != stimulus_id or not isinstance(row.get("line"), str):
            continue
        boot_match = BOOT_CLASSIFICATION_RE.search(row["line"])
        change_match = CHANGE_CLASSIFICATION_RE.search(row["line"])
        stable_match = STABLE_CLASSIFICATION_RE.search(row["line"])
        value = (
            boot_match.group(1)
            if boot_match is not None
            else change_match.group(1)
            if change_match is not None
            else stable_match.group(1)
            if stable_match is not None
            else None
        )
        if value is not None and isinstance(row.get("elapsed_ms"), int):
            found.append((row["elapsed_ms"], value.lower()))
    return found


def require_classification(
    rows: Sequence[Mapping[str, object]], stimulus_id: str, expected: str
) -> list[tuple[int, str]]:
    observed = classifications(rows, stimulus_id)
    if not observed or expected not in {value for _, value in observed}:
        raise AdapterError(
            f"required {expected} source classification was not captured for {stimulus_id}"
        )
    return observed


def usb_cold_boot_reached_idle(rows: Sequence[Mapping[str, object]]) -> bool:
    """True once a fresh boot banner and the idle-ready gate are both captured.

    The board cannot report ``usb`` while the cell is installed (no VBUS sense),
    so usb-cold-boot proves a *cold boot from the USB plug that reaches idle*
    instead. Requiring the firmware boot banner AND the ready/idle line rules out
    a stale pre-plug log or a partial boot. Doubles as the ``capture_serial``
    early-stop predicate so the window is a cap, not a forced wait.
    """
    saw_banner = False
    saw_idle = False
    for _, line in _usb_cold_boot_lines(rows):
        if BOOT_BANNER_RE.search(line):
            saw_banner = True
        if IDLE_READY_RE.search(line):
            saw_idle = True
    return saw_banner and saw_idle


def _usb_cold_boot_lines(
    rows: Sequence[Mapping[str, object]],
) -> list[tuple[int, str]]:
    lines: list[tuple[int, str]] = []
    for row in rows:
        if row.get("stimulus_id") != "usb-cold-boot" or not isinstance(row.get("line"), str):
            continue
        elapsed = row.get("elapsed_ms")
        lines.append((elapsed if isinstance(elapsed, int) else 0, row["line"]))
    return lines


def source_flapped(rows: Sequence[Mapping[str, object]], stimulus_ids: Sequence[str]) -> bool:
    for stimulus_id in stimulus_ids:
        values = [value for _, value in classifications(rows, stimulus_id)]
        collapsed = [value for index, value in enumerate(values) if index == 0 or value != values[index - 1]]
        if len(collapsed) >= 3 and collapsed[0] == collapsed[2]:
            return True
    return False


def parse_fault_lifecycle(rows: Sequence[Mapping[str, object]]) -> list[dict[str, object]]:
    lifecycle: list[dict[str, object]] = []
    for row in rows:
        line = row.get("line")
        if not isinstance(line, str):
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(payload, dict) or payload.get("hil_event") not in FAULT_EVENTS:
            continue
        lifecycle.append(
            {
                "id": payload.get("hil_event"),
                "sequence": len(lifecycle) + 1,
                "elapsed_ms": row["elapsed_ms"],
                "arm_sequence": payload.get("arm_sequence"),
                "ready_sequence": payload.get("ready_sequence"),
                "generation": payload.get("generation"),
                "phase": payload.get("phase"),
                "latch_initialized": payload.get("latch_initialized"),
                "adc_handle_allocated": payload.get("adc_handle_allocated"),
                "voltage_valid": payload.get("voltage_valid"),
                "source_classification": payload.get("source_classification"),
                "power_button_enabled": payload.get("power_button_enabled"),
            }
        )
    if tuple(row["id"] for row in lifecycle) != FAULT_EVENTS:
        raise AdapterError("complete BSC-16 fault lifecycle was not captured")
    return lifecycle


def validate_fault_lifecycle_capture(rows: Sequence[Mapping[str, object]]) -> None:
    lifecycle = parse_fault_lifecycle(rows)
    identity: tuple[int, int, int, int] | None = None
    previous_elapsed_ms = -1
    for row in lifecycle:
        current_identity = tuple(
            row[field]
            for field in ("arm_sequence", "ready_sequence", "generation", "phase")
        )
        elapsed_ms = row["elapsed_ms"]
        if (
            type(elapsed_ms) is not int
            or elapsed_ms < previous_elapsed_ms
            or any(type(item) is not int or item <= 0 for item in current_identity[:3])
            or type(current_identity[3]) is not int
            or current_identity[3] != 1
            or row["latch_initialized"] is not True
            or row["adc_handle_allocated"] is not False
            or row["voltage_valid"] is not False
            or row["source_classification"] not in {"battery", "unknown"}
            or row["power_button_enabled"] is not True
        ):
            raise AdapterError("BSC-16 ADC fault lifecycle did not preserve required behavior")
        if identity is None:
            identity = current_identity
        elif current_identity != identity:
            raise AdapterError("BSC-16 ADC fault lifecycle identity changed during capture")
        previous_elapsed_ms = elapsed_ms


def validate_stimulus_capture(
    stimulus_id: str,
    rows: Sequence[Mapping[str, object]],
) -> None:
    if stimulus_id == "pwr-wake-on-battery":
        observed = require_classification(rows, stimulus_id, "battery")
        if any(value == "usb" for _, value in observed):
            raise AdapterError("PWR wake transiently classified battery operation as usb")
        return
    if stimulus_id == "usb-cold-boot":
        # No usb reclassification is asserted: with the cell installed GPIO16 reads
        # HIGH and the board correctly classifies BATTERY on a USB cold boot. Verify
        # the real, observable outcome instead — a fresh cold boot reaching idle.
        if not usb_cold_boot_reached_idle(rows):
            raise AdapterError(
                "usb cold boot did not capture a fresh boot reaching the idle-ready gate"
            )
        return
    if stimulus_id == "force-adc-init-failure":
        validate_fault_lifecycle_capture(rows)
        return
    if stimulus_id == "hold-power-button":
        if any(value == "usb" for _, value in classifications(rows, stimulus_id)):
            raise AdapterError("PWR hold was incorrectly classified as usb")
        return
    if stimulus_id == "transition-battery-to-usb":
        require_classification(rows, stimulus_id, "usb")
    elif stimulus_id == "transition-usb-to-battery":
        require_classification(rows, stimulus_id, "battery")
    else:
        raise AdapterError("unknown BSC-16 stimulus cannot be validated")
    if source_flapped(rows, (stimulus_id,)):
        raise AdapterError(f"source-classification flapping was captured for {stimulus_id}")


def import_logic_capture(destination: Path) -> int:
    print(
        "Stop the analyzer and export a digital CSV no larger than 32 MB. The CSV needs "
        "a time column whose name contains \"time\" and a channel column named exactly "
        "gpio16 (case-insensitive). Samples must be digital 0/1. Seconds are assumed unless "
        "the time-column name contains \"ms\". Include at least one clean press and release, "
        "then type the local CSV path.",
        file=sys.stderr,
        flush=True,
    )
    source = Path(input().strip()).resolve()
    try:
        metadata = source.stat(follow_symlinks=False)
    except OSError as exc:
        raise AdapterError("logic-analyzer capture is unavailable") from exc
    if (
        not stat.S_ISREG(metadata.st_mode)
        or metadata.st_nlink != 1
        or not 1 <= metadata.st_size <= 32 * 1024 * 1024
        or source.is_symlink()
    ):
        raise AdapterError("logic-analyzer capture is not a safe bounded regular file")
    with source.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise AdapterError("logic-analyzer CSV lacks headers")
        time_name = next(
            (name for name in reader.fieldnames if "time" in name.strip().lower()),
            None,
        )
        gpio_name = next(
            (name for name in reader.fieldnames if name.strip().lower() == "gpio16"),
            None,
        )
        if time_name is None or gpio_name is None:
            raise AdapterError("logic-analyzer CSV requires time and gpio16 columns")
        scale = 1000.0 if "ms" not in time_name.lower() else 1.0
        samples: list[tuple[float, int]] = []
        for raw in reader:
            try:
                timestamp_ms = float(raw[time_name]) * scale
                level = int(raw[gpio_name])
            except (KeyError, TypeError, ValueError) as exc:
                raise AdapterError("logic-analyzer CSV contains an invalid sample") from exc
            if level not in {0, 1}:
                raise AdapterError("logic-analyzer GPIO16 samples must be digital")
            samples.append((timestamp_ms, level))
    if len(samples) < 2 or any(later[0] <= earlier[0] for earlier, later in zip(samples, samples[1:])):
        raise AdapterError("logic-analyzer capture is incomplete or unordered")
    transitions = [
        timestamp
        for (timestamp, level), (_, previous) in zip(samples[1:], samples)
        if level != previous
    ]
    if len(transitions) < 2:
        raise AdapterError("logic-analyzer capture lacks representative press and release edges")
    cluster_start = transitions[0]
    previous = transitions[0]
    maximum_bounce = 0.0
    for timestamp in transitions[1:]:
        if timestamp - previous > 25.0:
            maximum_bounce = max(maximum_bounce, previous - cluster_start)
            cluster_start = timestamp
        previous = timestamp
    maximum_bounce = max(maximum_bounce, previous - cluster_start)
    bounce_ms = int(round(maximum_bounce))
    if bounce_ms > 24:
        raise AdapterError("measured GPIO16 bounce exceeds the qualified bound")
    shutil.copyfile(source, destination, follow_symlinks=False)
    return bounce_ms


def perform_stimulus(
    *,
    checkpoint: Checkpoint,
    stimulus_id: str,
    run_started: float,
    resolve_serial_port: Callable[[], str],
    serial_rows: list[dict[str, object]],
    stimuli: list[dict[str, object]],
) -> None:
    prompt_ready(
        target_state=checkpoint.target_state,
        setup_instruction=checkpoint.setup_instruction,
        action_instruction=checkpoint.action_instruction,
        duration_seconds=checkpoint.duration_seconds,
    )
    started_ms = int((time.monotonic() - run_started) * 1000)
    stop_when = (
        usb_cold_boot_reached_idle if stimulus_id == "usb-cold-boot" else None
    )
    captured = capture_serial(
        resolve_serial_port,
        duration_seconds=checkpoint.duration_seconds,
        run_started=run_started,
        stimulus_id=stimulus_id,
        stop_when=stop_when,
    )
    serial_rows.extend(captured)
    validate_stimulus_capture(stimulus_id, captured)
    prompt_pass(checkpoint.observed_pass)
    stimuli.append(
        {
            "id": stimulus_id,
            "sequence": len(stimuli) + 1,
            "elapsed_ms": started_ms,
            "result": "pass",
        }
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", required=True)
    parser.add_argument("--role", choices=("fault-collection", "production-replay"), required=True)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--attempt-id", required=True)
    parser.add_argument("--target-sha", required=True)
    parser.add_argument("--dut-alias", required=True)
    parser.add_argument("--rig-alias", required=True)
    parser.add_argument("--serial-port", required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--pio-command", required=True)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--raw-artifact-request-sha256", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.case != "BSC-16" or FULL_SHA_RE.fullmatch(args.target_sha) is None:
            raise AdapterError("case or target identity is invalid")
        if SHA256_RE.fullmatch(args.raw_artifact_request_sha256) is None:
            raise AdapterError("raw-artifact request identity is invalid")
        for value, label in (
            (args.session_id, "session"),
            (args.attempt_id, "attempt"),
            (args.dut_alias, "DUT alias"),
            (args.rig_alias, "rig alias"),
        ):
            require_opaque(value, label)
        if not sys.stdin.isatty():
            raise AdapterError("physical BSC-16 requires an interactive bench operator")
        artifact_root = args.artifact_dir.resolve()
        if (
            not artifact_root.is_dir()
            or artifact_root.is_symlink()
            or any(artifact_root.iterdir())
        ):
            raise AdapterError("runner-owned artifact directory is invalid")

        started_at = utc_now()
        run_started = time.monotonic()
        environment = "waveshare-349-hil" if args.role == "fault-collection" else "waveshare-349"
        resolve_serial_port = SerialEndpointResolver(
            template=args.template,
            inventory=args.inventory,
            dut_alias=args.dut_alias,
            pio_command=args.pio_command,
        )
        print(
            f"\nStarting BSC-16 {args.role}. Keep USB VBUS isolated for every true battery cut.",
            file=sys.stderr,
            flush=True,
        )
        prompt_install_ready(args.role)
        binary_sha, build_evidence = run_firmware_install(
            environment, resolve_serial_port, args.target_sha
        )
        build_evidence["evidence_label"] = (
            "fault-instrumented"
            if args.role == "fault-collection"
            else "blocking-production-replay"
        )
        build_evidence["raw_artifact_request_sha256"] = args.raw_artifact_request_sha256
        write_json_exclusive(artifact_root / "build-evidence.json", build_evidence)

        serial_rows: list[dict[str, object]] = []
        stimuli: list[dict[str, object]] = []
        poweroff_rows: list[dict[str, object]] = []

        perform_stimulus(
            checkpoint=CHECKPOINTS["pwr-wake-on-battery"],
            stimulus_id="pwr-wake-on-battery",
            run_started=run_started,
            resolve_serial_port=resolve_serial_port,
            serial_rows=serial_rows,
            stimuli=stimuli,
        )
        perform_stimulus(
            checkpoint=CHECKPOINTS["usb-cold-boot"],
            stimulus_id="usb-cold-boot",
            run_started=run_started,
            resolve_serial_port=resolve_serial_port,
            serial_rows=serial_rows,
            stimuli=stimuli,
        )

        if args.role == "fault-collection":
            session_hash = hashlib.sha256(args.session_id.encode("ascii")).hexdigest()
            prompt_fault_staging_ready()
            announce_fault_staging()
            session_sent_at = send_hil_commands(
                resolve_serial_port,
                (
                    f"V1HIL BEGIN BSC-16 {session_hash} {FAULT_SESSION_DURATION_MS}",
                    f"V1HIL ARM BSC-16 battery-adc-init-fail-once {session_hash} 7",
                    f"V1HIL NEXT_BOOT BSC-16 battery-adc-init-fail-once {session_hash} 7",
                ),
                run_started=run_started,
                serial_rows=serial_rows,
            )
            remaining_seconds = max(
                0,
                int(
                    FAULT_SESSION_DURATION_MS / 1000
                    - (time.monotonic() - session_sent_at)
                ),
            )
            print(
                "\nFAULT STAGED — COUNTDOWN RUNNING: "
                f"approximately {remaining_seconds} seconds remain. "
                "Acknowledgement time has already been deducted; complete CP3 promptly.",
                file=sys.stderr,
                flush=True,
            )
            if remaining_seconds == 0:
                raise AdapterError("fault session expired before the CP3 setup prompt")
            perform_stimulus(
                checkpoint=CHECKPOINTS["force-adc-init-failure"],
                stimulus_id="force-adc-init-failure",
                run_started=run_started,
                resolve_serial_port=resolve_serial_port,
                serial_rows=serial_rows,
                stimuli=stimuli,
            )

        perform_stimulus(
            checkpoint=CHECKPOINTS["hold-power-button"],
            stimulus_id="hold-power-button",
            run_started=run_started,
            resolve_serial_port=resolve_serial_port,
            serial_rows=serial_rows,
            stimuli=stimuli,
        )
        poweroff_rows.append(
            {
                "observed_at_utc": utc_now(),
                "role": args.role,
                "stimulus_id": "hold-power-button",
                "intentional_shutdown_observed": True,
                "vbus_isolated": True,
            }
        )
        perform_stimulus(
            checkpoint=CHECKPOINTS["transition-battery-to-usb"],
            stimulus_id="transition-battery-to-usb",
            run_started=run_started,
            resolve_serial_port=resolve_serial_port,
            serial_rows=serial_rows,
            stimuli=stimuli,
        )
        perform_stimulus(
            checkpoint=CHECKPOINTS["transition-usb-to-battery"],
            stimulus_id="transition-usb-to-battery",
            run_started=run_started,
            resolve_serial_port=resolve_serial_port,
            serial_rows=serial_rows,
            stimuli=stimuli,
        )
        expected_stimuli = (
            FAULT_STIMULI
            if args.role == "fault-collection"
            else PRODUCTION_STIMULI
        )
        if tuple(str(row["id"]) for row in stimuli) != expected_stimuli:
            raise AdapterError("physical stimulus sequence is incomplete")

        pwr = require_classification(serial_rows, "pwr-wake-on-battery", "battery")
        if not usb_cold_boot_reached_idle(serial_rows):
            raise AdapterError(
                "usb cold boot did not capture a fresh boot reaching the idle-ready gate"
            )
        require_classification(serial_rows, "transition-battery-to-usb", "usb")
        require_classification(serial_rows, "transition-usb-to-battery", "battery")
        flapped = source_flapped(
            serial_rows,
            ("transition-battery-to-usb", "transition-usb-to-battery"),
        )
        if flapped:
            raise AdapterError("source-classification flapping was captured")

        bounce_ms = import_logic_capture(artifact_root / "logic-analyzer-capture")
        lifecycle = (
            parse_fault_lifecycle(serial_rows)
            if args.role == "fault-collection"
            else []
        )
        if args.role == "fault-collection":
            long_hold = classifications(serial_rows, "hold-power-button")
            facts = {
                "pwr-wake-transient-usb-observed": any(value == "usb" for _, value in pwr),
                "usb-cold-boot-reached-idle": True,
                "adc-failure-voltage-degraded": all(
                    row["voltage_valid"] is False for row in lifecycle
                ),
                "adc-failure-power-button-operational": all(
                    row["power_button_enabled"] is True for row in lifecycle
                ),
                "long-hold-classified-as-usb": any(
                    value == "usb" for _, value in long_hold
                ),
                "long-hold-shutdown-succeeded": True,
                "source-flapping-observed": False,
                "gpio16-bounce-ms": bounce_ms,
            }
        else:
            facts = {
                "battery-classification-correct": not any(
                    value == "usb" for _, value in pwr
                ),
                "usb-cold-boot-reached-idle": True,
                "power-button-operational": True,
                "source-flapping-observed": False,
                "hil-fault-control-active": False,
            }

        serial_bytes = b"".join(
            canonical_json_bytes(row) + b"\n" for row in serial_rows
        )
        transitions = [
            {
                "schema_version": 1,
                "role": args.role,
                "stimulus_id": stimulus["id"],
                "sequence": stimulus["sequence"],
                "elapsed_ms": stimulus["elapsed_ms"],
                "classifications": [
                    {"elapsed_ms": elapsed, "classification": value}
                    for elapsed, value in classifications(serial_rows, str(stimulus["id"]))
                ],
            }
            for stimulus in stimuli
        ]
        write_bytes_exclusive(artifact_root / "serial.log", serial_bytes)
        write_bytes_exclusive(
            artifact_root / "source-transitions.ndjson",
            b"".join(canonical_json_bytes(row) + b"\n" for row in transitions),
        )
        write_bytes_exclusive(
            artifact_root / "poweroff.log",
            b"".join(canonical_json_bytes(row) + b"\n" for row in poweroff_rows),
        )
        capture_commitments = {
            field: sha256_file(artifact_root / filename)
            for field, filename in ARTIFACT_FILENAMES.items()
        }
        if len(set(capture_commitments.values())) != len(capture_commitments):
            raise AdapterError("physical artifact identities were unexpectedly reused")

        record: dict[str, object] = {
            "schema_version": 1,
            "case_id": "BSC-16",
            "role": args.role,
            "session_id": args.session_id,
            "attempt_id": args.attempt_id,
            "target_sha": args.target_sha,
            "dut_alias": args.dut_alias,
            "rig_alias": args.rig_alias,
            "execution_mode": "physical",
            "hardware_observed": True,
            "started_at_utc": started_at,
            "completed_at_utc": utc_now(),
            "firmware": {
                "environment": environment,
                "target_sha": args.target_sha,
                "binary_sha256": binary_sha,
                "hil_fault_control_active": args.role == "fault-collection",
            },
            "stimuli": stimuli,
            "facts": facts,
            "fault_lifecycle": lifecycle,
            "capture_commitments": capture_commitments,
        }
        record["evidence_binding_sha256"] = commitment(record)
        sys.stdout.buffer.write(canonical_json_bytes(record) + b"\n")
        return 0
    except AdapterError as exc:
        print(
            f"BSC-16 adapter failed closed: {exc}. No qualifying record was emitted.",
            file=sys.stderr,
        )
        return 2
    except (EOFError, KeyboardInterrupt):
        print("BSC-16 adapter failed closed; no qualifying record was emitted.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
