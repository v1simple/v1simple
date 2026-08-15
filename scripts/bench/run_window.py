#!/usr/bin/env python3
"""Collect one SD-backed bench window over USB serial.

This is intentionally only a collector/importer. It starts one firmware bench
window (core, display, or replay), downloads the SD perf CSV, and also downloads
the encounter CSV for replay. It imports the perf data with the shared CSV
importer and writes a small window_result.json. It does not decide release
verdicts, apply OBD/proxy coverage, or promote baselines; optional baseline
manifests are passed through only for importer comparison output.
"""

from __future__ import annotations

import argparse
import binascii
import glob
import json
import math
import os
import secrets
import signal
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

from bench_identity import (
    baseline_directory,
    build_identity_manifest,
    load_identity_manifest,
    write_identity_manifest,
)
from camera_artifacts import (
    build_capture_manifest,
    camera_result_view,
    publish_capture_manifest,
    publish_grade,
    replay_timing_anchor,
)
from camera_capture import CameraCapture
from camera_contract import camera_grade_required as contract_grade_required
from camera_grade import grade_camera
from camera_preflight import run_camera_preflight

try:  # pyserial is needed only for live collection, not --from-csv imports.
    import serial  # type: ignore
except ImportError:  # pragma: no cover - exercised only on hosts without pyserial
    serial = None  # type: ignore

ROOT = Path(__file__).resolve().parents[2]
IMPORT_PERF_CSV = ROOT / "tools" / "import_perf_csv.py"
BUILD_SH = ROOT / "build.sh"
RUN_PROGRESS_INTERVAL_S = 15
QGETCSV_BUSY_RETRY_TIMEOUT_S = 15.0
QGETCSV_BUSY_RETRY_DELAY_S = 0.25
HANDSHAKE_LEDGER_NAME = "handshake_ledger.jsonl"
RECONNECT_LEDGER_NAME = "handshake_ledger_preflight.jsonl"
RECONNECT_LOG_NAME = "v1replay_reconnect_preflight.log"
V1_DISCONNECT_CLEANUP_PREFIX = "[BLE] V1 disconnected; cleared LCD BLE state at "
BOOT_PREFIX = "BOOT bootId="
RECONNECT_PREFLIGHT_START = "reconnect_preflight_start"
RECONNECT_FENCE_BEGIN = "reconnect_preflight_fence_begin"
RECONNECT_FENCE_COMPLETE = "reconnect_preflight_fence_complete"
RECONNECT_POST_CLEANUP_FENCE_BEGIN = "reconnect_post_cleanup_fence_begin"
RECONNECT_POST_CLEANUP_FENCE_COMPLETE = "reconnect_post_cleanup_fence_complete"
RECONNECT_PRE_QSTART_FENCE_BEGIN = "reconnect_pre_qstart_fence_begin"
RECONNECT_PRE_QSTART_FENCE_COMPLETE = "reconnect_pre_qstart_fence_complete"
HANDSHAKE_LEDGER_SCHEMA = 2
HANDSHAKE_LEDGER_TIMEBASE = "epoch_monotonic_ms"
MAX_HANDSHAKE_EPOCHS = 4
MAX_HANDSHAKE_EVENTS_PER_EPOCH = 12
# Six non-start events plus five accepted starts use 11 slots, so the existing
# cap still records a violating sixth start as the twelfth event.
MAX_HANDSHAKE_START_REQUESTS = 5
MIN_HANDSHAKE_START_RETRY_MS = 1000
MAX_HANDSHAKE_ELAPSED_MS = 0xFFFFFFFF

START_ALERT_REQUEST = [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB]
VERSION_REQUEST = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB]
VERSION_RESPONSE = [
    0xAA, 0xD6, 0xEA, 0x02, 0x08,
    0x76, 0x34, 0x2E, 0x31, 0x30, 0x33, 0x38,
    0x18, 0xAB,
]
ALL_VOLUME_REQUEST = [0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB]
ALL_VOLUME_RESPONSE = [
    0xAA, 0xD6, 0xEA, 0x3D, 0x05,
    0x04, 0x00, 0x04, 0x00,
    0xB4, 0xAB,
]
EMPTY_ALERT_ROW = [
    0xAA, 0xD8, 0xEA, 0x43, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB7, 0xAB,
]


class CameraPreflightFailure(RuntimeError):
    """Camera admission failed before the product collection path began."""

    def __init__(self, preflight: dict[str, Any], camera_result: dict[str, Any]) -> None:
        diagnostics = preflight.get("diagnostics") if isinstance(preflight.get("diagnostics"), list) else []
        diagnostic = diagnostics[0] if diagnostics and isinstance(diagnostics[0], dict) else {}
        super().__init__(
            str(diagnostic.get("message") or diagnostic.get("code") or "camera preflight failed")
        )
        self.preflight = preflight
        self.camera_result = camera_result
        self.reconnect_preflight: dict[str, Any] = {}


class ReconnectPreflightFailure(RuntimeError):
    """Managed reconnect could not establish a safe boundary before QSTART."""

    def __init__(
        self,
        message: str,
        result: dict[str, Any],
        *,
        classification: str,
        failure_kind: str,
    ) -> None:
        super().__init__(message)
        self.result = result
        self.classification = classification
        self.failure_kind = failure_kind


class ReconnectBehaviorError(RuntimeError):
    """Healthy evidence shows that the required reconnect transition failed."""

    def __init__(self, kind: str, message: str) -> None:
        super().__init__(message)
        self.kind = kind


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["core", "display", "replay"], required=True)
    parser.add_argument("--duration-seconds", type=int, default=300)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--port", default=os.environ.get("DEVICE_PORT", ""))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--board-id", default=os.environ.get("BENCH_BOARD_ID", "release"))
    parser.add_argument("--git-sha", default="")
    parser.add_argument("--git-ref", default="")
    parser.add_argument("--git-worktree-clean", choices=["0", "1"], default="0")
    parser.add_argument("--identity-manifest", required=True)
    parser.add_argument(
        "--baseline-root",
        default="",
        help="Optional identity-keyed baseline root; legacy board/suite baselines are ignored",
    )
    parser.add_argument("--profile", default="drive_wifi_off")
    parser.add_argument("--segment", default="last")
    parser.add_argument(
        "--compare-to",
        action="append",
        default=[],
        help="Optional baseline manifest.json passed through to the CSV importer",
    )
    parser.add_argument("--lane", default="bench")
    parser.add_argument("--upload", action="store_true", help="Build/upload production firmware+filesystem first")
    parser.add_argument("--skip-web", action="store_true", help="Pass --skip-web to build.sh when uploading")
    parser.add_argument(
        "--post-upload-settle-seconds",
        type=int,
        default=90,
        help="Unscored SD settle interval after upload and before the scored QSTART window",
    )
    parser.add_argument("--from-csv", default="", help="Import an existing perf CSV instead of collecting live")
    parser.add_argument(
        "--replay-executable",
        default="",
        help="v1replay executable used as the V1 emulator after QSTART acknowledgement",
    )
    blink_group = parser.add_mutually_exclusive_group()
    blink_group.add_argument(
        "--blink-profile",
        choices=["scenario", "steady", "stress"],
        default=None,
        help="Priority-arrow blink profile for replay (default: scenario)",
    )
    blink_group.add_argument(
        "--blink-arrow",
        action="store_true",
        help="Legacy alias for --blink-profile stress",
    )
    parser.add_argument("--camera", action="store_true", help="Capture calibrated camera evidence for this window")
    parser.add_argument("--ready-timeout-seconds", type=int, default=45)
    parser.add_argument("--completion-grace-seconds", type=int, default=45)
    parser.add_argument("--export-idle-timeout-seconds", type=int, default=30)
    parser.add_argument("--export-retries", type=int, default=2)
    parser.add_argument("--export-recovery-idle-timeout-seconds", type=int, default=120)
    return parser.parse_args()


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def write_window_result(out_dir: Path, payload: dict[str, Any]) -> None:
    payload.setdefault("schema_version", 1)
    payload.setdefault("timestamp_utc", utc_now())
    (out_dir / "window_result.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def install_signal_handlers() -> None:
    def interrupt(signum: int, _frame: Any) -> None:
        raise InterruptedError(f"received signal {signum}")

    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, interrupt)


