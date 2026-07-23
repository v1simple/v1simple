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
from typing import Mapping, Sequence

import serial


ROOT = Path(__file__).resolve().parents[1]
FULL_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
OPAQUE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
BOOT_CLASSIFICATION_RE = re.compile(
    r"\[Battery\] Power detection: classification=(battery|usb|unknown) "
    r"reported=(battery|usb|unknown)",
    re.IGNORECASE,
)
CHANGE_CLASSIFICATION_RE = re.compile(
    r"\[Battery\] Power source changed: (battery|usb|unknown)",
    re.IGNORECASE,
)
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


class AdapterError(RuntimeError):
    """Fail-closed physical adapter error safe to summarize."""


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


def prompt_ready(instruction: str) -> None:
    print(f"\nBSC-16 PHYSICAL CHECKPOINT: {instruction}", file=sys.stderr, flush=True)
    print("Type READY when the rig is prepared; the timed capture starts immediately.", file=sys.stderr, flush=True)
    if input().strip() != "READY":
        raise AdapterError("operator checkpoint was not acknowledged exactly")
    print("ACTION NOW", file=sys.stderr, flush=True)


def prompt_pass(question: str) -> None:
    print(question, file=sys.stderr, flush=True)
    print("Type PASS only for a directly observed pass.", file=sys.stderr, flush=True)
    if input().strip() != "PASS":
        raise AdapterError("physical observation did not pass")


def capture_serial(
    serial_port: str,
    *,
    duration_seconds: float,
    run_started: float,
    stimulus_id: str,
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    deadline = time.monotonic() + duration_seconds
    opened = False
    while time.monotonic() < deadline:
        try:
            handle = serial.Serial(serial_port, 115200, timeout=0.2)
        except (OSError, serial.SerialException):
            time.sleep(0.2)
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
        if time.monotonic() < deadline:
            time.sleep(0.2)
    if not opened:
        raise AdapterError("serial endpoint could not be opened for physical capture")
    return rows


def send_hil_command(
    serial_port: str,
    command: str,
    *,
    run_started: float,
    serial_rows: list[dict[str, object]],
) -> None:
    deadline = time.monotonic() + 8.0
    try:
        handle = serial.Serial(serial_port, 115200, timeout=0.2)
    except (OSError, serial.SerialException) as exc:
        raise AdapterError("HIL serial endpoint could not be opened") from exc
    accepted = False
    with handle:
        handle.reset_input_buffer()
        handle.write(command.encode("ascii") + b"\n")
        handle.flush()
        while time.monotonic() < deadline:
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
                accepted = True
                break
    if not accepted:
        raise AdapterError("HIL serial command was not positively acknowledged")


def run_firmware_install(environment: str, serial_port: str, target_sha: str) -> tuple[str, dict[str, object]]:
    commands = (
        ["pio", "run", "-e", environment],
        ["pio", "run", "-e", environment, "-t", "upload", "--upload-port", serial_port],
    )
    command_evidence: list[dict[str, object]] = []
    for command in commands:
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
        value = (
            boot_match.group(1)
            if boot_match is not None
            else change_match.group(1)
            if change_match is not None
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
        raise AdapterError("required source classification was not captured")
    return observed


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


def import_logic_capture(destination: Path) -> int:
    print(
        "Export the representative GPIO16 digital capture as CSV with a time column "
        "(seconds or milliseconds) and a gpio16 column, then type its local path.",
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
    instruction: str,
    stimulus_id: str,
    duration_seconds: float,
    run_started: float,
    serial_port: str,
    serial_rows: list[dict[str, object]],
    stimuli: list[dict[str, object]],
) -> None:
    prompt_ready(instruction)
    started_ms = int((time.monotonic() - run_started) * 1000)
    serial_rows.extend(
        capture_serial(
            serial_port,
            duration_seconds=duration_seconds,
            run_started=run_started,
            stimulus_id=stimulus_id,
        )
    )
    prompt_pass(f"Confirm the directly observed result for {stimulus_id}.")
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
        print(
            f"\nStarting BSC-16 {args.role}. Keep USB VBUS isolated for every true battery cut.",
            file=sys.stderr,
            flush=True,
        )
        binary_sha, build_evidence = run_firmware_install(
            environment, args.serial_port, args.target_sha
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
            instruction="Set battery-only power with USB VBUS isolated, arm the analyzer, and wake with PWR.",
            stimulus_id="pwr-wake-on-battery",
            duration_seconds=7.0,
            run_started=run_started,
            serial_port=args.serial_port,
            serial_rows=serial_rows,
            stimuli=stimuli,
        )
        perform_stimulus(
            instruction="Remove all power, configure USB cold boot, and connect USB on ACTION NOW.",
            stimulus_id="usb-cold-boot",
            duration_seconds=7.0,
            run_started=run_started,
            serial_port=args.serial_port,
            serial_rows=serial_rows,
            stimuli=stimuli,
        )

        if args.role == "fault-collection":
            session_hash = hashlib.sha256(args.session_id.encode("ascii")).hexdigest()
            for command in (
                f"V1HIL BEGIN BSC-16 {session_hash} 60000",
                f"V1HIL ARM BSC-16 battery-adc-init-fail-once {session_hash} 7",
                f"V1HIL NEXT_BOOT BSC-16 battery-adc-init-fail-once {session_hash} 7",
            ):
                send_hil_command(
                    args.serial_port,
                    command,
                    run_started=run_started,
                    serial_rows=serial_rows,
                )
            perform_stimulus(
                instruction=(
                    "With the one-shot ADC fault staged, switch to battery-only, power-cycle, "
                    "and wake with PWR on ACTION NOW."
                ),
                stimulus_id="force-adc-init-failure",
                duration_seconds=9.0,
                run_started=run_started,
                serial_port=args.serial_port,
                serial_rows=serial_rows,
                stimuli=stimuli,
            )

        perform_stimulus(
            instruction="On battery-only power, hold PWR for at least two seconds on ACTION NOW.",
            stimulus_id="hold-power-button",
            duration_seconds=7.0,
            run_started=run_started,
            serial_port=args.serial_port,
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
            instruction="Boot on battery, then connect isolated USB source on ACTION NOW.",
            stimulus_id="transition-battery-to-usb",
            duration_seconds=7.0,
            run_started=run_started,
            serial_port=args.serial_port,
            serial_rows=serial_rows,
            stimuli=stimuli,
        )
        perform_stimulus(
            instruction="While running on USB, disconnect the USB source to battery on ACTION NOW.",
            stimulus_id="transition-usb-to-battery",
            duration_seconds=7.0,
            run_started=run_started,
            serial_port=args.serial_port,
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
        usb = require_classification(serial_rows, "usb-cold-boot", "usb")
        require_classification(serial_rows, "transition-battery-to-usb", "usb")
        require_classification(serial_rows, "transition-usb-to-battery", "battery")
        flapped = source_flapped(
            serial_rows,
            ("transition-battery-to-usb", "transition-usb-to-battery"),
        )
        if flapped:
            raise AdapterError("source-classification flapping was captured")
        usb_start = next(
            row["elapsed_ms"] for row in stimuli if row["id"] == "usb-cold-boot"
        )
        usb_delay = next(elapsed for elapsed, value in usb if value == "usb") - usb_start
        if not 2800 <= usb_delay <= 4000:
            raise AdapterError("USB confirmation delay is outside the qualified window")

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
                "usb-confirmation-delay-ms": usb_delay,
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
                "usb-classification-correct": True,
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
    except (AdapterError, EOFError, KeyboardInterrupt):
        print("BSC-16 adapter failed closed; no qualifying record was emitted.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
