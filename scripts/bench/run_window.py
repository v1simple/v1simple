#!/usr/bin/env python3
"""Collect one SD-backed bench window and preserve its raw evidence."""

from __future__ import annotations

import argparse
import binascii
from collections import deque
import errno
import fcntl
import glob
import hashlib
import json
import math
import os
import pwd
import re
import secrets
import signal
import stat
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[2]
from artifact_privacy import (
    privacy_safe_identifier,
    redact_artifact_text,
    sanitize_artifact_value,
)
from camera_artifacts import (
    build_capture_manifest,
    publish_capture_manifest,
)
from camera_capture import CameraCapture
from camera_preflight import run_camera_preflight

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover - exercised only on hosts without pyserial
    serial = None  # type: ignore

IMPORT_PERF_CSV = ROOT / "tools" / "import_perf_csv.py"
BUILD_SH = ROOT / "build.sh"
RUN_PROGRESS_INTERVAL_S = 15
QSYNC_PERIOD_SECONDS = 7.5
QSYNC_BURST_COUNT = 8
QSYNC_MAX_SEGMENT_RESTARTS = 4
QSYNC_REPLY = re.compile(
    r"^QSYNC ([0-9a-fA-F]{16}) ([0-9a-fA-F]{16}) "
    r"([0-9a-fA-F]{16}) ([0-9a-fA-F]{16})$"
)
QGETCSV_BUSY_RETRY_TIMEOUT_S = 15.0
QGETCSV_BUSY_RETRY_DELAY_S = 0.25
QABORT_CONFIRM_TIMEOUT_S = 5.0
RECONNECT_LOG_NAME = "v1replay_reconnect_preflight.log"
REPLAY_STIMULUS_SCHEMA = 1
REPLAY_STIMULUS_EVENT_STATE = "stimulus_requested"
REPLAY_STIMULUS_NAME = "replay_stimulus.ndjson"
BENCH_TIMELINE_NAME = "bench_timeline.ndjson"
BUILD_UPLOAD_ARTIFACTS_NAME = "build_upload_artifacts.json"
BUILD_OUTPUT_DIR = ROOT / ".pio" / "build" / "waveshare-349"
# esp_app_get_elf_sha256() exposes the prefix retained by the firmware's
# CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9 setting. Keep the full ELF SHA-256 in the
# file inventory, but compare QSTATUS runtimeImageId with this lowercase prefix.
RUNTIME_IMAGE_ID_HEX_LENGTH = 9
RUNTIME_IMAGE_ID_BASIS = "firmware.elf_sha256_lowercase_hex_prefix"
BUILD_UPLOAD_FILES = (
    "bootloader.bin",
    "partitions.bin",
    "firmware.bin",
    "firmware.elf",
    "littlefs.bin",
)
REPLAY_SCENARIO_EVIDENCE_NAME = "replay_scenario.json"
RECONNECT_SCENARIO_EVIDENCE_NAME = "replay_scenario_preflight.json"
V1_DISCONNECT_CLEANUP_PREFIX = "[BLE] V1 disconnected; cleared LCD BLE state at "
BOOT_PREFIX = "BOOT bootId="
RECONNECT_PREFLIGHT_START = "reconnect_preflight_start"
RECONNECT_FENCE_BEGIN = "reconnect_preflight_fence_begin"
RECONNECT_FENCE_COMPLETE = "reconnect_preflight_fence_complete"
RECONNECT_POST_CLEANUP_FENCE_BEGIN = "reconnect_post_cleanup_fence_begin"
RECONNECT_POST_CLEANUP_FENCE_COMPLETE = "reconnect_post_cleanup_fence_complete"
RECONNECT_PRE_QSTART_FENCE_BEGIN = "reconnect_pre_qstart_fence_begin"
RECONNECT_PRE_QSTART_FENCE_COMPLETE = "reconnect_pre_qstart_fence_complete"
ACCOUNT_HOME = Path(pwd.getpwuid(os.geteuid()).pw_dir).resolve()
# This must be shared across clones/worktrees and must never sit under artifact
# retention, which could unlink a live lock and allow a second advertiser.
V1_RADIO_LEASE_PATH = (
    ACCOUNT_HOME / ".local" / "state" / "v1simple" / "managed-v1-radio.lock"
)
V1_RADIO_QUIET_SECONDS = 1.0
# A campaign controller may acquire V1_RADIO_LEASE_PATH once and pass that
# *locked descriptor* to each run_window child with pass_fds plus this variable.
# The child validates the descriptor, duplicates it for its own lifetime, and
# passes only that duplicate to its managed emulator. Merely naming an open fd
# for the lock file is insufficient: the referenced open-file description must
# already own the exclusive flock.
V1_RADIO_LEASE_FD_ENV = "V1SIMPLE_MANAGED_V1_LEASE_FD"


def next_qsync_deadline(previous_deadline: float, observed_now: float) -> float:
    """Advance an anchored cadence without adding exchange latency to the period."""
    next_deadline = previous_deadline + QSYNC_PERIOD_SECONDS
    if next_deadline >= observed_now:
        return next_deadline
    missed_periods = math.floor(
        (observed_now - next_deadline) / QSYNC_PERIOD_SECONDS
    ) + 1
    return next_deadline + missed_periods * QSYNC_PERIOD_SECONDS


def _lease_path_owner(path: Path) -> Path:
    try:
        path.relative_to(ACCOUNT_HOME)
    except ValueError:
        # Alternate paths are accepted only for isolated tests; their direct
        # parent still must be a real user-owned directory.
        return path.parent
    return ACCOUNT_HOME


def _ensure_lease_directory_chain(
    owner: Path,
    target: Path,
    *,
    create: bool,
) -> tuple[int, int]:
    if not owner.is_absolute() or not target.is_absolute():
        raise RuntimeError("managed V1 radio lease directory must be absolute")
    try:
        relative = target.relative_to(owner)
    except ValueError as exc:
        raise RuntimeError("managed V1 radio lease directory escaped its owner") from exc
    current = owner
    for part in (".", *relative.parts):
        if part != ".":
            parent = current
            current = current / part
            if not os.path.lexists(current):
                if not create:
                    raise RuntimeError(
                        f"managed V1 radio lease directory is unavailable: {current}"
                    )
                try:
                    parent_before = parent.lstat()
                    os.mkdir(current, 0o700)
                    parent_after = parent.lstat()
                except FileExistsError:
                    pass
                except OSError as exc:
                    raise RuntimeError(
                        f"could not create managed V1 radio lease directory: {current}"
                    ) from exc
                else:
                    if (parent_before.st_dev, parent_before.st_ino) != (
                        parent_after.st_dev,
                        parent_after.st_ino,
                    ):
                        raise RuntimeError(
                            "managed V1 radio lease directory changed while it was created"
                        )
        try:
            metadata = current.lstat()
        except OSError as exc:
            raise RuntimeError(
                f"could not inspect managed V1 radio lease directory: {current}"
            ) from exc
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISDIR(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
        ):
            raise RuntimeError(
                "managed V1 radio lease requires user-owned directories without symlinks: "
                f"{current}"
            )
    return (metadata.st_dev, metadata.st_ino)


def _prepare_lease_parent(path: Path) -> tuple[int, int]:
    return _ensure_lease_directory_chain(
        _lease_path_owner(path),
        path.parent,
        create=True,
    )


def _verify_lease_parent(path: Path, expected_identity: tuple[int, int]) -> None:
    actual = _ensure_lease_directory_chain(
        _lease_path_owner(path),
        path.parent,
        create=False,
    )
    if actual != expected_identity:
        raise RuntimeError("managed V1 radio lease directory changed while opening the lock")

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


class CameraEvidenceFailure(RuntimeError):
    """A gated replay recorder failed after camera admission."""

    def __init__(self, message: str, camera: CameraCapture) -> None:
        super().__init__(message)
        self.camera = camera
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