def detect_port() -> str:
    patterns = [
        "/dev/cu.usbmodem*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
        "/dev/cu.usbserial*",
        "/dev/tty.usbserial*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/tty.SLAB_USBtoUART*",
    ]
    candidates: list[str] = []
    for pattern in patterns:
        candidates.extend(glob.glob(pattern))
    candidates = sorted(dict.fromkeys(candidates))
    return candidates[0] if candidates else ""


def wait_for_port(preferred: str, timeout_s: int = 30) -> str:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if preferred and Path(preferred).exists():
            return preferred
        detected = detect_port()
        if detected:
            return detected
        time.sleep(1)
    raise RuntimeError("No USB serial device detected")


def run_upload(port: str, skip_web: bool) -> None:
    cmd = [str(BUILD_SH), "-f", "-u"]
    if skip_web:
        cmd.append("--skip-web")
    if port:
        cmd.extend(["--upload-port", port])
    subprocess.run(cmd, cwd=ROOT, check=True)


def wait_for_post_upload_settle(
    seconds: int,
    *,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    """Wait in short, signal-interruptible steps before scored collection."""
    if seconds <= 0:
        return
    print(
        f"[bench] allowing {seconds}s for post-upload SD activity to settle before scored collection",
        flush=True,
    )
    remaining = float(seconds)
    while remaining > 0:
        interval = min(1.0, remaining)
        sleep(interval)
        remaining -= interval
    print("[bench] post-upload SD settle complete", flush=True)


class BenchSerial:
    def __init__(self, port: str, baud: int, log_path: Path):
        if serial is None:
            raise RuntimeError("pyserial is required for live bench collection")
        self.port = port
        self.baud = baud
        self.log_path = log_path
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log = self.log_path.open("a", encoding="utf-8")
        self.ser = serial.Serial()  # type: ignore[union-attr]
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.25
        self.ser.write_timeout = 2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.reset_input_buffer()
        self.boot_marker_count = 0
        self.disconnect_cleanup_count = 0

    def close(self) -> None:
        try:
            if self.ser.is_open:
                self.ser.close()
        finally:
            self.log.close()

    def write_command(self, command: str) -> None:
        line = command.rstrip("\r\n") + "\n"
        self.log.write(f">>> {line}")
        self.log.flush()
        self.ser.write(line.encode("utf-8"))
        self.ser.flush()

    def read_line(self, timeout_s: float) -> str:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            self.log.write(text + "\n")
            self.log.flush()
            if text.startswith(BOOT_PREFIX):
                self.boot_marker_count += 1
            if text.startswith(V1_DISCONNECT_CLEANUP_PREFIX):
                self.disconnect_cleanup_count += 1
            return text
        raise TimeoutError("serial read timed out")

    def record_host_boundary(self, label: str) -> None:
        self.log.write(f"HOST_BOUNDARY {label}\n")
        self.log.flush()

    def read_protocol_line(self, prefixes: tuple[str, ...], timeout_s: float) -> str:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            try:
                line = self.read_line(min(0.5, remaining))
            except TimeoutError:
                continue
            if line.startswith(prefixes):
                return line
        raise TimeoutError(f"timed out waiting for {prefixes}")


def parse_json_line(line: str, prefix: str) -> dict[str, Any]:
    if not line.startswith(prefix):
        raise RuntimeError(f"expected {prefix!r}, got: {line}")
    return json.loads(line[len(prefix):])


def wait_ready(q: BenchSerial, timeout_s: int) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last_error = ""
    while time.monotonic() < deadline:
        q.write_command("QSTATUS")
        try:
            line = q.read_protocol_line(("QRESP ", "QERR "), 2)
        except TimeoutError as exc:
            last_error = str(exc)
            time.sleep(1)
            continue
        if line.startswith("QRESP "):
            return parse_json_line(line, "QRESP ")
        last_error = str(parse_json_line(line, "QERR "))
        time.sleep(1)
    raise RuntimeError(f"bench serial protocol did not become ready: {last_error}")


def establish_serial_fence(q: BenchSerial, timeout_s: float = 5.0) -> dict[str, Any]:
    """Round-trip QSTATUS so every earlier serial line has crossed the host boundary."""
    q.write_command("QSTATUS")
    line = q.read_protocol_line(("QRESP ", "QERR "), timeout_s)
    if not line.startswith("QRESP "):
        raise RuntimeError(f"reconnect serial fence failed: {parse_json_line(line, 'QERR ')}")
    payload = parse_json_line(line, "QRESP ")
    if not (
        payload.get("ok") is True
        and payload.get("state") in {"idle", "done"}
        and payload.get("suite") in {"core", "display"}
        and payload.get("mode") in {"current", "proxy", "obd", "v1"}
    ):
        raise RuntimeError(f"reconnect serial fence was not ready: {payload}")
    return payload


def establish_reconnect_readiness(
    q: BenchSerial,
    timeout_s: float,
    *,
    nonce: str | None = None,
) -> dict[str, Any]:
    """Order startup replies behind a unique FIFO barrier, then fence QSTATUS."""
    barrier_nonce = nonce if nonce is not None else secrets.token_hex(16)
    if len(barrier_nonce) != 32 or any(
        character not in "0123456789abcdef" for character in barrier_nonce
    ):
        raise RuntimeError("reconnect readiness barrier generated an invalid nonce")

    deadline = time.monotonic() + timeout_s
    q.write_command(f"QBSC08 {barrier_nonce}")
    while time.monotonic() < deadline:
        remaining = max(0.1, deadline - time.monotonic())
        try:
            line = q.read_line(min(0.5, remaining))
        except TimeoutError:
            continue
        if line.startswith("QRESP "):
            # A readiness response delayed by boot crossed before the nonce;
            # consuming it here prevents it from satisfying the final fence.
            try:
                delayed_response = json.loads(line[len("QRESP ") :])
            except json.JSONDecodeError as exc:
                raise RuntimeError(
                    "reconnect readiness barrier encountered malformed delayed QRESP"
                ) from exc
            if not isinstance(delayed_response, dict):
                raise RuntimeError(
                    "reconnect readiness barrier encountered malformed delayed QRESP"
                )
            continue
        if line.startswith("QERR "):
            raise RuntimeError("reconnect readiness barrier received QERR")
        if not line.startswith("QBSC08 "):
            continue
        try:
            response = json.loads(line[len("QBSC08 ") :])
        except json.JSONDecodeError as exc:
            raise RuntimeError("reconnect readiness barrier response is malformed") from exc
        if not isinstance(response, dict):
            raise RuntimeError("reconnect readiness barrier response is malformed")
        schema = response.get("schema")
        if (
            not isinstance(schema, int)
            or isinstance(schema, bool)
            or schema != 1
            or response.get("nonce") != barrier_nonce
        ):
            raise RuntimeError("reconnect readiness barrier response has the wrong nonce")
        if response.get("status") not in {"ready", "busy"}:
            raise RuntimeError("reconnect readiness barrier response has an invalid status")
        return establish_serial_fence(
            q,
            timeout_s=max(0.1, deadline - time.monotonic()),
        )
    raise RuntimeError("reconnect readiness barrier timed out")


def _preflight_ledger_is_complete(path: Path) -> bool:
    """Return readiness from one bounded, delivery-confirmed schema-v2 epoch.

    A valid but incomplete ledger returns False so collection can continue.
    Irreversible protocol contradictions raise ``handshake_invalid``. Evidence
    schema/type failures remain collection errors. The grader independently
    decodes the final ledger after the emulator exits.
    """
    try:
        if path.stat().st_size > 8 * 1024:
            raise RuntimeError("reconnect preflight ledger exceeds its bounded size")
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return False
    except (OSError, UnicodeError) as exc:
        raise RuntimeError(f"reconnect preflight ledger could not be read: {exc}") from exc
    if text and not text.endswith("\n"):
        return False
    lines = text.splitlines()
    if not lines:
        return False
    if len(lines) > 1 + MAX_HANDSHAKE_EPOCHS * MAX_HANDSHAKE_EVENTS_PER_EPOCH:
        raise RuntimeError("reconnect preflight ledger exceeds its bounded event count")
    try:
        records = [json.loads(line) for line in lines]
    except json.JSONDecodeError as exc:
        raise RuntimeError("reconnect preflight ledger contains invalid JSON") from exc
    if records[0] != {
        "schema_version": HANDSHAKE_LEDGER_SCHEMA,
        "kind": "v1replay_handshake_ledger",
        "timebase": HANDSHAKE_LEDGER_TIMEBASE,
    }:
        raise RuntimeError("reconnect preflight ledger has an invalid header")
    events = records[1:]
    event_keys = {
        "subscribe": {"event", "epoch", "channel", "elapsed_ms"},
        "request": {"event", "epoch", "channel", "bytes", "elapsed_ms"},
        "response": {"event", "epoch", "channel", "bytes", "delivery", "elapsed_ms"},
        "stream_started": {
            "event", "epoch", "channel", "bytes", "delivery", "elapsed_ms",
        },
    }
    previous_elapsed_ms = -1
    for event in events:
        if not isinstance(event, dict):
            raise RuntimeError("reconnect preflight ledger event is not an object")
        event_name = event.get("event")
        if (
            not isinstance(event_name, str)
            or event_name not in event_keys
            or set(event) != event_keys[event_name]
        ):
            raise RuntimeError("reconnect preflight ledger has an invalid event schema")
        epoch = event.get("epoch")
        if (
            not isinstance(epoch, int)
            or isinstance(epoch, bool)
            or not 1 <= epoch <= 4
        ):
            raise RuntimeError("reconnect preflight ledger has an invalid anonymous epoch")
        if epoch != 1:
            raise ReconnectBehaviorError(
                "handshake_invalid",
                "reconnect preflight ledger crosses its one anonymous epoch",
            )
        if not isinstance(event.get("channel"), str):
            raise RuntimeError("reconnect preflight ledger has an invalid channel")
        elapsed_ms = event.get("elapsed_ms")
        if (
            not isinstance(elapsed_ms, int)
            or isinstance(elapsed_ms, bool)
            or not 0 <= elapsed_ms <= MAX_HANDSHAKE_ELAPSED_MS
        ):
            raise RuntimeError("reconnect preflight ledger has invalid relative timing evidence")
        if elapsed_ms < previous_elapsed_ms:
            raise RuntimeError("reconnect preflight ledger relative timing is not monotonic")
        if event_name == "subscribe" and elapsed_ms != 0:
            raise RuntimeError(
                "reconnect preflight ledger subscription timing does not begin at zero"
            )
        previous_elapsed_ms = elapsed_ms
        if event_name != "subscribe":
            raw = event.get("bytes")
            if (
                not isinstance(raw, list)
                or len(raw) > 64
                or any(
                    not isinstance(value, int)
                    or isinstance(value, bool)
                    or not 0 <= value <= 0xFF
                    for value in raw
                )
            ):
                raise RuntimeError("reconnect preflight ledger has invalid packet bytes")
        if event_name in {"response", "stream_started"} and not isinstance(
            event.get("delivery"), str
        ):
            raise RuntimeError("reconnect preflight ledger has invalid delivery evidence")
    if len(events) > MAX_HANDSHAKE_EVENTS_PER_EPOCH:
        raise RuntimeError("reconnect preflight ledger epoch exceeds its bounded event count")

    seen = {
        "subscribe": False,
        "stream": False,
        "version_request": False,
        "version_response": False,
        "all_volume_request": False,
        "all_volume_response": False,
    }
    selected_request_channel: str | None = None
    start_elapsed_ms: list[int] = []
    for index, event in enumerate(events):
        event_name = event.get("event")
        channel = event.get("channel")
        raw = event.get("bytes")
        if event_name == "subscribe":
            if index != 0 or seen["subscribe"] or channel != "B2CE":
                raise ReconnectBehaviorError(
                    "handshake_invalid",
                    "reconnect preflight ledger has an invalid B2CE subscription boundary",
                )
            seen["subscribe"] = True
            continue

        if not seen["subscribe"]:
            raise ReconnectBehaviorError(
                "handshake_invalid",
                "reconnect preflight handshake traffic occurred before B2CE subscription",
            )

        key = ""
        if event_name == "request" and channel in {"B6D4", "BAD4"}:
            if selected_request_channel is None:
                selected_request_channel = str(channel)
            elif channel != selected_request_channel:
                raise ReconnectBehaviorError(
                    "handshake_invalid",
                    "reconnect preflight ledger switches its selected command channel",
                )
            if raw == START_ALERT_REQUEST:
                if seen["stream"]:
                    raise ReconnectBehaviorError(
                        "handshake_invalid",
                        "reconnect preflight repeats start after stream delivery",
                    )
                start_elapsed_ms.append(event["elapsed_ms"])
                if len(start_elapsed_ms) > MAX_HANDSHAKE_START_REQUESTS:
                    raise ReconnectBehaviorError(
                        "handshake_invalid",
                        "reconnect preflight exceeds its bounded pre-stream start retries",
                    )
                if (
                    len(start_elapsed_ms) > 1
                    and start_elapsed_ms[-1] - start_elapsed_ms[-2]
                    < MIN_HANDSHAKE_START_RETRY_MS
                ):
                    raise ReconnectBehaviorError(
                        "handshake_invalid",
                        "reconnect preflight start retry arrived before the 1000 ms recovery interval",
                    )
                continue
            if raw == VERSION_REQUEST:
                key = "version_request"
                if not start_elapsed_ms:
                    raise ReconnectBehaviorError(
                        "handshake_invalid",
                        "reconnect preflight version request occurred before start",
                    )
            elif raw == ALL_VOLUME_REQUEST:
                key = "all_volume_request"
                if not seen["version_request"]:
                    raise ReconnectBehaviorError(
                        "handshake_invalid",
                        "reconnect preflight all-volume request occurred before version request",
                    )
            else:
                raise ReconnectBehaviorError(
                    "handshake_invalid",
                    "reconnect preflight ledger contains a non-canonical request",
                )
        elif (
            event_name == "response"
            and channel == "B2CE"
            and event.get("delivery") == "delivered"
        ):
            if raw == VERSION_RESPONSE:
                key = "version_response"
                if not seen["version_request"]:
                    raise ReconnectBehaviorError(
                        "handshake_invalid",
                        "reconnect preflight version response occurred before its request",
                    )
            elif raw == ALL_VOLUME_RESPONSE:
                key = "all_volume_response"
                if not seen["all_volume_request"]:
                    raise ReconnectBehaviorError(
                        "handshake_invalid",
                        "reconnect preflight all-volume response occurred before its request",
                    )
        elif (
            event_name == "stream_started"
            and channel == "B2CE"
            and event.get("delivery") == "delivered"
            and raw == EMPTY_ALERT_ROW
        ):
            key = "stream"
            if not start_elapsed_ms:
                raise ReconnectBehaviorError(
                    "handshake_invalid",
                    "reconnect preflight stream began before start",
                )
        if not key:
            raise ReconnectBehaviorError(
                "handshake_invalid",
                "reconnect preflight ledger contains non-canonical delivery evidence",
            )
        if seen[key]:
            raise ReconnectBehaviorError(
                "handshake_invalid",
                f"reconnect preflight repeats {key.replace('_', ' ')}",
            )
        seen[key] = True

    return bool(
        start_elapsed_ms
        and selected_request_channel is not None
        and all(seen.values())
    )


def start_and_wait(
    q: BenchSerial,
    suite: str,
    duration_s: int,
    grace_s: int,
    after_started: Callable[[], None] | None = None,
    health_check: Callable[[], str] | None = None,
) -> dict[str, Any]:
    start_deadline = time.monotonic() + 15
    last_start_error: dict[str, Any] | None = None
    start_payload: dict[str, Any] | None = None
    firmware_suite = "core" if suite == "replay" else suite
    command = f"QSTART {firmware_suite} {duration_s}"
    while time.monotonic() < start_deadline:
        q.write_command(command)
        attempt_deadline = min(start_deadline, time.monotonic() + 5)
        retry_start = False
        while time.monotonic() < attempt_deadline:
            remaining = max(0.1, attempt_deadline - time.monotonic())
            try:
                line = q.read_protocol_line(("QRESP ", "QERR "), remaining)
            except TimeoutError as exc:
                last_start_error = {"timeout": str(exc)}
                break
            if line.startswith("QRESP "):
                payload = parse_json_line(line, "QRESP ")
                if (
                    payload.get("ok")
                    and payload.get("state") == "running"
                    and payload.get("suite") == firmware_suite
                ):
                    start_payload = payload
                    break
                last_start_error = {"stale_response": payload}
                continue
            last_start_error = parse_json_line(line, "QERR ")
            retry_reason = str(last_start_error.get("error") or last_start_error.get("message") or "")
            if retry_reason == "perf_sd_busy_retry":
                retry_start = True
                break
            raise RuntimeError(f"QSTART failed: {last_start_error}")
        if start_payload is not None:
            break
        if retry_start:
            time.sleep(0.25)
            continue
    if start_payload is None:
        raise RuntimeError(f"QSTART did not produce a running acknowledgement: {last_start_error}")

    print(
        f"[bench] started suite={suite} duration={duration_s}s csv={start_payload.get('csvPath') or 'unknown'}; "
        "metrics are recording to SD",
        flush=True,
    )
    if after_started is not None:
        after_started()

    deadline = time.monotonic() + duration_s + grace_s
    run_started = time.monotonic()
    next_progress = run_started + RUN_PROGRESS_INTERVAL_S
    last_event: dict[str, Any] = start_payload
    while time.monotonic() < deadline:
        if health_check is not None:
            problem = health_check()
            if problem:
                try:
                    q.write_command("QABORT")
                except Exception:  # noqa: BLE001 - retain the original companion failure
                    pass
                raise RuntimeError(problem)
        try:
            line = q.read_protocol_line(("QEVENT ", "QERR "), 1)
        except TimeoutError:
            now = time.monotonic()
            if now >= next_progress:
                elapsed_s = min(duration_s, int(now - run_started))
                print(f"[bench] running suite={suite}: {elapsed_s}/{duration_s}s elapsed", flush=True)
                next_progress = now + RUN_PROGRESS_INTERVAL_S
            continue
        prefix = "QEVENT " if line.startswith("QEVENT ") else "QERR "
        payload = parse_json_line(line, prefix)
        last_event = payload
        if payload.get("state") in {"done", "error"}:
            if not payload.get("ok"):
                raise RuntimeError(f"bench window failed: {payload}")
            print(f"[bench] firmware completed suite={suite}: {payload}", flush=True)
            return payload
    raise RuntimeError(f"bench window timed out waiting for completion; last={last_event}")


def download_csv(q: BenchSerial, out_dir: Path, idle_timeout_s: int, sd_path: str = "") -> Path:
    command = "QGETCSV"
    if sd_path:
        command += f" {sd_path}"
    retry_deadline = time.monotonic() + QGETCSV_BUSY_RETRY_TIMEOUT_S
    last_busy_error: dict[str, Any] | None = None
    while True:
        remaining = retry_deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError(
                "QGETCSV failed after retrying perf_sd_busy_retry for "
                f"{QGETCSV_BUSY_RETRY_TIMEOUT_S:g}s: {last_busy_error}"
            )
        q.write_command(command)
        line = q.read_protocol_line(("QFILE ", "QERR "), min(10.0, remaining))
        if not line.startswith("QERR "):
            break
        error = parse_json_line(line, "QERR ")
        if error.get("error") != "perf_sd_busy_retry":
            raise RuntimeError(f"QGETCSV failed: {line}")
        last_busy_error = error
        remaining = retry_deadline - time.monotonic()
        if remaining <= 0:
            continue
        time.sleep(min(QGETCSV_BUSY_RETRY_DELAY_S, remaining))
    header = parse_json_line(line, "QFILE ")
    path = str(header.get("path") or "perf.csv")
    expected_size = int(header.get("size") or 0)
    basename = Path(path).name or "perf.csv"
    csv_path = out_dir / basename
    print(f"[bench] exporting SD CSV path={path} size={expected_size} bytes", flush=True)

    payload = bytearray()
    expected_seq = 0
    while True:
        line = q.read_protocol_line(("QCHUNK ", "QEND ", "QERR "), idle_timeout_s)
        if line.startswith("QERR "):
            raise RuntimeError(f"CSV export failed: {line}")
        if line.startswith("QEND "):
            end = parse_json_line(line, "QEND ")
            if int(end.get("bytes") or 0) != len(payload):
                raise RuntimeError(f"CSV byte count mismatch: firmware={end.get('bytes')} host={len(payload)}")
            reported_crc = str(end.get("crc32") or "").upper()
            host_crc = f"{binascii.crc32(payload) & 0xFFFFFFFF:08X}"
            if reported_crc and reported_crc != host_crc:
                raise RuntimeError(f"CSV CRC mismatch: firmware={reported_crc} host={host_crc}")
            break
        _prefix, seq_text, hex_text = line.split(" ", 2)
        seq = int(seq_text)
        if seq != expected_seq:
            raise RuntimeError(f"CSV chunk sequence mismatch: expected {expected_seq}, got {seq}")
        payload.extend(bytes.fromhex(hex_text.strip()))
        expected_seq += 1

    if expected_size and expected_size != len(payload):
        raise RuntimeError(f"CSV size mismatch: header={expected_size} downloaded={len(payload)}")
    csv_path.write_bytes(payload)
    print(f"[bench] downloaded CSV to {csv_path} ({len(payload)} bytes)", flush=True)
    return csv_path


def encounter_csv_sd_path(perf_csv_sd_path: str) -> str:
    prefix = "/perf/perf_boot_"
    if not perf_csv_sd_path.startswith(prefix) or not perf_csv_sd_path.endswith(".csv"):
        return ""
    boot_suffix = perf_csv_sd_path[len(prefix) :]
    boot_identity = boot_suffix[: -len(".csv")]
    if not boot_identity or "/" in boot_identity or ".." in boot_identity:
        return ""
    return f"/encounters/encounters_{boot_suffix}"


def run_import(
    args: argparse.Namespace,
    csv_path: Path,
    out_dir: Path,
    identity: dict[str, Any],
) -> subprocess.CompletedProcess[str]:
    stress_class = "core" if args.suite in {"core", "replay"} else "display_preview"
    cmd = [
        sys.executable,
        str(IMPORT_PERF_CSV),
        "--input",
        str(csv_path),
        "--out-dir",
        str(out_dir),
        "--board-id",
        args.board_id,
        "--git-sha",
        args.git_sha,
        "--git-ref",
        args.git_ref,
        "--product-fingerprint",
        str(identity["product_fingerprint"]),
        "--grader-fingerprint",
        str(identity["grader_fingerprint"]),
        "--scenario-fingerprint",
        str(identity["scenario_fingerprint"]),
        "--profile",
        args.profile,
        "--segment",
        args.segment,
        "--stress-class",
        stress_class,
        "--lane",
        f"{args.lane}-{args.suite}",
    ]
    baselines = list(args.compare_to)
    if args.baseline_root and args.suite != "replay":
        compatible_dir = baseline_directory(Path(args.baseline_root), args.board_id, identity)
        compatible_manifest = compatible_dir / "manifest.json"
        compatible_identity_path = compatible_dir / "identity.json"
        legacy_dir = Path(args.baseline_root).resolve() / args.board_id / args.suite
        if compatible_manifest.is_file() and compatible_identity_path.is_file():
            baseline_identity = load_identity_manifest(compatible_identity_path)
            matches_current = all(
                baseline_identity.get(field) == identity.get(field)
                for field in ("product_fingerprint", "scenario_fingerprint")
            )
            if matches_current:
                baselines.append(str(compatible_manifest))
                print(f"[bench] using compatible baseline: {compatible_manifest}", flush=True)
            else:
                print(f"[bench] incompatible identity-keyed baseline ignored: {compatible_dir}", flush=True)
        elif compatible_manifest.is_file():
            print(f"[bench] baseline without identity manifest ignored: {compatible_dir}", flush=True)
        elif (legacy_dir / "manifest.json").is_file():
            print(
                f"[bench] legacy baseline ignored (explicit adoption required): {legacy_dir}",
                flush=True,
            )
    for baseline in baselines:
        if baseline:
            cmd.extend(["--compare-to", baseline])
    proc = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, check=False)
    (out_dir / "import_stdout.log").write_text(proc.stdout, encoding="utf-8")
    (out_dir / "import_stderr.log").write_text(proc.stderr, encoding="utf-8")
    return proc


class V1Emulator:
    """Own the LightBlue-compatible V1 emulator for one complete window."""

    COMPLETE_MARKER = b'V1REPLAY_EVENT {"state":"complete"}'

    def __init__(
        self,
        executable: Path,
        out_dir: Path,
        suite: str,
        blink_profile: str | None = None,
        handshake_only: bool = False,
        handshake_notification_hold_ms: int = 0,
    ):
        if (
            not isinstance(handshake_notification_hold_ms, int)
            or isinstance(handshake_notification_hold_ms, bool)
            or handshake_notification_hold_ms < 0
            or handshake_notification_hold_ms > 1_999
        ):
            raise ValueError(
                "handshake notification hold must be an integer from 0 through 1999 milliseconds"
            )
        if handshake_notification_hold_ms > 0 and (
            not handshake_only or suite != "replay"
        ):
            raise ValueError(
                "handshake notification hold requires a replay handshake-only emulator"
            )
        self.executable = executable
        self.suite = suite
        self.mode = "bench" if suite == "replay" else "idle"
        self.handshake_only = handshake_only
        self.handshake_notification_hold_ms = handshake_notification_hold_ms
        self.blink_profile = blink_profile or ("scenario" if suite == "replay" else "steady")
        self.log_path = out_dir / (RECONNECT_LOG_NAME if handshake_only else "v1replay.log")
        ledger_name = RECONNECT_LEDGER_NAME if handshake_only else HANDSHAKE_LEDGER_NAME
        self.handshake_ledger_path = out_dir / ledger_name if self.mode == "bench" else None
        self.process: subprocess.Popen[bytes] | None = None
        self.log_handle: Any = None
        self.started = False
        self.started_monotonic: float | None = None
        self.completed = False
        self.managed_stop = False
        self.returncode: int | None = None

    def start(self) -> None:
        if not self.executable.is_file() or not os.access(self.executable, os.X_OK):
            raise RuntimeError("v1replay executable is missing or not executable")
        if self.mode == "bench":
            if self.handshake_ledger_path is None:
                raise RuntimeError("replay handshake ledger path is unavailable")
            if self.handshake_ledger_path.exists():
                raise RuntimeError("refusing to reuse an existing replay handshake ledger")
        if self.log_path.exists():
            raise RuntimeError("refusing to overwrite an existing V1 emulator log")
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_handle = self.log_path.open("wb")
        command = [str(self.executable), self.mode]
        if self.mode == "bench":
            command.extend(["--machine-events", "--blink-profile", self.blink_profile])
            if self.handshake_only:
                command.extend(["--handshake-only", "--log-packets"])
                if self.handshake_notification_hold_ms > 0:
                    command.extend(
                        [
                            "--handshake-notification-hold-ms",
                            str(self.handshake_notification_hold_ms),
                        ]
                    )
            assert self.handshake_ledger_path is not None
            command.extend(["--handshake-ledger", str(self.handshake_ledger_path)])
        self.process = subprocess.Popen(
            command,
            cwd=self.executable.parent.parent,
            stdout=self.log_handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self.started_monotonic = time.monotonic()
        self.started = True
        print(f"[bench] launched V1 emulator mode={self.mode}; log: {self.log_path}", flush=True)

    def wait_for_handshake_ready(self, timeout_s: float) -> None:
        if self.handshake_ledger_path is None:
            raise RuntimeError("reconnect preflight handshake ledger path is unavailable")
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            problem = self.health_problem()
            if problem:
                raise RuntimeError(problem)
            if _preflight_ledger_is_complete(self.handshake_ledger_path):
                if self.health_problem():
                    raise RuntimeError("reconnect preflight exited as its handshake became ready")
                return
            time.sleep(0.05)
        raise ReconnectBehaviorError(
            "handshake_timeout",
            "reconnect preflight timed out before one complete active handshake epoch",
        )

    def finish_preflight(self, handshake_ready_while_alive: bool) -> dict[str, Any]:
        process_was_running = self.process is not None and self.process.poll() is None
        self.managed_stop = process_was_running
        self.stop()
        confirmed_exit = self.process is not None and self.process.poll() is not None
        return {
            "handshake_ready_while_alive": bool(handshake_ready_while_alive and process_was_running),
            "managed_stop": self.managed_stop,
            "confirmed_exit": confirmed_exit,
            "log": self.log_path.name if self.log_path.is_file() else "",
            "handshake_ledger": (
                self.handshake_ledger_path.name if self.handshake_ledger_path is not None else ""
            ),
        }

    def health_problem(self) -> str:
        if self.process is None:
            return "v1replay did not start"
        code = self.process.poll()
        if code is not None:
            self.returncode = code
            return f"V1 emulator exited early with code {code}"
        return ""

    def _bench_completed(self) -> bool:
        try:
            return self.COMPLETE_MARKER in self.log_path.read_bytes()
        except OSError:
            return False

    def _bench_event(self, state: str) -> dict[str, Any]:
        try:
            lines = self.log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return {}
        prefix = "V1REPLAY_EVENT "
        decoder = json.JSONDecoder()
        for line in reversed(lines):
            marker = line.find(prefix)
            if marker < 0:
                continue
            try:
                event, _end = decoder.raw_decode(line[marker + len(prefix) :])
            except (json.JSONDecodeError, TypeError):
                continue
            if isinstance(event, dict) and event.get("state") == state:
                return event
        return {}

    def _bench_configuration(self) -> dict[str, Any]:
        return self._bench_event("configured")

    def finish(self, window_completed: bool) -> dict[str, Any]:
        process_was_running = self.process is not None and self.process.poll() is None
        bench_completed = self._bench_completed() if self.mode == "bench" else True
        configuration = self._bench_configuration() if self.mode == "bench" else {}
        replay_started = self._bench_event("replay_started") if self.mode == "bench" else {}
        try:
            replay_started_monotonic = float(replay_started.get("hostMonotonicSeconds"))
        except (TypeError, ValueError):
            replay_started_monotonic = math.nan
        try:
            total_samples = int(configuration.get("totalSamples") or 0)
            cadence_hz = int(configuration.get("cadenceHz") or 0)
            blink_samples = int(configuration.get("blinkSamples") or 0)
        except (TypeError, ValueError):
            total_samples = 0
            cadence_hz = 0
            blink_samples = -1
        configuration_valid = self.mode != "bench" or (
            configuration.get("blinkProfile") == self.blink_profile
            and total_samples > 0
            and cadence_hz > 0
            and blink_samples >= 0
        )
        self.completed = bool(
            window_completed and process_was_running and bench_completed and configuration_valid
        )
        if process_was_running:
            self.managed_stop = True
            self.stop()
        elif self.process is not None:
            self.returncode = self.process.poll()
        self._close_log()
        return {
            "started": self.started,
            "completed": self.completed,
            "mode": self.mode,
            "blink_profile": str(configuration.get("blinkProfile") or self.blink_profile),
            "blink_source": str(configuration.get("blinkSource") or ""),
            "blink_samples": blink_samples,
            "blink_nominal_seconds": (blink_samples / cadence_hz) if cadence_hz else 0.0,
            "total_samples": total_samples,
            "cadence_hz": cadence_hz,
            "managed_stop": self.managed_stop,
            "returncode": self.returncode,
            "log": self.log_path.name if self.log_path.is_file() else "",
            "handshake_ledger": (
                self.handshake_ledger_path.name if self.handshake_ledger_path is not None else ""
            ),
            "replay_started_monotonic_seconds": (
                replay_started_monotonic if math.isfinite(replay_started_monotonic) else None
            ),
        }

    def _close_log(self) -> None:
        if self.log_handle is not None:
            self.log_handle.close()
            self.log_handle = None

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
                self.process.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired):
                if self.process.poll() is None:
                    try:
                        os.killpg(self.process.pid, signal.SIGKILL)
                    except OSError:
                        pass
                    try:
                        self.process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        pass
        if self.process is not None:
            self.returncode = self.process.poll()
        self._close_log()