class V1RadioLease:
    """Exclude other managed V1 advertisers, including campaign-owned children.

    Normally this object acquires the durable lock itself. If
    ``V1SIMPLE_MANAGED_V1_LEASE_FD`` is present, the value must be the canonical
    decimal number of an inherited, read/write descriptor for ``path`` whose
    open-file description already owns the exclusive flock. Invalid, stale,
    unlocked, or independently opened descriptors are rejected rather than
    falling back to a new lease.
    """

    def __init__(
        self,
        path: Path = V1_RADIO_LEASE_PATH,
        quiet_seconds: float = V1_RADIO_QUIET_SECONDS,
    ) -> None:
        self.path = path
        self.quiet_seconds = quiet_seconds
        self.fd: int | None = None
        self.inherited = False

    def _inherited_fd(self) -> int | None:
        raw = os.environ.get(V1_RADIO_LEASE_FD_ENV)
        if raw is None:
            return None
        try:
            fd = int(raw, 10)
        except ValueError as exc:
            raise RuntimeError(
                f"{V1_RADIO_LEASE_FD_ENV} must be a canonical decimal file descriptor"
            ) from exc
        if fd < 3 or raw != str(fd):
            raise RuntimeError(
                f"{V1_RADIO_LEASE_FD_ENV} must be a canonical decimal file descriptor"
            )
        return fd

    def _validate_inherited_fd(self, fd: int) -> None:
        parent_identity = _prepare_lease_parent(self.path)
        try:
            fd_stat = os.fstat(fd)
            fd_flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        except OSError as exc:
            raise RuntimeError(
                f"{V1_RADIO_LEASE_FD_ENV} does not name an open file descriptor"
            ) from exc
        try:
            path_stat = self.path.lstat()
        except OSError as exc:
            raise RuntimeError("inherited managed V1 radio lease path is unavailable") from exc
        if not stat.S_ISREG(fd_stat.st_mode) or not stat.S_ISREG(path_stat.st_mode):
            raise RuntimeError("inherited managed V1 radio lease is not a regular file")
        if (fd_stat.st_dev, fd_stat.st_ino) != (path_stat.st_dev, path_stat.st_ino):
            raise RuntimeError("inherited managed V1 radio lease does not match the lease path")
        if fd_stat.st_uid != os.geteuid() or path_stat.st_uid != os.geteuid():
            raise RuntimeError("inherited managed V1 radio lease is not owned by this user")
        if fd_stat.st_nlink != 1 or path_stat.st_nlink != 1:
            raise RuntimeError("inherited managed V1 radio lease link ownership is invalid")
        if fd_flags & os.O_ACCMODE != os.O_RDWR:
            raise RuntimeError("inherited managed V1 radio lease is not open read/write")
        _verify_lease_parent(self.path, parent_identity)

        # A separate open file description must observe the lock as busy. Then
        # an idempotent nonblocking lock on the inherited descriptor must
        # succeed. Together these checks distinguish the controller's inherited
        # locked descriptor from an unlocked or separately opened stale fd.
        contender_fd = os.open(
            self.path,
            os.O_RDWR | getattr(os, "O_NOFOLLOW", 0),
        )
        try:
            contender_stat = os.fstat(contender_fd)
            if (contender_stat.st_dev, contender_stat.st_ino) != (
                fd_stat.st_dev,
                fd_stat.st_ino,
            ):
                raise RuntimeError(
                    "inherited managed V1 radio lease path changed during validation"
                )
            try:
                fcntl.flock(contender_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as exc:
                if exc.errno not in (errno.EACCES, errno.EAGAIN):
                    raise RuntimeError(
                        "could not validate inherited managed V1 radio lease ownership"
                    ) from exc
            else:
                fcntl.flock(contender_fd, fcntl.LOCK_UN)
                raise RuntimeError("inherited managed V1 radio lease is not locked")

            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as exc:
                if exc.errno in (errno.EACCES, errno.EAGAIN):
                    raise RuntimeError(
                        "inherited managed V1 radio lease is locked by a different owner"
                    ) from exc
                raise RuntimeError(
                    "could not validate inherited managed V1 radio lease ownership"
                ) from exc
        finally:
            os.close(contender_fd)

    def __enter__(self) -> V1RadioLease:
        inherited_fd = self._inherited_fd()
        if inherited_fd is not None:
            self._validate_inherited_fd(inherited_fd)
            self.fd = os.dup(inherited_fd)
            os.set_inheritable(self.fd, True)
            self.inherited = True
            return self

        parent_identity = _prepare_lease_parent(self.path)
        fd = os.open(
            self.path,
            os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0),
            0o600,
        )
        try:
            fd_stat = os.fstat(fd)
            path_stat = self.path.lstat()
            if (
                not stat.S_ISREG(fd_stat.st_mode)
                or not stat.S_ISREG(path_stat.st_mode)
                or fd_stat.st_uid != os.geteuid()
                or path_stat.st_uid != os.geteuid()
                or fd_stat.st_nlink != 1
                or path_stat.st_nlink != 1
                or (fd_stat.st_dev, fd_stat.st_ino)
                != (path_stat.st_dev, path_stat.st_ino)
            ):
                raise RuntimeError("managed V1 radio lease ownership is invalid")
            _verify_lease_parent(self.path, parent_identity)
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            os.close(fd)
            raise RuntimeError(
                "managed V1 radio lease unavailable; another bench or orphan emulator owns it"
            ) from exc
        except Exception:
            os.close(fd)
            raise
        os.set_inheritable(fd, True)
        self.fd = fd
        self.inherited = False
        if self.quiet_seconds > 0:
            time.sleep(self.quiet_seconds)
        return self

    def close(self) -> None:
        if self.fd is None:
            return
        os.close(self.fd)
        self.fd = None

    def __exit__(self, _exc_type: Any, _exc: Any, _traceback: Any) -> None:
        self.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["core", "display", "replay"], required=True)
    parser.add_argument("--duration-seconds", type=int, default=300)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--runner-stdout-log", default="")
    parser.add_argument("--runner-stderr-log", default="")
    parser.add_argument("--port", default=os.environ.get("DEVICE_PORT", ""))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--board-id", default=os.environ.get("BENCH_BOARD_ID", "release"))
    parser.add_argument("--git-sha", default="")
    parser.add_argument("--git-ref", default="")
    parser.add_argument("--git-worktree-clean", choices=["0", "1"], default="0")
    parser.add_argument("--segment", default="last")
    parser.add_argument("--upload", action="store_true", help="Build/upload production firmware+filesystem first")
    parser.add_argument("--skip-web", action="store_true", help="Pass --skip-web to build.sh when uploading")
    parser.add_argument(
        "--post-upload-settle-seconds",
        type=int,
        default=90,
        help="SD settle interval after upload and before the captured QSTART window",
    )
    parser.add_argument(
        "--replay-executable",
        default="",
        help="v1replay executable used as the V1 emulator after QSTART acknowledgement",
    )
    parser.add_argument(
        "--scenario",
        default="",
        help="Optional scenario passed through to managed v1replay without host-side parsing",
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


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class BenchTimeline:
    """Append host-clock observations as they occur; never infer missing events."""

    def __init__(self, path: Path):
        self.path = path
        self.run_dir = path.parent
        path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = path.open("x", encoding="utf-8")
        self.record("timeline_opened")

    def _write(self, payload: dict[str, Any]) -> None:
        safe_payload = sanitize_artifact_value(payload, run_dir=self.run_dir)
        self.handle.write(
            json.dumps(safe_payload, ensure_ascii=True, allow_nan=False, separators=(",", ":"))
            + "\n"
        )
        self.handle.flush()

    def record(
        self,
        event: str,
        *,
        host_monotonic_ns: int | None = None,
        **fields: Any,
    ) -> int:
        observed = time.monotonic_ns() if host_monotonic_ns is None else host_monotonic_ns
        self._write(
            {
                "schema_version": 1,
                "event": event,
                "host_monotonic_ns": observed,
                **fields,
            }
        )
        return observed

    def record_external(self, payload: dict[str, Any], source: str) -> None:
        observed = time.monotonic_ns()
        envelope = {
            **payload,
            "schema_version": 1,
            "timeline_source": source,
            "observer_host_monotonic_ns": observed,
        }
        envelope.setdefault("source", source)
        self._write(envelope)

    def close(self) -> None:
        if self.handle.closed:
            return
        self.record("timeline_closed")
        self.handle.close()


def retain_build_upload_artifacts(
    out_dir: Path,
    build_dir: Path = BUILD_OUTPUT_DIR,
    *,
    upload_performed: bool = False,
) -> dict[str, Any]:
    """Record hashes of the exact local images left by the successful upload build."""
    out_dir.mkdir(parents=True, exist_ok=True)
    files: list[dict[str, Any]] = []
    missing: list[str] = []
    for name in BUILD_UPLOAD_FILES:
        path = build_dir / name
        if not path.is_file():
            missing.append(name)
            continue
        try:
            source_path = path.resolve().relative_to(ROOT).as_posix()
        except ValueError:
            source_path = path.name
        files.append(
            {
                "name": name,
                "source_path": source_path,
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    firmware_elf_sha256 = next(
        (item["sha256"] for item in files if item["name"] == "firmware.elf"),
        "",
    )
    expected_runtime_image_id = firmware_elf_sha256[:RUNTIME_IMAGE_ID_HEX_LENGTH]
    payload = {
        "schema_version": 1,
        "kind": "bench_build_upload_artifacts",
        "upload_performed": upload_performed,
        "expected_runtime_image_id": expected_runtime_image_id,
        "expected_runtime_image_id_basis": RUNTIME_IMAGE_ID_BASIS,
        "expected_runtime_image_id_hex_length": RUNTIME_IMAGE_ID_HEX_LENGTH,
        "files": files,
        "missing": missing,
    }
    path = out_dir / BUILD_UPLOAD_ARTIFACTS_NAME
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return {
        "path": path.name,
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
        "upload_performed": upload_performed,
        "expected_runtime_image_id": expected_runtime_image_id,
        "expected_runtime_image_id_basis": RUNTIME_IMAGE_ID_BASIS,
        "expected_runtime_image_id_hex_length": RUNTIME_IMAGE_ID_HEX_LENGTH,
        "files": files,
        "missing": missing,
    }


def write_window_result(out_dir: Path, payload: dict[str, Any]) -> None:
    payload.setdefault("schema_version", 3)
    payload.setdefault("timestamp_utc", utc_now())
    safe_payload = sanitize_artifact_value(payload, run_dir=out_dir)
    (out_dir / "window_result.json").write_text(
        json.dumps(safe_payload, indent=2) + "\n",
        encoding="utf-8",
    )


def resolve_runner_log_paths(args: argparse.Namespace, out_dir: Path) -> dict[str, Path]:
    """Accept only the two exact per-suite streams owned by bench.sh."""
    result: dict[str, Path] = {}
    for key, attribute, name in (
        ("stdout", "runner_stdout_log", "run.log"),
        ("stderr", "runner_stderr_log", "run.err"),
    ):
        reference = str(getattr(args, attribute, "") or "")
        if not reference:
            continue
        path = Path(reference).resolve()
        if path != (out_dir / name).resolve():
            raise ValueError(f"--{attribute.replace('_', '-')} must name {name} inside --out-dir")
        result[key] = path
    return result


def publish_replay_stimulus_evidence(
    emulator_result: dict[str, Any],
    out_dir: Path,
    *,
    suite: str,
    live: bool,
) -> dict[str, Any] | None:
    """Persist the raw sample-request event stream without transforming it."""
    if suite != "replay" or not live:
        return None
    events = emulator_result.pop("stimulus_events", [])
    base = {
        "schema_version": REPLAY_STIMULUS_SCHEMA,
        "status": "unavailable",
        "path": "",
        "sha256": "",
        "size_bytes": 0,
        "event_count": len(events) if isinstance(events, list) else 0,
        "reason": "",
    }
    if not isinstance(events, list):
        return {**base, "reason": "events_invalid"}
    try:
        payload = b"".join(
            (
                json.dumps(
                    event,
                    ensure_ascii=True,
                    allow_nan=False,
                    separators=(",", ":"),
                    sort_keys=True,
                )
                + "\n"
            ).encode("utf-8")
            for event in events
        )
    except (TypeError, ValueError):
        return {**base, "reason": "event_serialization_invalid"}
    path = out_dir / REPLAY_STIMULUS_NAME
    try:
        with path.open("xb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
    except OSError:
        return {**base, "reason": "publish_failed"}
    return {
        **base,
        "status": "captured",
        "path": path.name,
        "sha256": hashlib.sha256(payload).hexdigest(),
        "size_bytes": len(payload),
    }


def install_signal_handlers() -> None:
    handled = False

    def interrupt(signum: int, _frame: Any) -> None:
        nonlocal handled
        if handled:
            return
        handled = True
        # Cleanup owns the process after the first interruption. A repeated
        # Ctrl-C/SIGTERM must not interrupt emulator withdrawal or evidence flush.
        for managed_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(managed_signal, signal.SIG_IGN)
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
    """Wait in short, signal-interruptible steps before collection."""
    if seconds <= 0:
        return
    print(
        f"[bench] allowing {seconds}s for post-upload SD activity to settle before collection",
        flush=True,
    )
    remaining = float(seconds)
    while remaining > 0:
        interval = min(1.0, remaining)
        sleep(interval)
        remaining -= interval
    print("[bench] post-upload SD settle complete", flush=True)


class BenchSerial:
    def __init__(
        self,
        port: str,
        baud: int,
        log_path: Path,
        timeline: BenchTimeline | None = None,
    ):
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
        self.timeline = timeline
        self.last_receive_monotonic_ns: int | None = None
        self._serial_read_buffer = bytearray()
        self._serial_buffer_received_ns: int | None = None
        self._protocol_inbox: deque[tuple[str, int]] = deque()
        self._protocol_inbox_limit = 256

    def close(self) -> None:
        try:
            if self.ser.is_open:
                self.ser.close()
        finally:
            self.log.close()

    def write_command(self, command: str) -> int:
        line = command.rstrip("\r\n") + "\n"
        payload = line.encode("utf-8")
        safe_line = redact_artifact_text(line)
        self.log.write(f">>> {safe_line}")
        self.log.flush()
        sent = time.monotonic_ns()
        try:
            self.ser.write(payload)
            self.ser.flush()
        except Exception as exc:
            if self.timeline is not None:
                self.timeline.record(
                    "serial_send",
                    host_monotonic_ns=sent,
                    line=safe_line.rstrip("\n"),
                    status="failed",
                    error=type(exc).__name__,
                )
            raise
        if self.timeline is not None:
            self.timeline.record(
                "serial_send",
                host_monotonic_ns=sent,
                line=safe_line.rstrip("\n"),
                status="sent",
            )
        return sent

    def read_line(self, timeout_s: float) -> str:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            newline = self._serial_read_buffer.find(b"\n")
            if newline < 0:
                raw = self.ser.readline()
                chunk_received_ns = time.monotonic_ns()
                if not raw:
                    continue
                self._serial_read_buffer.extend(raw)
                self._serial_buffer_received_ns = chunk_received_ns
                newline = self._serial_read_buffer.find(b"\n")
                if newline < 0:
                    continue
            received = self._serial_buffer_received_ns or time.monotonic_ns()
            raw_line = bytes(self._serial_read_buffer[:newline])
            del self._serial_read_buffer[: newline + 1]
            if not self._serial_read_buffer:
                self._serial_buffer_received_ns = None
            text = raw_line.decode("utf-8", errors="replace").rstrip("\r")
            safe_text = redact_artifact_text(text)
            self.last_receive_monotonic_ns = received
            self.log.write(safe_text + "\n")
            self.log.flush()
            if self.timeline is not None:
                self.timeline.record(
                    "serial_receive",
                    host_monotonic_ns=received,
                    line=safe_text,
                )
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
        for index, (pending, received) in enumerate(self._protocol_inbox):
            if pending.startswith(prefixes):
                del self._protocol_inbox[index]
                self.last_receive_monotonic_ns = received
                return pending
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            try:
                line = self.read_line(min(0.5, remaining))
            except TimeoutError:
                continue
            if line.startswith(prefixes):
                return line
            if line.startswith("Q"):
                if len(self._protocol_inbox) >= self._protocol_inbox_limit:
                    raise RuntimeError("serial protocol inbox overflow")
                self._protocol_inbox.append(
                    (line, self.last_receive_monotonic_ns or time.monotonic_ns())
                )
        raise TimeoutError(f"timed out waiting for {prefixes}")


def parse_json_line(line: str, prefix: str) -> dict[str, Any]:
    if not line.startswith(prefix):
        raise RuntimeError(f"expected {prefix!r}, got: {line}")
    payload = json.loads(line[len(prefix):])
    if not isinstance(payload, dict):
        raise RuntimeError(f"expected {prefix!r} JSON object")
    return payload


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


def capture_qstatus_round_trip(
    q: BenchSerial,
    phase: str,
    timeout_s: float = 5.0,
) -> dict[str, Any]:
    """Record a designated status exchange without making it a new admission gate."""
    timeline = getattr(q, "timeline", None)
    if timeline is None:
        return {}
    sent = time.monotonic_ns()
    received: int | None = None
    try:
        observed_send = q.write_command("QSTATUS")
        if isinstance(observed_send, int) and not isinstance(observed_send, bool):
            sent = observed_send
        line = q.read_protocol_line(("QRESP ", "QERR "), timeout_s)
        received = getattr(q, "last_receive_monotonic_ns", None) or time.monotonic_ns()
        prefix = "QRESP " if line.startswith("QRESP ") else "QERR "
        payload = parse_json_line(line, prefix)
        status = "observed" if prefix == "QRESP " else "device_error"
        if timeline is not None:
            timeline.record(
                "qstatus_round_trip",
                phase=phase,
                status=status,
                request="QSTATUS",
                response_prefix=prefix.strip(),
                response=payload,
                send_host_monotonic_ns=sent,
                receive_host_monotonic_ns=received,
                duration_ns=max(0, received - sent),
            )
        return payload
    except Exception as exc:  # evidence gap remains visible without replacing the bench result
        if timeline is not None:
            timeline.record(
                "qstatus_round_trip",
                phase=phase,
                status="failed",
                request="QSTATUS",
                send_host_monotonic_ns=sent,
                receive_host_monotonic_ns=received,
                error=f"{type(exc).__name__}: {exc}",
            )
        return {}


class QSyncCollector:
    """Collect four-timestamp exchanges without creating a new bench gate."""

    def __init__(self, q: BenchSerial) -> None:
        self.q = q
        self.sequence = 0
        self.last_clock_segment: str | None = None
        self.pending_h1_by_nonce: dict[str, int] = {}

    def collect_one(self, phase: str, timeout_s: float = 2.0) -> dict[str, Any]:
        self.sequence += 1
        nonce = secrets.token_hex(8)
        h1 = time.monotonic_ns()
        h4: int | None = None
        record: dict[str, Any] = {
            "phase": phase,
            "exchange_sequence": self.sequence,
            "nonce": nonce,
            "status": "failed",
            "h1_host_ns": h1,
            "d2_dut_us": None,
            "d3_dut_us": None,
            "h4_host_ns": None,
            "clock_segment": None,
            "segment_changed": False,
            "unexpected_replies": [],
        }
        try:
            h1 = self.q.write_command(f"QSYNC {nonce}")
            record["h1_host_ns"] = h1
            self.pending_h1_by_nonce[nonce] = h1
            deadline = time.monotonic() + timeout_s
            while time.monotonic() < deadline:
                remaining = max(0.001, deadline - time.monotonic())
                line = self.q.read_protocol_line(("QSYNC ", "QERR "), remaining)
                reply_h4 = self.q.last_receive_monotonic_ns or time.monotonic_ns()
                if line.startswith("QERR "):
                    record["h4_host_ns"] = reply_h4
                    record["status"] = "device_error"
                    record["error"] = parse_json_line(line, "QERR ")
                    self.pending_h1_by_nonce.pop(nonce, None)
                    break
                match = QSYNC_REPLY.fullmatch(line)
                if match is None:
                    record["unexpected_replies"].append(
                        {
                            "status": "invalid_reply",
                            "h4_host_ns": reply_h4,
                            "reason": "fixed_width_reply_required",
                        }
                    )
                    continue
                reply_nonce, segment_wire, d2_hex, d3_hex = match.groups()
                reply_nonce = reply_nonce.lower()
                d2 = int(d2_hex, 16)
                d3 = int(d3_hex, 16)
                segment_wire = segment_wire.lower()
                segment = str(int(segment_wire, 16))
                if reply_nonce != nonce:
                    unexpected = {
                        "status": "nonce_mismatch",
                        "reply_nonce": reply_nonce,
                        "clock_segment": segment,
                        "clock_segment_wire": segment_wire,
                        "d2_dut_us": d2,
                        "d3_dut_us": d3,
                        "h4_host_ns": reply_h4,
                    }
                    original_h1 = self.pending_h1_by_nonce.pop(reply_nonce, None)
                    if original_h1 is not None:
                        unexpected["h1_host_ns"] = original_h1
                        unexpected["status"] = "late_observed"
                    record["unexpected_replies"].append(unexpected)
                    continue
                record.update(
                    {
                        "reply_nonce": reply_nonce,
                        "clock_segment": segment,
                        "clock_segment_wire": segment_wire,
                        "d2_dut_us": d2,
                        "d3_dut_us": d3,
                        "h4_host_ns": reply_h4,
                    }
                )
                if reply_h4 < h1 or d3 < d2:
                    record["status"] = "invalid_order"
                else:
                    changed = (
                        self.last_clock_segment is not None
                        and self.last_clock_segment != segment
                    )
                    record["status"] = "observed"
                    record["segment_changed"] = changed
                    self.last_clock_segment = segment
                self.pending_h1_by_nonce.pop(nonce, None)
                break
        except Exception as exc:  # raw clock exchange loss is deliberately non-gating
            record["error"] = f"{type(exc).__name__}: {exc}"
        timeline = getattr(self.q, "timeline", None)
        if timeline is not None:
            timeline.record("qsync_exchange", **record)
        return record

    def burst(self, phase: str, count: int = QSYNC_BURST_COUNT) -> list[dict[str, Any]]:
        records: list[dict[str, Any]] = []
        current_phase = phase
        for restart in range(QSYNC_MAX_SEGMENT_RESTARTS + 1):
            batch = [self.collect_one(current_phase) for _ in range(count)]
            records.extend(batch)
            if not any(record.get("segment_changed") is True for record in batch):
                break
            if restart == QSYNC_MAX_SEGMENT_RESTARTS:
                timeline = getattr(self.q, "timeline", None)
                if timeline is not None:
                    timeline.record(
                        "qsync_segment_restart_limit",
                        phase=phase,
                        status="incomplete",
                        restart_limit=QSYNC_MAX_SEGMENT_RESTARTS,
                    )
                break
            current_phase = f"{phase}_segment_restart_{restart + 1}"
        return records

    def periodic(self) -> None:
        record = self.collect_one("during_window")
        if record.get("segment_changed") is True:
            self.burst("during_window_segment_restart")


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


def start_and_wait(
    q: BenchSerial,
    suite: str,
    duration_s: int,
    grace_s: int,
    after_started: Callable[[], None] | None = None,
    health_check: Callable[[], str] | None = None,
    clock_sync: Callable[[], None] | None = None,
) -> dict[str, Any]:
    start_deadline = time.monotonic() + 15
    last_start_error: dict[str, Any] | None = None
    start_payload: dict[str, Any] | None = None
    firmware_suite = "core" if suite == "replay" else suite
    command = f"QSTART {firmware_suite} {duration_s}"

    def abort_started_window() -> str:
        try:
            q.write_command("QABORT")
            deadline = time.monotonic() + QABORT_CONFIRM_TIMEOUT_S
            last_unexpected = ""
            while time.monotonic() < deadline:
                line = q.read_protocol_line(
                    ("QRESP ", "QERR "),
                    max(0.1, deadline - time.monotonic()),
                )
                if line.startswith("QRESP "):
                    payload = parse_json_line(line, "QRESP ")
                    if (
                        payload.get("ok") is False
                        and payload.get("state") == "error"
                        and payload.get("suite") == firmware_suite
                        and payload.get("message") == "aborted"
                        and payload.get("error") == "aborted"
                    ):
                        return ""
                    last_unexpected = f"unexpected abort acknowledgement: {payload}"
                else:
                    last_unexpected = f"unexpected abort response: {line}"
            return last_unexpected or "timed out waiting for the abort acknowledgement"
        except Exception as exc:  # noqa: BLE001 - preserve cleanup beside root cause
            return str(exc)

    def preserve_failure_after_abort(exc: BaseException) -> None:
        abort_error = abort_started_window()
        if not abort_error:
            return
        detail = f"QABORT cleanup was not confirmed: {abort_error}"
        if isinstance(exc, Exception):
            exc.args = (f"{exc}; {detail}",)
            return
        if isinstance(exc, (KeyboardInterrupt, SystemExit)):
            exc.add_note(detail)
            return
        raise RuntimeError(f"{exc}; {detail}") from exc

    start_attempted = False
    while time.monotonic() < start_deadline:
        start_attempted = True
        try:
            q.write_command(command)
        except BaseException as exc:
            preserve_failure_after_abort(exc)
            raise
        attempt_deadline = min(start_deadline, time.monotonic() + 5)
        retry_start = False
        while time.monotonic() < attempt_deadline:
            remaining = max(0.1, attempt_deadline - time.monotonic())
            try:
                line = q.read_protocol_line(("QRESP ", "QERR "), remaining)
            except TimeoutError as exc:
                last_start_error = {"timeout": str(exc)}
                break
            except BaseException as exc:
                preserve_failure_after_abort(exc)
                raise
            if line.startswith("QRESP "):
                try:
                    payload = parse_json_line(line, "QRESP ")
                    if not isinstance(payload.get("ok"), bool):
                        raise RuntimeError("QSTART acknowledgement ok field is not boolean")
                except BaseException as exc:
                    preserve_failure_after_abort(exc)
                    raise
                if (
                    payload.get("ok") is True
                    and payload.get("state") == "running"
                    and payload.get("suite") == firmware_suite
                ):
                    start_payload = payload
                    break
                last_start_error = {"stale_response": payload}
                continue
            try:
                last_start_error = parse_json_line(line, "QERR ")
            except BaseException as exc:
                preserve_failure_after_abort(exc)
                raise
            retry_reason = str(last_start_error.get("error") or last_start_error.get("message") or "")
            if retry_reason == "perf_sd_busy_retry":
                retry_start = True
                break
            exc = RuntimeError(f"QSTART failed: {last_start_error}")
            preserve_failure_after_abort(exc)
            raise exc
        if start_payload is not None:
            break
        if retry_start:
            time.sleep(0.25)
            continue
    if start_payload is None:
        exc = RuntimeError(
            f"QSTART did not produce a running acknowledgement: {last_start_error}"
        )
        if start_attempted:
            preserve_failure_after_abort(exc)
        raise exc

    print(
        f"[bench] started suite={suite} duration={duration_s}s csv={start_payload.get('csvPath') or 'unknown'}; "
        "metrics are recording to SD",
        flush=True,
    )

    try:
        if after_started is not None:
            after_started()

        deadline = time.monotonic() + duration_s + grace_s
        run_started = time.monotonic()
        next_progress = run_started + RUN_PROGRESS_INTERVAL_S
        next_clock_sync = run_started + QSYNC_PERIOD_SECONDS
        last_event: dict[str, Any] = start_payload
        while time.monotonic() < deadline:
            if health_check is not None:
                problem = health_check()
                if problem:
                    raise RuntimeError(problem)
            now = time.monotonic()
            if clock_sync is not None and now >= next_clock_sync:
                scheduled_clock_sync = next_clock_sync
                clock_sync()
                next_clock_sync = next_qsync_deadline(
                    scheduled_clock_sync,
                    time.monotonic(),
                )
            try:
                line = q.read_protocol_line(("QEVENT ", "QERR "), 1)
            except TimeoutError:
                now = time.monotonic()
                if now >= next_progress:
                    elapsed_s = min(duration_s, int(now - run_started))
                    print(
                        f"[bench] running suite={suite}: {elapsed_s}/{duration_s}s elapsed",
                        flush=True,
                    )
                    next_progress = now + RUN_PROGRESS_INTERVAL_S
                continue
            prefix = "QEVENT " if line.startswith("QEVENT ") else "QERR "
            payload = parse_json_line(line, prefix)
            last_event = payload
            if payload.get("state") in {"done", "error"}:
                if prefix != "QEVENT ":
                    raise RuntimeError(f"bench terminal response was not QEVENT: {payload}")
                if not isinstance(payload.get("ok"), bool):
                    raise RuntimeError("bench terminal event ok field is not boolean")
                if payload.get("suite") != firmware_suite:
                    raise RuntimeError(
                        "bench terminal event suite="
                        f"{payload.get('suite')!r} expected={firmware_suite!r}"
                    )
                if payload.get("state") != "done" or payload.get("ok") is not True:
                    raise RuntimeError(f"bench window failed: {payload}")
                print(f"[bench] firmware completed suite={suite}: {payload}", flush=True)
                return payload
        raise RuntimeError(f"bench window timed out waiting for completion; last={last_event}")
    except BaseException as exc:
        preserve_failure_after_abort(exc)
        raise


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
    print(f"[bench] downloaded CSV {csv_path.name} ({len(payload)} bytes)", flush=True)
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


def panic_sidecar_path(perf_csv_path: str) -> str:
    """Return the adjacent panic sidecar name without interpreting its contents."""
    return perf_csv_path[: -len(".csv")] + ".panic.jsonl" if perf_csv_path.endswith(".csv") else ""


def file_artifact(path: Path | None, reason: str = "") -> dict[str, Any]:
    if path is None or not path.is_file():
        return {"status": "unavailable", "path": "", "reason": reason}
    return {
        "status": "captured",
        "path": path.name,
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
        "reason": "",
    }






def panic_sidecar_artifact(path: Path | None, reason: str = "") -> dict[str, Any]:
    return file_artifact(path, reason)


def collect_optional_sd_artifact(
    q: BenchSerial,
    out_dir: Path,
    idle_timeout_s: int,
    sd_path: str,
    label: str,
) -> dict[str, Any]:
    """Download one device-reported file while leaving absence/failure descriptive."""
    if not sd_path:
        return file_artifact(None, "device_path_unavailable")
    try:
        path = download_csv(q, out_dir, idle_timeout_s, sd_path)
    except Exception as exc:  # optional evidence never replaces the bench result
        print(f"[bench] {label} unavailable ({exc})", flush=True)
        return file_artifact(None, f"export_failed: {exc}")
    return file_artifact(path)




def display_commit_csv_sd_path(perf_csv_sd_path: str) -> str:
    """Where the firmware wrote this boot's display commit records."""
    prefix = "/perf/perf_boot_"
    if not perf_csv_sd_path.startswith(prefix) or not perf_csv_sd_path.endswith(".csv"):
        return ""
    boot_suffix = perf_csv_sd_path[len(prefix) :]
    boot_identity = boot_suffix[: -len(".csv")]
    if not boot_identity or "/" in boot_identity or ".." in boot_identity:
        return ""
    return f"/display_commits/display_commits_{boot_suffix}"




def validate_metrics(
    args: argparse.Namespace,
    csv_path: Path,
    out_dir: Path,
) -> subprocess.CompletedProcess[str]:
    """Validate the recorded metric lines without deriving another artifact."""
    command = [
        sys.executable,
        str(IMPORT_PERF_CSV),
        "--input",
        str(csv_path),
        "--suite",
        args.suite,
        "--segment",
        args.segment,
    ]
    process = subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    stdout = str(sanitize_artifact_value(process.stdout, run_dir=out_dir)).strip()
    stderr = str(sanitize_artifact_value(process.stderr, run_dir=out_dir)).strip()
    if stdout:
        print(stdout, flush=True)
    if stderr:
        print(stderr, file=sys.stderr, flush=True)
    return process


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
        lease_fd: int | None = None,
        scenario: str = "",
        machine_event: Callable[[dict[str, Any]], None] | None = None,
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
        self.lease_fd = lease_fd
        self.scenario = scenario
        self.machine_event = machine_event
        self.machine_events_observed = 0
        self.machine_event_observation_error = ""
        self.blink_profile = blink_profile or ("scenario" if suite == "replay" else "steady")
        self.log_path = out_dir / (RECONNECT_LOG_NAME if handshake_only else "v1replay.log")
        scenario_name = (
            RECONNECT_SCENARIO_EVIDENCE_NAME
            if handshake_only
            else REPLAY_SCENARIO_EVIDENCE_NAME
        )
        self.scenario_evidence_path = out_dir / scenario_name if self.mode == "bench" else None
        self.process: subprocess.Popen[bytes] | None = None
        self.log_handle: Any = None
        self.started = False
        self.started_monotonic: float | None = None
        self.completed = False
        self.managed_stop = False
        self.session_transport_owned = False
        self.session_transport_continuous = False
        self.graceful_stop_confirmed = False
        self._managed_shutdown_evidence: (
            tuple[list[dict[str, Any]], int, int] | None
        ) = None
        self.returncode: int | None = None

    def start(self) -> None:
        if not self.executable.is_file() or not os.access(self.executable, os.X_OK):
            raise RuntimeError("v1replay executable is missing or not executable")
        if self.mode == "bench":
            if self.scenario_evidence_path is None or self.scenario_evidence_path.exists():
                raise RuntimeError("refusing to reuse existing replay scenario evidence")
        if self.log_path.exists():
            raise RuntimeError("refusing to overwrite an existing V1 emulator log")
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_handle = self.log_path.open("wb")
        command = [str(self.executable), self.mode]
        if self.mode == "bench":
            if self.scenario:
                command.extend(["--scenario", self.scenario])
            assert self.scenario_evidence_path is not None
            command.extend(["--scenario-evidence", str(self.scenario_evidence_path)])
        command.extend(["--machine-events", "--owner-pid", str(os.getpid())])
        if self.mode == "bench":
            if self.handshake_only:
                command.extend(["--handshake-only", "--log-packets"])
                if self.handshake_notification_hold_ms > 0:
                    command.extend(
                        [
                            "--handshake-notification-hold-ms",
                            str(self.handshake_notification_hold_ms),
                        ]
                    )
            command.extend(["--blink-profile", self.blink_profile])
        self.process = subprocess.Popen(
            command,
            cwd=self.executable.parent.parent,
            stdout=self.log_handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            pass_fds=(() if self.lease_fd is None else (self.lease_fd,)),
        )
        self.started_monotonic = time.monotonic()
        self.started = True
        print(f"[bench] launched V1 emulator mode={self.mode}", flush=True)

    def wait_for_handshake_ready(self, timeout_s: float) -> None:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            problem = self.health_problem()
            if problem:
                raise RuntimeError(problem)
            ready = self._bench_event("handshake_ready")
            transport = self._bench_event("handshake_transport")
            if ready and transport.get("active") is True:
                if self.health_problem():
                    raise RuntimeError("reconnect preflight exited as its handshake became ready")
                return
            time.sleep(0.05)
        raise ReconnectBehaviorError(
            "handshake_timeout",
            "reconnect preflight timed out before one complete active handshake epoch",
        )

    def wait_for_session_transport(self, timeout_s: float) -> None:
        """Require proof that this idle emulator, not another advertiser, owns the DUT."""
        if self.mode != "idle":
            return
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            problem = self.health_problem()
            if problem:
                raise RuntimeError(problem)
            if self._bench_event("session_transport").get("active") is True:
                self.session_transport_owned = True
                return
            time.sleep(0.05)
        raise RuntimeError(
            "idle V1 emulator did not prove current-process session transport ownership"
        )

    def finish_preflight(self, handshake_ready_while_alive: bool) -> dict[str, Any]:
        process_was_running = self.process is not None and self.process.poll() is None
        self.managed_stop = self.managed_stop or process_was_running
        self.stop()
        confirmed_exit = self.process is not None and self.process.poll() is not None
        return {
            "handshake_ready_while_alive": bool(handshake_ready_while_alive and process_was_running),
            "managed_stop": self.managed_stop,
            "graceful_stop_confirmed": self.graceful_stop_confirmed,
            "confirmed_exit": confirmed_exit,
            "returncode": self.returncode,
            "log": self.log_path.name if self.log_path.is_file() else "",
            "scenario_evidence": (
                self.scenario_evidence_path.name
                if self.scenario_evidence_path is not None
                and self.scenario_evidence_path.is_file()
                else ""
            ),
            "machine_event_timeline_error": self.machine_event_observation_error,
        }

    def health_problem(self) -> str:
        self._observe_machine_events()
        if self.process is None:
            return "v1replay did not start"
        code = self.process.poll()
        if code is not None:
            self.returncode = code
            return f"V1 emulator exited early with code {code}"
        return ""

    def _bench_completed(self) -> bool:
        self._observe_machine_events()
        try:
            return self.COMPLETE_MARKER in self.log_path.read_bytes()
        except OSError:
            return False

    def _bench_event(self, state: str) -> dict[str, Any]:
        events = self._bench_events(state)
        return events[-1] if events else {}

    def _bench_events(self, state: str) -> list[dict[str, Any]]:
        self._observe_machine_events()
        return [event for event in self._read_machine_events(strict=False) if event["state"] == state]

    def _read_machine_events(self, *, strict: bool) -> list[dict[str, Any]]:
        try:
            lines = self.log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            if strict:
                raise RuntimeError("could not read the V1 emulator machine-event log") from exc
            return []
        prefix = "V1REPLAY_EVENT "
        decoder = json.JSONDecoder()
        events: list[dict[str, Any]] = []
        for line_number, line in enumerate(lines, start=1):
            marker = line.find(prefix)
            if marker < 0:
                continue
            payload = line[marker + len(prefix) :]
            try:
                event, end = decoder.raw_decode(payload)
            except (json.JSONDecodeError, TypeError) as exc:
                if strict:
                    raise RuntimeError(
                        f"malformed V1 emulator machine event at log line {line_number}"
                    ) from exc
                continue
            if payload[end:].strip():
                if strict:
                    raise RuntimeError(
                        f"trailing data in V1 emulator machine event at log line {line_number}"
                    )
                continue
            if not isinstance(event, dict):
                if strict:
                    raise RuntimeError(
                        f"V1 emulator machine event at log line {line_number} is not an object"
                    )
                continue
            if not isinstance(event.get("state"), str) or not event.get("state"):
                if strict:
                    raise RuntimeError(
                        f"V1 emulator machine event at log line {line_number} has no valid state"
                    )
                continue
            events.append(event)
        return events

    def _observe_machine_events(self) -> None:
        events = self._read_machine_events(strict=False)
        if len(events) <= self.machine_events_observed:
            return
        pending = events[self.machine_events_observed :]
        if self.machine_event is None or self.machine_event_observation_error:
            self.machine_events_observed = len(events)
            return
        for event in pending:
            try:
                self.machine_event(dict(event))
            except Exception as exc:  # evidence loss stays visible without changing bench behavior
                self.machine_event_observation_error = f"{type(exc).__name__}: {exc}"
                print(
                    "[bench] replay machine-event timeline write failed "
                    f"({self.machine_event_observation_error})",
                    file=sys.stderr,
                    flush=True,
                )
                self.machine_events_observed = len(events)
                return
            self.machine_events_observed += 1

    def _ordered_machine_events(self) -> list[dict[str, Any]]:
        """Read the complete child-authored event stream after process exit."""
        return self._read_machine_events(strict=True)

    def _validate_managed_shutdown(self) -> tuple[list[dict[str, Any]], int, int]:
        """Prove graceful withdrawal from the complete child-authored event stream."""
        if self.returncode != 0:
            raise RuntimeError(
                "managed V1 emulator graceful teardown exited with "
                f"code {self.returncode}"
            )
        events = self._ordered_machine_events()
        stopping_indexes = [
            index for index, event in enumerate(events) if event.get("state") == "stopping"
        ]
        stopped_indexes = [
            index for index, event in enumerate(events) if event.get("state") == "stopped"
        ]
        if len(stopping_indexes) != 1:
            raise RuntimeError(
                "managed V1 emulator shutdown requires exactly one stopping ownership snapshot"
            )
        if not stopped_indexes:
            raise RuntimeError(
                "managed V1 emulator exited without a graceful stopped marker"
            )
        if len(stopped_indexes) != 1:
            raise RuntimeError(
                "managed V1 emulator shutdown requires exactly one graceful stopped marker"
            )

        stopping_index = stopping_indexes[0]
        stopped_index = stopped_indexes[0]
        if stopping_index >= stopped_index:
            raise RuntimeError(
                "managed V1 emulator stopped marker preceded its stopping ownership snapshot"
            )
        if stopped_index != len(events) - 1:
            raise RuntimeError(
                "managed V1 emulator stopped marker is not the final machine event"
            )

        stopping_active = events[stopping_index].get("sessionTransportActive")
        if not isinstance(stopping_active, bool):
            raise RuntimeError(
                "managed V1 emulator stopping ownership snapshot is not boolean"
            )
        if not stopping_active:
            raise RuntimeError(
                "managed V1 emulator did not own session transport at the stopping boundary"
            )

        teardown_observed = False
        for index, event in enumerate(events):
            if event.get("state") != "session_transport":
                continue
            if not (stopping_index < index < stopped_index):
                continue
            active = event.get("active")
            if not isinstance(active, bool):
                raise RuntimeError(
                    "managed V1 emulator teardown session transport event is not boolean"
                )
            if active:
                raise RuntimeError(
                    "managed V1 emulator reactivated session transport during teardown"
                )
            teardown_observed = True
        if not teardown_observed:
            raise RuntimeError(
                "managed V1 emulator stopped without a teardown session transport event"
            )
        return events, stopping_index, stopped_index

    def _validate_idle_shutdown(self) -> None:
        """Prove one uninterrupted admitted session through the stopping snapshot."""
        self.session_transport_continuous = False
        if not self.session_transport_owned:
            raise RuntimeError(
                "idle V1 emulator shutdown has no prior current-process ownership admission"
            )
        if self._managed_shutdown_evidence is None:
            raise RuntimeError("idle V1 emulator has no managed shutdown evidence")
        events, stopping_index, _stopped_index = self._managed_shutdown_evidence
        first_owned_index: int | None = None
        for index, event in enumerate(events):
            if event.get("state") != "session_transport":
                continue
            active = event.get("active")
            if not isinstance(active, bool):
                raise RuntimeError("idle V1 emulator session transport event is not boolean")
            if index >= stopping_index:
                continue
            if active:
                if first_owned_index is None:
                    first_owned_index = index
            elif first_owned_index is not None:
                raise RuntimeError(
                    "idle V1 emulator lost session transport before the stopping boundary"
                )
        if first_owned_index is None:
            raise RuntimeError(
                "idle V1 emulator shutdown has no owned session transport event before stopping"
            )
        self.session_transport_continuous = True

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
            blink_samples = int(configuration.get("blinkSamples") or 0)
        except (TypeError, ValueError):
            total_samples = 0
            blink_samples = -1
        cadence_value = configuration.get("cadenceHz")
        cadence_hz: float | None = None
        cadence_valid = False
        if cadence_value is None:
            cadence_valid = configuration.get("scenarioOrigin") == "external"
        elif isinstance(cadence_value, (int, float)) and not isinstance(cadence_value, bool):
            cadence_hz = float(cadence_value)
            cadence_valid = math.isfinite(cadence_hz) and cadence_hz > 0
        configuration_valid = self.mode != "bench" or (
            configuration.get("blinkProfile") == self.blink_profile
            and total_samples > 0
            and cadence_valid
            and blink_samples >= 0
        )
        base_completed = bool(
            window_completed
            and process_was_running
            and bench_completed
            and configuration_valid
        )
        self.completed = False
        if process_was_running:
            self.managed_stop = True
            self.stop()
        elif self.process is not None:
            self.returncode = self.process.poll()
        self._close_log()
        stimulus_events = (
            self._bench_events(REPLAY_STIMULUS_EVENT_STATE) if self.mode == "bench" else []
        )
        idle_shutdown_valid = self.mode != "idle"
        if self.mode == "idle" and process_was_running and self.session_transport_owned:
            # Validate only after the child has flushed the complete ordered stream.
            self._validate_idle_shutdown()
            idle_shutdown_valid = True
        elif self.mode == "idle" and process_was_running and window_completed:
            raise RuntimeError(
                "idle V1 emulator completed without prior current-process ownership admission"
            )
        self.completed = bool(base_completed and idle_shutdown_valid)
        result = {
            "started": self.started,
            "completed": self.completed,
            "mode": self.mode,
            "blink_profile": str(configuration.get("blinkProfile") or self.blink_profile),
            "blink_source": str(configuration.get("blinkSource") or ""),
            "blink_samples": blink_samples,
            "blink_nominal_seconds": (blink_samples / cadence_hz) if cadence_hz else None,
            "total_samples": total_samples,
            "cadence_hz": cadence_hz,
            "managed_stop": self.managed_stop,
            "session_transport_owned": (
                self.session_transport_owned if self.mode == "idle" else None
            ),
            "session_transport_continuous": (
                self.session_transport_continuous if self.mode == "idle" else None
            ),
            "graceful_stop_confirmed": self.graceful_stop_confirmed,
            "returncode": self.returncode,
            "log": self.log_path.name if self.log_path.is_file() else "",
            "scenario_evidence": (
                self.scenario_evidence_path.name
                if self.scenario_evidence_path is not None
                and self.scenario_evidence_path.is_file()
                else ""
            ),
            "machine_event_timeline_error": self.machine_event_observation_error,
            "replay_started_monotonic_seconds": (
                replay_started_monotonic if math.isfinite(replay_started_monotonic) else None
            ),
        }
        if self.mode == "bench":
            result["stimulus_events"] = stimulus_events
        return result

    def _close_log(self) -> None:
        if self.log_handle is not None:
            self.log_handle.close()
            self.log_handle = None

    def _sanitize_log(self) -> None:
        try:
            raw = self.log_path.read_text(encoding="utf-8", errors="replace")
            safe = sanitize_artifact_value(raw, run_dir=self.log_path.parent)
            if safe != raw:
                self.log_path.write_text(safe, encoding="utf-8")
        except OSError as exc:
            try:
                self.log_path.unlink(missing_ok=True)
            except OSError:
                pass
            raise RuntimeError("could not privacy-sanitize the V1 emulator log") from exc

    def stop(self) -> None:
        managed_stop_requested = self.process is not None and self.process.poll() is None
        if managed_stop_requested:
            try:
                # Ask the emulator itself to run its graceful teardown. Only
                # the timeout fallback targets the whole isolated process group.
                self.process.send_signal(signal.SIGTERM)
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
        try:
            if managed_stop_requested:
                self.graceful_stop_confirmed = False
                self._managed_shutdown_evidence = self._validate_managed_shutdown()
                self.graceful_stop_confirmed = True
        finally:
            self._observe_machine_events()
            self._sanitize_log()


def run_reconnect_preflight(
    q: BenchSerial,
    emulator: V1Emulator,
    timeout_s: float,
    post_ready_observation_s: float = 0,
    pre_stop_fence_timeout_s: float = 5.0,
) -> dict[str, Any]:
    """Prove managed disappearance and board cleanup before the captured window."""
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
        # No cleanup or reboot may occur from here through process B.
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
            if emulator._bench_event("handshake_ready") == {}:
                raise RuntimeError("reconnect preflight lost its handshake-ready event")
            if emulator._bench_event("handshake_transport").get("active") is not True:
                raise ReconnectBehaviorError(
                    "active_session_lost",
                    "reconnect preflight lost its active short-notify session",
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
            # A negative result is a product failure only if the same serial
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
    artifacts: dict[str, Any] | None = None,
) -> tuple[
    Path,
    Path | None,
    dict[str, Any],
    str,
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
]:
    # This lease is acquired here, or safely adopted from a campaign controller,
    # before port discovery, upload, or board reset. Its fd is inherited by
    # every managed advertiser, so parent death cannot make an orphan invisible
    # to the next bench invocation.
    with V1RadioLease() as lease:
        assert lease.fd is not None
        return _collect_live(
            args,
            out_dir,
            lease_fd=lease.fd,
            artifacts=artifacts,
        )


def _collect_live(
    args: argparse.Namespace,
    out_dir: Path,
    *,
    lease_fd: int,
    artifacts: dict[str, Any] | None = None,
) -> tuple[
    Path,
    Path | None,
    dict[str, Any],
    str,
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
]:
    protocol_log = out_dir / "bench_serial.log"
    reserved_evidence = [
        protocol_log,
        out_dir / BENCH_TIMELINE_NAME,
        out_dir / BUILD_UPLOAD_ARTIFACTS_NAME,
        out_dir / "window_result.json",
        out_dir / "v1replay.log",
        out_dir / REPLAY_STIMULUS_NAME,
    ]
    if args.suite == "replay":
        reserved_evidence.extend(
            [
                out_dir / RECONNECT_LOG_NAME,
                out_dir / REPLAY_SCENARIO_EVIDENCE_NAME,
                out_dir / RECONNECT_SCENARIO_EVIDENCE_NAME,
            ]
        )
    if args.camera:
        reserved_evidence.append(out_dir / "camera")
    existing_evidence = [path.name for path in reserved_evidence if path.exists()]
    if existing_evidence:
        raise RuntimeError(
            "refusing to reuse existing live evidence: " + ", ".join(existing_evidence)
        )

    if artifacts is None:
        artifacts = {}
    artifacts.setdefault(
        "display_commits",
        file_artifact(None, "not_attempted"),
    )

    port = wait_for_port(args.port)
    if args.upload:
        print("[bench] uploading firmware/filesystem before first window", flush=True)
        run_upload(port, args.skip_web)
        artifacts["build_upload"] = retain_build_upload_artifacts(
            out_dir,
            upload_performed=True,
        )
        port = wait_for_port(port, 30)
        time.sleep(2)
        wait_for_post_upload_settle(args.post_upload_settle_seconds)
    else:
        artifacts["build_upload"] = retain_build_upload_artifacts(out_dir)

    q: BenchSerial | None = None
    completion: dict[str, Any] = {}
    scenario = str(getattr(args, "scenario", "") or "")
    timeline = BenchTimeline(out_dir / BENCH_TIMELINE_NAME)
    if "build_upload" in artifacts:
        timeline.record(
            "build_upload_artifacts",
            artifact_path=str(artifacts["build_upload"].get("path") or ""),
        )
    emulator = V1Emulator(
        Path(args.replay_executable).resolve(),
        out_dir,
        args.suite,
        args.blink_profile,
        lease_fd=lease_fd,
        scenario=scenario,
        machine_event=lambda payload: timeline.record_external(payload, "v1replay"),
    )
    emulator_result: dict[str, Any] = {}
    reconnect_preflight_result: dict[str, Any] = {}
    reconnect_preflight: V1Emulator | None = None
    camera = CameraCapture(out_dir / "camera", args.duration_seconds) if args.camera else None
    if camera is not None:
        camera.timeline_event = lambda payload: timeline.record_external(
            payload,
            "camera_recorder",
        )
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
        print("[bench] camera preflight passed; recording started", flush=True)

    try:
        if args.suite != "replay":
            admit_camera()
        print(
            f"[bench] opening serial port {redact_artifact_text(port)}; protocol log retained in run artifacts",
            flush=True,
        )
        q = BenchSerial(port, args.baud, protocol_log, timeline)
        ready = wait_ready(q, args.ready_timeout_seconds)
        if args.suite == "replay":
            ready = establish_reconnect_readiness(q, args.ready_timeout_seconds)
        print("[bench] protocol ready", flush=True)
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
                lease_fd=lease_fd,
                scenario=scenario,
                machine_event=lambda payload: timeline.record_external(
                    payload,
                    "v1replay_reconnect_preflight",
                ),
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
            # captured QSTART window, never to the reconnect preflight.
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

        def start_managed_emulator() -> None:
            emulator.start()
            if args.suite != "replay":
                emulator.wait_for_session_transport(args.ready_timeout_seconds)

        def require_healthy_replay_camera() -> str:
            if camera is None or args.suite != "replay":
                return ""
            problem = camera.health_problem()
            if not problem:
                return ""
            failure = CameraEvidenceFailure(problem, camera)
            failure.reconnect_preflight = dict(reconnect_preflight_result)
            raise failure

        require_healthy_replay_camera()
        capture_qstatus_round_trip(q, "pre_window")
        qsync = QSyncCollector(q)
        qsync.burst("before_window")

        completion = start_and_wait(
            q,
            args.suite,
            args.duration_seconds,
            args.completion_grace_seconds,
            after_started=start_managed_emulator,
            clock_sync=qsync.periodic,
            health_check=lambda: (
                emulator.health_problem()
                or require_healthy_replay_camera()
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
        qsync.burst("after_window")
        post_window_status = capture_qstatus_round_trip(q, "post_window")
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
                    q = BenchSerial(port, args.baud, protocol_log, timeline)
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
        artifacts["causal_trace"] = collect_optional_sd_artifact(
            q,
            out_dir,
            args.export_idle_timeout_seconds,
            str(post_window_status.get("causalTracePath") or ""),
            "causal trace",
        )
        artifacts["panic_sidecar"] = collect_optional_sd_artifact(
            q,
            out_dir,
            args.export_idle_timeout_seconds,
            panic_sidecar_path(str(completion.get("csvPath") or "")),
            "panic sidecar",
        )
        if args.suite == "replay":
            encounter_sd_path = encounter_csv_sd_path(str(completion.get("csvPath") or ""))
            if not encounter_sd_path:
                raise RuntimeError("Could not derive encounter CSV path from the perf CSV path")
            encounter_csv_path = download_csv(q, out_dir, args.export_idle_timeout_seconds, encounter_sd_path)

        # Preserve the renderer's raw commit lines exactly as downloaded.
        commit_sd_path = display_commit_csv_sd_path(str(completion.get("csvPath") or ""))
        if commit_sd_path:
            try:
                commit_csv_path = download_csv(q, out_dir, args.export_idle_timeout_seconds, commit_sd_path)
                artifacts["display_commits"] = file_artifact(commit_csv_path)
            except Exception as exc:  # noqa: BLE001 - evidence is optional, collection is not
                print(f"[bench] display commit log unavailable ({exc})", flush=True)
                artifacts["display_commits"] = file_artifact(None, f"export_failed: {exc}")
        else:
            artifacts["display_commits"] = file_artifact(None, "path_derivation_failed")
        collection_completed = True
    finally:
        primary_error = sys.exc_info()[1]
        cleanup_errors: list[tuple[str, Exception]] = []
        try:
            if not emulator_result:
                emulator_result = emulator.finish(collection_completed)
        except Exception as exc:  # noqa: BLE001 - finish remaining cleanup before failing closed
            cleanup_errors.append(("V1 emulator", exc))
        try:
            if reconnect_preflight is not None and reconnect_preflight.process is not None:
                if reconnect_preflight.process.poll() is None:
                    reconnect_preflight.stop()
        except Exception as exc:  # noqa: BLE001 - finish serial/camera cleanup before surfacing
            cleanup_errors.append(("reconnect preflight", exc))
        if q is not None:
            try:
                q.close()
            except Exception as exc:  # noqa: BLE001 - continue remaining cleanup
                cleanup_errors.append(("serial", exc))
        if camera is not None:
            try:
                camera_result = camera.stop(collection_completed)
                try:
                    camera_result = json.loads(camera.result_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    pass
                if camera_result.get("result") == "CAPTURED":
                    capture_manifest = build_capture_manifest(
                        camera_dir=camera.out_dir,
                        camera_result=camera_result,
                        suite=args.suite,
                    )
                    manifest_path, _created = publish_capture_manifest(
                        camera.out_dir,
                        capture_manifest,
                    )
                    camera_result = {
                        **camera_result,
                        "capture_manifest": manifest_path.name,
                        "capture_id": capture_manifest["capture_id"],
                        "preflight": camera.preflight_result_path.name,
                        "preflight_result": "PASS",
                    }
                print(
                    f"[bench] camera capture={camera_result.get('result')}",
                    flush=True,
                )
            except Exception as exc:  # noqa: BLE001 - aggregate every cleanup failure
                cleanup_errors.append(("camera", exc))
        try:
            timeline.close()
            artifacts["bench_timeline"] = {
                "path": timeline.path.name,
                "sha256": sha256_file(timeline.path),
                "size_bytes": timeline.path.stat().st_size,
            }
        except Exception as exc:  # noqa: BLE001 - timeline loss is visible but non-gating
            print(
                f"[bench] timeline finalization failed ({redact_artifact_text(str(exc))})",
                flush=True,
            )
            artifacts["bench_timeline"] = {
                "status": "failed",
                "path": timeline.path.name if timeline.path.exists() else "",
                "reason": str(exc),
            }
        if cleanup_errors:
            cleanup_detail = "; ".join(
                f"{owner}: {error}" for owner, error in cleanup_errors
            )
            if primary_error is not None:
                primary_error.args = (
                    f"{primary_error}; cleanup failure: {cleanup_detail}",
                )
            else:
                raise RuntimeError(f"cleanup failure: {cleanup_detail}") from cleanup_errors[0][1]
    if not emulator_result.get("completed"):
        mode = str(emulator_result.get("mode") or args.suite)
        raise RuntimeError(f"V1 emulator mode={mode} did not cover the complete metrics window")
    if camera is not None and camera_result.get("result") != "CAPTURED":
        failure = CameraEvidenceFailure("camera leg did not retain complete raw evidence", camera)
        failure.reconnect_preflight = dict(reconnect_preflight_result)
        raise failure
    return (
        csv_path,
        encounter_csv_path,
        completion,
        port,
        emulator_result,
        camera_result,
        reconnect_preflight_result,
        artifacts,
    )
def main() -> int:
    install_signal_handlers()
    args = parse_args()
    args.board_id = privacy_safe_identifier(args.board_id, namespace="board")
    if args.blink_arrow:
        args.blink_profile = "stress"
    elif args.blink_profile is None:
        args.blink_profile = "scenario" if args.suite == "replay" else "steady"

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    artifacts: dict[str, Any] = {
        "display_commits": file_artifact(None, "not_attempted"),
    }

    def fail(message: str, *, result: str = "COLLECTION_FAILED", **fields: Any) -> int:
        safe_message = redact_artifact_text(message)
        write_window_result(
            out_dir,
            {
                "result": result,
                "suite": args.suite,
                "board_id": args.board_id,
                "artifacts": artifacts,
                "error": safe_message,
                **fields,
            },
        )
        print(f"[bench] {result.lower()}: {safe_message}", file=sys.stderr, flush=True)
        return 2 if result == "FAIL" else 3

    try:
        resolve_runner_log_paths(args, out_dir)
    except ValueError as exc:
        return fail(str(exc))

    if args.duration_seconds < 1:
        return fail("duration must be positive")
    if args.post_upload_settle_seconds < 0:
        return fail("post-upload settle duration cannot be negative")
    if args.suite != "replay" and args.scenario:
        return fail("--scenario is valid only for replay")
    if args.suite != "replay" and args.blink_profile not in {"steady", None}:
        return fail("blink profile selection is valid only for replay")
    if not args.replay_executable:
        return fail("managed v1replay is required for live collection")
    if serial is None:
        return fail("pyserial is required for live collection")

    try:
        (
            csv_path,
            encounter_csv_path,
            completion,
            port,
            emulator_result,
            camera_result,
            reconnect_preflight_result,
            artifacts,
        ) = collect_live(args, out_dir, artifacts=artifacts)

        artifacts["perf_csv"] = file_artifact(csv_path)
        artifacts["encounter_csv"] = (
            file_artifact(encounter_csv_path)
            if encounter_csv_path is not None
            else file_artifact(None, "not_applicable")
        )
        replay_stimulus = publish_replay_stimulus_evidence(
            emulator_result,
            out_dir,
            suite=args.suite,
            live=True,
        )
        if replay_stimulus is not None:
            artifacts["replay_stimulus"] = replay_stimulus

        validation = validate_metrics(args, csv_path, out_dir)
        validation_lines = [
            line.strip()
            for line in (validation.stdout + "\n" + validation.stderr).splitlines()
            if line.strip()
        ]
        validation_message = redact_artifact_text(
            validation_lines[-1] if validation_lines else "metric validator returned no message"
        )[:512]
        result = (
            "PASS"
            if validation.returncode == 0
            else "FAIL"
            if validation.returncode == 2
            else "COLLECTION_FAILED"
        )
        write_window_result(
            out_dir,
            {
                "result": result,
                "suite": args.suite,
                "duration_seconds": args.duration_seconds,
                "board_id": args.board_id,
                "git_sha": args.git_sha,
                "git_ref": args.git_ref,
                "git_worktree_clean": args.git_worktree_clean == "1",
                "device_port": redact_artifact_text(port),
                "completion": completion,
                "emulator": emulator_result,
                "reconnect_preflight": reconnect_preflight_result,
                "camera": camera_result,
                "artifacts": artifacts,
                "metric_validation_returncode": validation.returncode,
                "metric_validation_message": validation_message,
            },
        )
        return 0 if result == "PASS" else (2 if result == "FAIL" else 3)
    except CameraPreflightFailure as exc:
        return fail(
            str(exc),
            result="FAIL",
            camera=exc.camera_result,
            reconnect_preflight=exc.reconnect_preflight,
            failure_kind="camera_preflight",
        )
    except CameraEvidenceFailure as exc:
        try:
            camera_result = json.loads(exc.camera.result_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            camera_result = {
                "result": "CAPTURE_FAILED",
                "errors": list(exc.camera.errors),
            }
        return fail(
            str(exc),
            camera=camera_result,
            reconnect_preflight=exc.reconnect_preflight,
            failure_kind="camera_evidence",
        )
    except ReconnectPreflightFailure as exc:
        return fail(
            str(exc),
            result=exc.classification,
            reconnect_preflight=exc.result,
            failure_kind=exc.failure_kind,
        )
    except (InterruptedError, KeyboardInterrupt) as exc:
        safe_message = redact_artifact_text(str(exc) or "interrupted")
        write_window_result(
            out_dir,
            {
                "result": "INTERRUPTED",
                "suite": args.suite,
                "board_id": args.board_id,
                "artifacts": artifacts,
                "error": safe_message,
            },
        )
        print(f"[bench] interrupted: {safe_message}", file=sys.stderr, flush=True)
        return 130
    except Exception as exc:  # noqa: BLE001 - preserve a bounded failure result
        return fail(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())