def run_reconnect_preflight(
    q: BenchSerial,
    emulator: V1Emulator,
    timeout_s: float,
    post_ready_observation_s: float = 0,
    pre_stop_fence_timeout_s: float = 5.0,
) -> dict[str, Any]:
    """Prove managed disappearance and board cleanup before the scored window."""
    if (
        not isinstance(post_ready_observation_s, (int, float))
        or isinstance(post_ready_observation_s, bool)
        or not math.isfinite(float(post_ready_observation_s))
        or post_ready_observation_s < 0
    ):
        raise ValueError(
            "post-ready observation duration must be a finite non-negative number of seconds"
        )
    if (
        not isinstance(pre_stop_fence_timeout_s, (int, float))
        or isinstance(pre_stop_fence_timeout_s, bool)
        or not math.isfinite(float(pre_stop_fence_timeout_s))
        or pre_stop_fence_timeout_s <= 0
    ):
        raise ValueError(
            "pre-stop fence timeout must be a finite positive number of seconds"
        )
    observation_s = float(post_ready_observation_s)
    pre_stop_timeout_s = float(pre_stop_fence_timeout_s)
    initial_boot_count = q.boot_marker_count
    initial_cleanup_count = q.disconnect_cleanup_count
    handshake_ready = False
    serial_fence_observed = False
    post_exit_fence_observed = False
    process_exit_boundary_recorded = False
    result: dict[str, Any] = {}
    try:
        # Anchor every reconnect artifact to this already-ready serial session.
        # The scorer rejects cleanup or reboot evidence from here through B.
        q.record_host_boundary(RECONNECT_PREFLIGHT_START)
        emulator.start()
        emulator.wait_for_handshake_ready(timeout_s)
        handshake_ready = True

        if observation_s > 0:
            observation_deadline = time.monotonic() + observation_s
            while True:
                problem = emulator.health_problem()
                if problem:
                    raise RuntimeError(
                        "reconnect preflight exited during its post-ready observation: "
                        + problem
                    )
                remaining = observation_deadline - time.monotonic()
                if remaining <= 0:
                    break
                time.sleep(min(0.05, remaining))
            if emulator.handshake_ledger_path is None:
                raise RuntimeError(
                    "reconnect preflight handshake ledger path is unavailable after readiness"
                )
            if not _preflight_ledger_is_complete(emulator.handshake_ledger_path):
                raise RuntimeError(
                    "reconnect preflight handshake ledger did not remain complete after readiness"
                )

        # A response received while process A is still healthy drains every
        # earlier serial line, so stale cleanup cannot satisfy the later gate.
        q.record_host_boundary(RECONNECT_FENCE_BEGIN)
        establish_serial_fence(q, timeout_s=pre_stop_timeout_s)
        q.record_host_boundary(RECONNECT_FENCE_COMPLETE)
        serial_fence_observed = True
        if emulator.health_problem():
            raise RuntimeError("reconnect preflight exited before its serial fence")
        if emulator._bench_event("handshake_transport").get("active") is not True:
            raise ReconnectBehaviorError(
                "active_session_lost",
                "reconnect preflight lost its active short-notify session",
            )
        if q.boot_marker_count != initial_boot_count:
            raise RuntimeError("board rebooted during reconnect preflight")
        if q.disconnect_cleanup_count != initial_cleanup_count:
            raise ReconnectBehaviorError(
                "cleanup_before_stop",
                "V1 disconnect cleanup occurred before managed emulator stop",
            )

        result = emulator.finish_preflight(handshake_ready_while_alive=True)
        if result.get("confirmed_exit") is not True:
            raise RuntimeError("reconnect preflight process did not exit after managed stop")

        q.record_host_boundary("reconnect_preflight_process_exited")
        process_exit_boundary_recorded = True
        cleanup_deadline = time.monotonic() + timeout_s
        while time.monotonic() < cleanup_deadline:
            try:
                line = q.read_line(min(0.5, max(0.1, cleanup_deadline - time.monotonic())))
            except TimeoutError:
                continue
            if line.startswith(BOOT_PREFIX):
                raise RuntimeError("board rebooted while waiting for V1 disconnect cleanup")
            if line.startswith(V1_DISCONNECT_CLEANUP_PREFIX):
                break
        else:
            raise ReconnectBehaviorError(
                "cleanup_missing",
                "board did not report V1 disconnect cleanup after emulator exit",
            )

        # Drain through one more protocol response before QSTART so duplicate
        # cleanup lines cannot hide in the serial buffer and satisfy the gate.
        q.record_host_boundary(RECONNECT_POST_CLEANUP_FENCE_BEGIN)
        establish_serial_fence(q)
        q.record_host_boundary(RECONNECT_POST_CLEANUP_FENCE_COMPLETE)
        post_exit_fence_observed = True
        cleanup_markers = q.disconnect_cleanup_count - initial_cleanup_count
        if cleanup_markers != 1:
            raise ReconnectBehaviorError(
                "cleanup_count",
                "reconnect preflight requires exactly one new V1 disconnect cleanup marker"
            )
        if q.boot_marker_count != initial_boot_count:
            raise RuntimeError("board rebooted before the replacement emulator launch")

        result.update(
            {
                "serial_fence_observed": True,
                "cleanup_marker_count": cleanup_markers,
                "serial_session_continuous": True,
                "boot_observed_before_second_complete": False,
            }
        )
        return result
    except InterruptedError:
        # The outer collector owns process cleanup and the signal exit code.
        # Do not turn an operator interruption into reconnect evidence.
        raise
    except Exception as exc:
        behavioral = isinstance(exc, ReconnectBehaviorError)
        failure_kind = exc.kind if behavioral else "evidence_or_transport"
        if not result:
            result = emulator.finish_preflight(handshake_ready_while_alive=handshake_ready)
        if result.get("confirmed_exit") is True and not process_exit_boundary_recorded:
            q.record_host_boundary("reconnect_preflight_process_exited")
            process_exit_boundary_recorded = True
        if behavioral:
            # A negative result is a product FAIL only if the same serial
            # session still answers after process A has exited.
            if result.get("confirmed_exit") is not True or not process_exit_boundary_recorded:
                behavioral = False
                failure_kind = "process_exit_unconfirmed"
            try:
                if behavioral and not post_exit_fence_observed:
                    establish_serial_fence(q)
                    serial_fence_observed = True
            except InterruptedError:
                raise
            except Exception as serial_exc:
                behavioral = False
                failure_kind = "serial_failure"
                exc = RuntimeError(f"{exc}; serial health confirmation failed: {serial_exc}")
            if q.boot_marker_count != initial_boot_count:
                behavioral = False
                failure_kind = "board_reboot"
        result.update(
            {
                "serial_fence_observed": serial_fence_observed,
                "cleanup_marker_count": q.disconnect_cleanup_count - initial_cleanup_count,
                "serial_session_continuous": behavioral,
                "boot_observed_before_second_complete": q.boot_marker_count != initial_boot_count,
            }
        )
        raise ReconnectPreflightFailure(
            str(exc),
            result,
            classification="FAIL" if behavioral else "COLLECTION_FAILED",
            failure_kind=failure_kind,
        ) from exc


def collect_live(
    args: argparse.Namespace,
    out_dir: Path,
    after_upload: Callable[[], None] | None = None,
    identity_provider: Callable[[], dict[str, Any]] | None = None,
) -> tuple[
    Path,
    Path | None,
    dict[str, Any],
    str,
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
]:
    protocol_log = out_dir / "bench_serial.log"
    reserved_evidence = [
        protocol_log,
        out_dir / "window_result.json",
        out_dir / "v1replay.log",
        out_dir / "import_stdout.log",
        out_dir / "import_stderr.log",
        out_dir / "scoring.json",
        out_dir / "manifest.json",
    ]
    if args.suite == "replay":
        reserved_evidence.extend(
            [
                out_dir / HANDSHAKE_LEDGER_NAME,
                out_dir / RECONNECT_LOG_NAME,
                out_dir / RECONNECT_LEDGER_NAME,
            ]
        )
    if args.camera:
        reserved_evidence.append(out_dir / "camera")
    existing_evidence = [path.name for path in reserved_evidence if path.exists()]
    if existing_evidence:
        raise RuntimeError(
            "refusing to reuse existing live evidence: " + ", ".join(existing_evidence)
        )

    port = wait_for_port(args.port)
    if args.upload:
        print("[bench] uploading firmware/filesystem before first window", flush=True)
        run_upload(port, args.skip_web)
        if after_upload is not None:
            after_upload()
        port = wait_for_port(port, 30)
        time.sleep(2)
        wait_for_post_upload_settle(args.post_upload_settle_seconds)

    q: BenchSerial | None = None
    completion: dict[str, Any] = {}
    emulator = V1Emulator(
        Path(args.replay_executable).resolve(), out_dir, args.suite, args.blink_profile
    )
    emulator_result: dict[str, Any] = {}
    reconnect_preflight_result: dict[str, Any] = {}
    reconnect_preflight: V1Emulator | None = None
    camera = CameraCapture(out_dir / "camera", args.duration_seconds) if args.camera else None
    camera_result: dict[str, Any] = {}
    encounter_csv_path: Path | None = None
    collection_completed = False

    def admit_camera() -> None:
        nonlocal camera_result
        if camera is None:
            return
        preflight = run_camera_preflight(camera)
        if preflight.get("result") != "PASS":
            try:
                camera_result = json.loads(camera.result_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                camera_result = {"result": "CAPTURE_FAILED", "errors": list(camera.errors)}
            camera_result.update(
                {
                    "preflight": camera.preflight_result_path.name,
                    "preflight_result": preflight.get("result"),
                    "preflight_diagnostics": preflight.get("diagnostics") or [],
                }
            )
            failure = CameraPreflightFailure(preflight, camera_result)
            failure.reconnect_preflight = dict(reconnect_preflight_result)
            raise failure
        print(f"[bench] camera preflight passed; recording: {camera.video_path}", flush=True)

    try:
        if args.suite != "replay":
            admit_camera()
        print(f"[bench] opening serial port {port}; protocol log: {protocol_log}", flush=True)
        q = BenchSerial(port, args.baud, protocol_log)
        ready = wait_ready(q, args.ready_timeout_seconds)
        if args.suite == "replay":
            ready = establish_reconnect_readiness(q, args.ready_timeout_seconds)
        print(f"[bench] protocol ready: {ready}", flush=True)
        boot_count_before_reconnect: int | None = None
        cleanup_count_before_reconnect: int | None = None
        expected_cleanup_count: int | None = None
        if args.suite == "replay":
            boot_count_before_reconnect = q.boot_marker_count
            cleanup_count_before_reconnect = q.disconnect_cleanup_count
            reconnect_preflight = V1Emulator(
                Path(args.replay_executable).resolve(),
                out_dir,
                args.suite,
                args.blink_profile,
                handshake_only=True,
            )
            reconnect_preflight_result = run_reconnect_preflight(
                q,
                reconnect_preflight,
                args.ready_timeout_seconds,
            )
            print(
                "[bench] reconnect preflight passed; prior V1 session cleaned up",
                flush=True,
            )
            # The duration-bounded recording belongs only to process B and its
            # scored QSTART window, never to the unscored reconnect preflight.
            admit_camera()
            q.record_host_boundary(RECONNECT_PRE_QSTART_FENCE_BEGIN)
            establish_serial_fence(q)
            q.record_host_boundary(RECONNECT_PRE_QSTART_FENCE_COMPLETE)
            if q.boot_marker_count != boot_count_before_reconnect:
                raise RuntimeError("board rebooted before the replacement emulator launch")
            if q.disconnect_cleanup_count != cleanup_count_before_reconnect + 1:
                raise RuntimeError(
                    "unexpected V1 disconnect cleanup before the replacement emulator launch"
                )
            expected_cleanup_count = q.disconnect_cleanup_count
        completion = start_and_wait(
            q,
            args.suite,
            args.duration_seconds,
            args.completion_grace_seconds,
            after_started=emulator.start,
            health_check=lambda: (
                emulator.health_problem()
                or (
                    "board rebooted before the replacement V1 session completed"
                    if args.suite == "replay"
                    and q is not None
                    and boot_count_before_reconnect is not None
                    and q.boot_marker_count != boot_count_before_reconnect
                    else (
                        "an extra V1 disconnect cleanup occurred during the replacement session"
                        if args.suite == "replay"
                        and q is not None
                        and expected_cleanup_count is not None
                        and q.disconnect_cleanup_count != expected_cleanup_count
                        else ""
                    )
                )
            ),
        )
        if (
            args.suite == "replay"
            and boot_count_before_reconnect is not None
            and q.boot_marker_count != boot_count_before_reconnect
        ):
            raise RuntimeError("board rebooted before the replacement V1 session completed")
        if (
            args.suite == "replay"
            and expected_cleanup_count is not None
            and q.disconnect_cleanup_count != expected_cleanup_count
        ):
            raise RuntimeError("replacement V1 session disconnected before completion")
        try:
            csv_path = download_csv(q, out_dir, args.export_idle_timeout_seconds)
        except TimeoutError as exc:
            sd_path = str(completion.get("csvPath") or "")
            if not sd_path:
                raise
            print(f"[bench] export timed out ({exc}); retrying explicit SD path {sd_path}", flush=True)
            q.close()
            q = None
            last_error: Exception | None = exc
            for attempt in range(1, max(0, args.export_retries) + 1):
                try:
                    port = wait_for_port(port, 10)
                    print(f"[bench] recovery export attempt {attempt}/{args.export_retries}", flush=True)
                    q = BenchSerial(port, args.baud, protocol_log)
                    ready = wait_ready(q, args.ready_timeout_seconds)
                    print(f"[bench] recovery protocol ready: {ready}", flush=True)
                    csv_path = download_csv(q, out_dir, args.export_recovery_idle_timeout_seconds, sd_path)
                    break
                except Exception as retry_exc:  # noqa: BLE001 - keep retry evidence
                    last_error = retry_exc
                    if q is not None:
                        q.close()
                        q = None
                    time.sleep(1)
            else:
                raise RuntimeError(f"CSV export recovery failed: {last_error}") from last_error
        if args.suite == "replay":
            encounter_sd_path = encounter_csv_sd_path(str(completion.get("csvPath") or ""))
            if not encounter_sd_path:
                raise RuntimeError("Could not derive encounter CSV path from the perf CSV path")
            encounter_csv_path = download_csv(q, out_dir, args.export_idle_timeout_seconds, encounter_sd_path)
        collection_completed = True
    finally:
        if q is not None:
            q.close()
        if not emulator_result:
            emulator_result = emulator.finish(collection_completed)
        if reconnect_preflight is not None and reconnect_preflight.process is not None:
            if reconnect_preflight.process.poll() is None:
                reconnect_preflight.stop()
        if camera is not None:
            camera_result = camera.stop(collection_completed)
            try:
                camera_result = json.loads(camera.result_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                pass
            replay_started_monotonic = emulator_result.get("replay_started_monotonic_seconds")
            timeline_start_video_s, timing_anchor = replay_timing_anchor(
                args.suite,
                camera.recording_started_monotonic,
                replay_started_monotonic,
            )
            camera_grade: dict[str, Any] = {}
            capture_inputs_complete = args.suite != "replay" or encounter_csv_path is not None
            if camera_result.get("result") == "CAPTURED" and capture_inputs_complete:
                current_identity = identity_provider() if identity_provider is not None else {}
                capture_manifest = build_capture_manifest(
                    camera_dir=camera.out_dir,
                    camera_result=camera_result,
                    suite=args.suite,
                    product_fingerprint=str(current_identity.get("product_fingerprint") or ""),
                    scenario_fingerprint=str(current_identity.get("scenario_fingerprint") or ""),
                    encounter_csv_path=encounter_csv_path,
                    timing_anchor=timing_anchor,
                    traceability=(
                        current_identity.get("traceability")
                        if isinstance(current_identity.get("traceability"), dict)
                        else {}
                    ),
                )
                manifest_path, _created = publish_capture_manifest(camera.out_dir, capture_manifest)
                camera_result = {
                    **camera_result,
                    "capture_manifest": manifest_path.name,
                    "capture_id": capture_manifest["capture_id"],
                    "grader_fingerprint": current_identity.get("grader_fingerprint", ""),
                    "preflight": camera.preflight_result_path.name,
                    "preflight_result": "PASS",
                    "preflight_registration": capture_manifest.get("preflight", {}).get("registration", {}),
                }
            else:
                capture_manifest = {}
                current_identity = identity_provider() if identity_provider is not None else {}
            if camera_grade_required(args.suite, camera_result) and capture_manifest:
                grader_fingerprint = str(current_identity.get("grader_fingerprint") or "")
                grade_camera_result = camera_result_view(capture_manifest)
                camera_grade = grade_camera(
                    suite=args.suite,
                    camera_dir=camera.out_dir,
                    camera_result=grade_camera_result,
                    capture_manifest=capture_manifest,
                    grader_fingerprint=grader_fingerprint,
                    emulator_result=emulator_result,
                    encounter_csv_path=encounter_csv_path,
                    timeline_start_video_s=timeline_start_video_s,
                )
                grade_path, _created = publish_grade(
                    camera.out_dir,
                    capture_manifest,
                    grader_fingerprint,
                    camera_grade,
                )
                camera_result["visually_graded"] = True
                camera_result["grade"] = grade_path.relative_to(camera.out_dir).as_posix()
                camera_result["grade_result"] = camera_grade.get("result")
            grade_result = camera_grade.get("result") or (
                "ungraded" if camera_result.get("result") == "CAPTURED" else "unavailable"
            )
            print(f"[bench] camera capture={camera_result.get('result')} grade={grade_result}", flush=True)
    if not emulator_result.get("completed"):
        mode = str(emulator_result.get("mode") or args.suite)
        raise RuntimeError(f"V1 emulator mode={mode} did not cover the complete metrics window")
    return (
        csv_path,
        encounter_csv_path,
        completion,
        port,
        emulator_result,
        camera_result,
        reconnect_preflight_result,
    )


def camera_grade_required(suite: str, camera_result: dict[str, Any]) -> bool:
    """Only replay has an independent log contract suitable for camera grading."""
    return contract_grade_required(suite, str(camera_result.get("result") or ""))


def main() -> int:
    install_signal_handlers()
    args = parse_args()
    blink_profile_was_selected = args.blink_profile is not None or args.blink_arrow
    if args.blink_arrow:
        args.blink_profile = "stress"
    elif args.blink_profile is None:
        args.blink_profile = "scenario" if args.suite == "replay" else "steady"
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    identity_path = Path(args.identity_manifest).resolve()
    identity: dict[str, Any] = {}

    def refresh_identity() -> None:
        nonlocal identity
        identity = build_identity_manifest(
            ROOT,
            suite=args.suite,
            duration_seconds=args.duration_seconds,
            profile=args.profile,
            segment=args.segment,
            blink_profile=args.blink_profile,
            traceability={
                "repository_sha": args.git_sha,
                "repository_ref": args.git_ref,
                "worktree_clean": args.git_worktree_clean == "1",
            },
        )
        write_identity_manifest(identity_path, identity)

    refresh_identity()

    def identity_summary() -> dict[str, Any]:
        return {
            "identity_manifest": identity_path.name,
            "product_fingerprint": identity["product_fingerprint"],
            "grader_fingerprint": identity["grader_fingerprint"],
            "scenario_fingerprint": identity["scenario_fingerprint"],
        }

    if args.duration_seconds < 1:
        write_window_result(
            out_dir,
            {"result": "COLLECTION_FAILED", "suite": args.suite, "error": "duration must be positive"},
        )
        return 3
    if args.post_upload_settle_seconds < 0:
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "error": "post-upload settle duration cannot be negative",
            },
        )
        return 3
    if blink_profile_was_selected and args.suite != "replay":
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "error": "--blink-profile requires the replay suite",
            },
        )
        return 3
    if not args.from_csv and not args.replay_executable:
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "error": "live bench suites require --replay-executable for managed V1 emulation",
            },
        )
        return 3
    if args.from_csv and args.replay_executable:
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "error": "--replay-executable cannot be used with --from-csv",
            },
        )
        return 3
    if args.camera and args.from_csv:
        write_window_result(
            out_dir,
            {"result": "COLLECTION_FAILED", "suite": args.suite, "error": "--camera cannot be used with --from-csv"},
        )
        return 3

    try:
        if args.from_csv:
            source = Path(args.from_csv).resolve()
            if not source.is_file():
                raise RuntimeError(f"CSV not found: {source}")
            csv_path = out_dir / source.name
            if source != csv_path:
                shutil.copy2(source, csv_path)
            completion: dict[str, Any] = {"source": "from_csv"}
            port = ""
            emulator_result: dict[str, Any] = {}
            reconnect_preflight_result: dict[str, Any] = {}
            camera_result: dict[str, Any] = {}
            encounter_csv_path: Path | None = None
        else:
            (
                csv_path,
                encounter_csv_path,
                completion,
                port,
                emulator_result,
                camera_result,
                reconnect_preflight_result,
            ) = collect_live(
                args,
                out_dir,
                after_upload=refresh_identity if args.upload else None,
                identity_provider=lambda: identity,
            )

        import_proc = run_import(args, csv_path, out_dir, identity)
        scoring_path = out_dir / "scoring.json"
        manifest_path = out_dir / "manifest.json"
        result = "COLLECTION_FAILED" if import_proc.returncode >= 3 else "COLLECTED"
        write_window_result(
            out_dir,
            {
                "result": result,
                "suite": args.suite,
                "board_id": args.board_id,
                "git_sha": args.git_sha,
                "git_ref": args.git_ref,
                "git_worktree_clean": args.git_worktree_clean == "1",
                **identity_summary(),
                "duration_seconds": args.duration_seconds,
                "post_upload_settle_seconds": args.post_upload_settle_seconds if args.upload else 0,
                "segment": args.segment,
                "port": port,
                "csv_path": str(csv_path),
                "encounter_csv_path": str(encounter_csv_path) if encounter_csv_path else "",
                "handshake_ledger_path": (
                    str(out_dir / HANDSHAKE_LEDGER_NAME) if args.suite == "replay" else ""
                ),
                "reconnect_preflight_handshake_ledger_path": (
                    str(out_dir / RECONNECT_LEDGER_NAME) if args.suite == "replay" else ""
                ),
                "reconnect_preflight_log_path": (
                    str(out_dir / RECONNECT_LOG_NAME) if args.suite == "replay" else ""
                ),
                "bench_serial_log_path": str(out_dir / "bench_serial.log"),
                "reconnect_preflight": (
                    reconnect_preflight_result if args.suite == "replay" else {}
                ),
                "completion": completion,
                "v1_emulator": emulator_result,
                "replay": emulator_result if args.suite == "replay" else {},
                "camera": camera_result,
                "import_returncode": import_proc.returncode,
                "manifest_path": str(manifest_path) if manifest_path.exists() else "",
                "scoring_path": str(scoring_path) if scoring_path.exists() else "",
            },
        )
        return 0 if import_proc.returncode < 3 else 3
    except CameraPreflightFailure as exc:
        write_window_result(
            out_dir,
            {
                "result": "EVIDENCE_FAILED",
                "suite": args.suite,
                "board_id": args.board_id,
                "git_sha": args.git_sha,
                "git_ref": args.git_ref,
                "git_worktree_clean": args.git_worktree_clean == "1",
                **identity_summary(),
                "camera": exc.camera_result,
                "reconnect_preflight_handshake_ledger_path": (
                    str(out_dir / RECONNECT_LEDGER_NAME) if args.suite == "replay" else ""
                ),
                "reconnect_preflight_log_path": (
                    str(out_dir / RECONNECT_LOG_NAME) if args.suite == "replay" else ""
                ),
                "bench_serial_log_path": str(out_dir / "bench_serial.log"),
                "reconnect_preflight": exc.reconnect_preflight,
                "error": str(exc),
            },
        )
        print(f"[bench] camera preflight inconclusive: {exc}", file=sys.stderr)
        return 3
    except ReconnectPreflightFailure as exc:
        write_window_result(
            out_dir,
            {
                "result": (
                    "RECONNECT_FAILED"
                    if exc.classification == "FAIL"
                    else "COLLECTION_FAILED"
                ),
                "suite": args.suite,
                "duration_seconds": args.duration_seconds,
                "board_id": args.board_id,
                "git_sha": args.git_sha,
                "git_ref": args.git_ref,
                "git_worktree_clean": args.git_worktree_clean == "1",
                **identity_summary(),
                "reconnect_preflight_handshake_ledger_path": str(
                    out_dir / RECONNECT_LEDGER_NAME
                ),
                "reconnect_preflight_log_path": str(out_dir / RECONNECT_LOG_NAME),
                "bench_serial_log_path": str(out_dir / "bench_serial.log"),
                "reconnect_preflight": exc.result,
                "reconnect_failure_kind": exc.failure_kind,
                "error": str(exc),
            },
        )
        print(f"[bench] reconnect preflight failed: {exc}", file=sys.stderr)
        return 3
    except Exception as exc:  # noqa: BLE001 - top-level artifact capture
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "board_id": args.board_id,
                "git_sha": args.git_sha,
                "git_ref": args.git_ref,
                "git_worktree_clean": args.git_worktree_clean == "1",
                **identity_summary(),
                "error": str(exc),
            },
        )
        print(f"[bench] collection failed: {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
