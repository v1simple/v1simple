#!/usr/bin/env python3
"""Collect one external-only bench window and preserve its raw evidence."""

from __future__ import annotations

import argparse
import errno
import fcntl
import glob
import hashlib
import json
import os
import pwd
import re
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
from camera_artifacts import build_capture_manifest, publish_capture_manifest
from camera_capture import CameraCapture
from camera_preflight import run_camera_preflight

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover - host capability check
    serial = None  # type: ignore

BUILD_SH = ROOT / "build.sh"
BUILD_OUTPUT_DIR = ROOT / ".pio" / "build" / "waveshare-349"
BUILD_UPLOAD_FILES = (
    "bootloader.bin",
    "partitions.bin",
    "firmware.bin",
    "firmware.elf",
    "littlefs.bin",
)
BUILD_UPLOAD_ARTIFACTS_NAME = "build_upload_artifacts.json"
BENCH_TIMELINE_NAME = "bench_timeline.ndjson"
REPLAY_STIMULUS_NAME = "replay_stimulus.ndjson"
REPLAY_DELIVERY_NAME = "replay_delivery.ndjson"
REPLAY_SCENARIO_EVIDENCE_NAME = "replay_scenario.json"
REPLAY_STIMULUS_EVENT_STATE = "stimulus_requested"
REPLAY_DELIVERY_EVENT_STATES = frozenset(
    {
        "notification_requested",
        "notification_accepted",
        "notification_delayed",
        "notification_dropped",
        "notification_skipped",
    }
)
RUNTIME_IMAGE_ID_HEX_LENGTH = 9
RUNTIME_IMAGE_ID_BASIS = "firmware.elf_sha256_lowercase_hex_prefix"
RUN_PROGRESS_INTERVAL_S = 15
BOOT_RECORD_PREFIX = "BOOT "
GIT_IDENTITY_RE = re.compile(r"[0-9a-f]{7,40}")
RUNTIME_IMAGE_ID_RE = re.compile(r"[0-9a-f]{9}")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
BOOT_START_PREFIXES = (
    "ESP-ROM:",
    "Build:Mar ",
    "rst:",
    "Saved PC:",
    "SPIWP:",
    "load:",
    "entry ",
    "[NVS] Entries:",
    "V1 Gen2 Simple Display",
    "[BootTiming] reset=",
    "[Boot] stage=",
)

ACCOUNT_HOME = Path(pwd.getpwuid(os.geteuid()).pw_dir).resolve()
V1_RADIO_LEASE_PATH = (
    ACCOUNT_HOME / ".local" / "state" / "v1simple" / "managed-v1-radio.lock"
)
V1_RADIO_LEASE_FD_ENV = "V1SIMPLE_MANAGED_V1_LEASE_FD"
V1_RADIO_QUIET_SECONDS = 1.0


class CameraPreflightFailure(RuntimeError):
    def __init__(self, preflight: dict[str, Any], camera_result: dict[str, Any]):
        diagnostics = preflight.get("diagnostics") or []
        detail = diagnostics[0] if diagnostics and isinstance(diagnostics[0], dict) else {}
        super().__init__(
            str(detail.get("message") or detail.get("code") or "camera preflight failed")
        )
        self.preflight = preflight
        self.camera_result = camera_result


class CameraEvidenceFailure(RuntimeError):
    def __init__(self, message: str, camera: CameraCapture):
        super().__init__(message)
        self.camera = camera


class RuntimeIdentityFailure(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        identity: dict[str, Any] | None = None,
        qualification: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.identity = identity or {}
        self.qualification = qualification or {}


def _lease_owner(path: Path) -> Path:
    try:
        path.relative_to(ACCOUNT_HOME)
    except ValueError:
        return path.parent
    return ACCOUNT_HOME


def _secure_directory_chain(owner: Path, target: Path, *, create: bool) -> tuple[int, int]:
    if not owner.is_absolute() or not target.is_absolute():
        raise RuntimeError("managed V1 radio lease directory must be absolute")
    try:
        relative = target.relative_to(owner)
    except ValueError as exc:
        raise RuntimeError("managed V1 radio lease directory escaped its owner") from exc
    current = owner
    metadata = current.lstat()
    if (
        stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISDIR(metadata.st_mode)
        or metadata.st_uid != os.geteuid()
    ):
        raise RuntimeError(
            "managed V1 radio lease requires user-owned directories without symlinks"
        )
    for part in relative.parts:
        current = current / part
        if not os.path.lexists(current):
            if not create:
                raise RuntimeError("managed V1 radio lease directory is unavailable")
            os.mkdir(current, 0o700)
        metadata = current.lstat()
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISDIR(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
        ):
            raise RuntimeError(
                "managed V1 radio lease requires user-owned directories without symlinks"
            )
    return metadata.st_dev, metadata.st_ino


class V1RadioLease:
    """Exclude concurrent managed V1 advertisers across clones and campaigns."""

    def __init__(
        self,
        path: Path = V1_RADIO_LEASE_PATH,
        quiet_seconds: float = V1_RADIO_QUIET_SECONDS,
    ) -> None:
        self.path = path
        self.quiet_seconds = quiet_seconds
        self.fd: int | None = None
        self.inherited = False

    def _validate_inherited(self, inherited_fd: int) -> None:
        parent_identity = _secure_directory_chain(
            _lease_owner(self.path), self.path.parent, create=True
        )
        fd_stat = os.fstat(inherited_fd)
        path_stat = self.path.lstat()
        if (
            not stat.S_ISREG(fd_stat.st_mode)
            or not stat.S_ISREG(path_stat.st_mode)
            or (fd_stat.st_dev, fd_stat.st_ino) != (path_stat.st_dev, path_stat.st_ino)
            or fd_stat.st_uid != os.geteuid()
            or path_stat.st_uid != os.geteuid()
        ):
            raise RuntimeError("inherited managed V1 radio lease is invalid")
        if fcntl.fcntl(inherited_fd, fcntl.F_GETFL) & os.O_ACCMODE != os.O_RDWR:
            raise RuntimeError("inherited managed V1 radio lease is not open read/write")
        if (
            _secure_directory_chain(_lease_owner(self.path), self.path.parent, create=False)
            != parent_identity
        ):
            raise RuntimeError("managed V1 radio lease directory changed while opening")

    def __enter__(self) -> V1RadioLease:
        raw_fd = os.environ.get(V1_RADIO_LEASE_FD_ENV)
        if raw_fd is not None:
            try:
                inherited_fd = int(raw_fd, 10)
            except ValueError as exc:
                raise RuntimeError("managed V1 radio lease descriptor is invalid") from exc
            if inherited_fd < 3 or raw_fd != str(inherited_fd):
                raise RuntimeError("managed V1 radio lease descriptor is invalid")
            self._validate_inherited(inherited_fd)
            self.fd = os.dup(inherited_fd)
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as exc:
                os.close(self.fd)
                self.fd = None
                raise RuntimeError(
                    "inherited managed V1 radio lease does not own the exclusive lock"
                ) from exc
            self.inherited = True
            return self

        _secure_directory_chain(_lease_owner(self.path), self.path.parent, create=True)
        flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0)
        self.fd = os.open(self.path, flags, 0o600)
        try:
            metadata = os.fstat(self.fd)
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.geteuid():
                raise RuntimeError("managed V1 radio lease is not a user-owned regular file")
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            os.close(self.fd)
            self.fd = None
            if exc.errno in (errno.EACCES, errno.EAGAIN):
                raise RuntimeError("another managed V1 advertiser owns the radio") from exc
            raise
        return self

    def __exit__(self, _exc_type: Any, _exc: Any, _traceback: Any) -> None:
        if self.fd is None:
            return
        if not self.inherited and self.quiet_seconds > 0:
            time.sleep(self.quiet_seconds)
        os.close(self.fd)
        self.fd = None


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
    parser.add_argument("--upload", action="store_true")
    parser.add_argument("--skip-web", action="store_true")
    parser.add_argument("--post-upload-settle-seconds", type=int, default=90)
    parser.add_argument("--replay-executable", default="")
    parser.add_argument("--scenario", default="")
    blink_group = parser.add_mutually_exclusive_group()
    blink_group.add_argument(
        "--blink-profile", choices=["scenario", "steady", "stress"], default=None
    )
    blink_group.add_argument("--blink-arrow", action="store_true")
    parser.add_argument("--camera", action="store_true")
    parser.add_argument("--ready-timeout-seconds", type=int, default=45)
    parser.add_argument("--completion-grace-seconds", type=int, default=45)
    return parser.parse_args()


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_artifact(path: Path) -> dict[str, Any]:
    return {
        "path": path.name,
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
    }


class BenchTimeline:
    def __init__(self, path: Path):
        self.path = path
        self.run_dir = path.parent
        self.handle = path.open("x", encoding="utf-8")
        self.record("timeline_opened")

    def record(self, event: str, **fields: Any) -> None:
        payload = sanitize_artifact_value(
            {
                "schema_version": 1,
                "event": event,
                "host_monotonic_ns": time.monotonic_ns(),
                **fields,
            },
            run_dir=self.run_dir,
        )
        self.handle.write(json.dumps(payload, separators=(",", ":"), allow_nan=False) + "\n")
        self.handle.flush()

    def record_external(self, payload: dict[str, Any], source: str) -> None:
        self.record("external_event", source=source, payload=payload)

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
    files: list[dict[str, Any]] = []
    missing: list[str] = []
    for name in BUILD_UPLOAD_FILES:
        path = build_dir / name
        if not path.is_file():
            missing.append(name)
            continue
        files.append(
            {
                "name": name,
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    elf_sha = next((item["sha256"] for item in files if item["name"] == "firmware.elf"), "")
    payload = {
        "schema_version": 1,
        "kind": "bench_build_upload_artifacts",
        "upload_performed": upload_performed,
        "expected_runtime_image_id": elf_sha[:RUNTIME_IMAGE_ID_HEX_LENGTH],
        "expected_runtime_image_id_basis": RUNTIME_IMAGE_ID_BASIS,
        "files": files,
        "missing": missing,
    }
    path = out_dir / BUILD_UPLOAD_ARTIFACTS_NAME
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return {**file_artifact(path), **payload}


def parse_runtime_boot_identity(line: str) -> dict[str, Any] | None:
    if not line.startswith(BOOT_RECORD_PREFIX):
        return None

    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if token.count("=") != 1:
            raise RuntimeIdentityFailure("malformed runtime BOOT identity")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise RuntimeIdentityFailure("malformed runtime BOOT identity")
        fields[key] = value

    missing = [name for name in ("bootId", "git", "image") if name not in fields]
    if missing:
        raise RuntimeIdentityFailure(
            "malformed runtime BOOT identity: missing " + ", ".join(missing)
        )
    try:
        boot_id = int(fields["bootId"], 10)
    except ValueError as exc:
        raise RuntimeIdentityFailure("malformed runtime BOOT identity: invalid bootId") from exc
    if not (1 <= boot_id <= 0xFFFFFFFF) or str(boot_id) != fields["bootId"]:
        raise RuntimeIdentityFailure("malformed runtime BOOT identity: invalid bootId")
    if GIT_IDENTITY_RE.fullmatch(fields["git"]) is None:
        raise RuntimeIdentityFailure("malformed runtime BOOT identity: invalid git")
    if RUNTIME_IMAGE_ID_RE.fullmatch(fields["image"]) is None:
        raise RuntimeIdentityFailure("malformed runtime BOOT identity: invalid image")
    return {
        "boot_id": boot_id,
        "git_sha": fields["git"],
        "image_id": fields["image"],
    }


class RuntimeIdentityTracker:
    def __init__(self) -> None:
        self.identity: dict[str, Any] | None = None
        self.boot_marker_count = 0

    def observe(self, line: str) -> None:
        if not line.startswith(BOOT_RECORD_PREFIX):
            return
        self.boot_marker_count += 1
        identity = parse_runtime_boot_identity(line)
        assert identity is not None
        if self.identity is not None and identity != self.identity:
            raise RuntimeIdentityFailure(
                "runtime BOOT identity changed during collection",
                identity=identity,
            )
        self.identity = identity


def _retained_elf_image_id(build_upload: dict[str, Any]) -> tuple[str, str]:
    if build_upload.get("expected_runtime_image_id_basis") != RUNTIME_IMAGE_ID_BASIS:
        return "", "retained firmware ELF identity basis is missing or inconsistent"
    files = build_upload.get("files")
    if not isinstance(files, list):
        return "", "retained firmware ELF artifact list is missing"
    elf_files = [
        item
        for item in files
        if isinstance(item, dict) and item.get("name") == "firmware.elf"
    ]
    if len(elf_files) != 1:
        return "", "retained firmware ELF artifact is missing or duplicated"
    elf_sha = elf_files[0].get("sha256")
    if not isinstance(elf_sha, str) or SHA256_RE.fullmatch(elf_sha) is None:
        return "", "retained firmware ELF hash is missing or malformed"
    image_id = elf_sha[:RUNTIME_IMAGE_ID_HEX_LENGTH]
    if build_upload.get("expected_runtime_image_id") != image_id:
        return "", "retained firmware ELF identity is inconsistent with its manifest"
    return image_id, ""


def qualify_runtime_identity(
    identity: dict[str, Any],
    *,
    intended_git_sha: str,
    build_upload: dict[str, Any],
    upload: bool,
) -> dict[str, Any]:
    mode = "upload" if upload else "no_flash"
    qualification: dict[str, Any] = {
        "status": "unqualified",
        "mode": mode,
        "git_match": False,
        "artifact_linked": False,
    }
    observed_git = str(identity.get("git_sha") or "")
    if GIT_IDENTITY_RE.fullmatch(intended_git_sha) is None:
        raise RuntimeIdentityFailure(
            "intended source git identity is missing or malformed",
            identity=identity,
            qualification=qualification,
        )
    if GIT_IDENTITY_RE.fullmatch(observed_git) is None or not intended_git_sha.startswith(
        observed_git
    ):
        raise RuntimeIdentityFailure(
            f"runtime git {observed_git or '<missing>'} does not match intended source commit {intended_git_sha}",
            identity=identity,
            qualification=qualification,
        )
    qualification["git_match"] = True

    if bool(build_upload.get("upload_performed")) != upload:
        raise RuntimeIdentityFailure(
            f"{mode} artifact manifest does not match the collection mode",
            identity=identity,
            qualification=qualification,
        )
    artifact_image_id, artifact_problem = _retained_elf_image_id(build_upload)
    observed_image_id = str(identity.get("image_id") or "")
    image_match = bool(artifact_image_id and observed_image_id == artifact_image_id)
    qualification.update(
        {
            "artifact_image_id": artifact_image_id,
            "image_match": image_match,
        }
    )

    if upload:
        if artifact_problem:
            raise RuntimeIdentityFailure(
                artifact_problem,
                identity=identity,
                qualification=qualification,
            )
        if not image_match:
            raise RuntimeIdentityFailure(
                f"runtime image {observed_image_id or '<missing>'} does not match uploaded firmware image {artifact_image_id}",
                identity=identity,
                qualification=qualification,
            )
        qualification.update(
            {
                "status": "qualified",
                "artifact_linked": True,
                "artifact": BUILD_UPLOAD_ARTIFACTS_NAME,
            }
        )
        return qualification

    if image_match:
        qualification.update(
            {
                "status": "qualified",
                "artifact_linked": True,
                "artifact": BUILD_UPLOAD_ARTIFACTS_NAME,
            }
        )
        return qualification

    reason = artifact_problem or (
        f"resident runtime image {observed_image_id or '<missing>'} is not linked to the retained firmware ELF"
    )
    qualification.update({"status": "collection_only", "reason": reason})
    return qualification


def write_window_result(out_dir: Path, payload: dict[str, Any]) -> None:
    payload.setdefault("schema_version", 5)
    payload.setdefault("timestamp_utc", utc_now())
    safe = sanitize_artifact_value(payload, run_dir=out_dir)
    (out_dir / "window_result.json").write_text(
        json.dumps(safe, indent=2) + "\n", encoding="utf-8"
    )


def resolve_runner_log_paths(args: argparse.Namespace, out_dir: Path) -> dict[str, Path]:
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
    emulator_result: dict[str, Any], out_dir: Path, *, suite: str
) -> dict[str, Any] | None:
    if suite != "replay":
        return None
    events = emulator_result.pop("stimulus_events", [])
    if not isinstance(events, list):
        raise RuntimeError("replay stimulus event stream is invalid")
    payload = b"".join(
        (
            json.dumps(event, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n"
        ).encode("utf-8")
        for event in events
    )
    path = out_dir / REPLAY_STIMULUS_NAME
    with path.open("xb") as handle:
        handle.write(payload)
        handle.flush()
        os.fsync(handle.fileno())
    return {**file_artifact(path), "event_count": len(events), "status": "captured"}


def publish_replay_delivery_evidence(
    emulator_result: dict[str, Any], out_dir: Path, *, suite: str
) -> dict[str, Any] | None:
    if suite != "replay":
        return None
    events = emulator_result.pop("delivery_events", [])
    if not isinstance(events, list):
        raise RuntimeError("replay notification delivery event stream is invalid")
    payload = b"".join(
        (
            json.dumps(event, sort_keys=True, separators=(",", ":"), allow_nan=False)
            + "\n"
        ).encode("utf-8")
        for event in events
    )
    path = out_dir / REPLAY_DELIVERY_NAME
    with path.open("xb") as handle:
        handle.write(payload)
        handle.flush()
        os.fsync(handle.fileno())
    return {**file_artifact(path), "event_count": len(events), "status": "captured"}


def summarize_notification_delivery(events: list[dict[str, Any]]) -> dict[str, Any]:
    counts = {
        state: sum(1 for event in events if event.get("state") == state)
        for state in REPLAY_DELIVERY_EVENT_STATES
    }
    requested = counts["notification_requested"]
    accepted = counts["notification_accepted"]
    delayed = counts["notification_delayed"]
    dropped = counts["notification_dropped"]
    skipped = counts["notification_skipped"]
    terminal_outcomes = accepted + dropped + skipped
    terminal_states = {
        "notification_accepted",
        "notification_dropped",
        "notification_skipped",
    }
    sequences: dict[int, dict[str, int | str | bool]] = {}
    invalid_identity_events = 0
    malformed_event_count = 0
    for event in events:
        sequence = event.get("globalTxSequence")
        if isinstance(sequence, bool) or not isinstance(sequence, int) or sequence <= 0:
            invalid_identity_events += 1
            continue

        track = sequences.setdefault(
            sequence,
            {
                "requested": 0,
                "terminal": 0,
                "phase": "new",
                "malformed": False,
            },
        )
        state = event.get("state")
        if state == "notification_requested":
            track["requested"] = int(track["requested"]) + 1
            if track["phase"] != "new":
                track["malformed"] = True
                malformed_event_count += 1
            else:
                track["phase"] = "requested"
        elif state == "notification_delayed":
            if track["phase"] != "requested":
                track["malformed"] = True
                malformed_event_count += 1
        elif state in terminal_states:
            track["terminal"] = int(track["terminal"]) + 1
            if track["phase"] != "requested":
                track["malformed"] = True
                malformed_event_count += 1
            else:
                track["phase"] = "terminal"

    unresolved = sum(
        1
        for track in sequences.values()
        if int(track["requested"]) > 0 and int(track["terminal"]) == 0
    )
    for track in sequences.values():
        if int(track["requested"]) != 1 or int(track["terminal"]) != 1:
            track["malformed"] = True
    malformed_sequences = sum(
        1 for track in sequences.values() if bool(track["malformed"])
    )
    completed_sequences = sum(
        1 for track in sequences.values() if not bool(track["malformed"])
    )
    loss_events = dropped + skipped
    return {
        "events": sum(counts.values()),
        "requested": requested,
        # An accepted or delayed CoreBluetooth updateValue call is one host-stack attempt.
        "attempted": accepted + delayed,
        "delivered": accepted,
        "delivered_meaning": "accepted_by_CoreBluetooth_not_DUT_receipt",
        "delayed_attempts": delayed,
        "dropped": dropped,
        "skipped": skipped,
        "terminal_outcomes": terminal_outcomes,
        "unresolved": unresolved,
        "identified_sequences": len(sequences),
        "completed_sequences": completed_sequences,
        "malformed_sequences": malformed_sequences,
        "malformed_events": malformed_event_count,
        "invalid_identity_events": invalid_identity_events,
        "loss_events": loss_events,
        "complete": (
            requested > 0
            and completed_sequences > 0
            and loss_events == 0
            and unresolved == 0
            and malformed_sequences == 0
            and invalid_identity_events == 0
        ),
    }


def notification_delivery_problem(
    summary: object, *, required: bool = False
) -> str:
    if not isinstance(summary, dict):
        return "notification delivery summary is missing" if required else ""
    fields = (
        "events",
        "requested",
        "attempted",
        "delivered",
        "delayed_attempts",
        "dropped",
        "skipped",
        "terminal_outcomes",
        "unresolved",
        "identified_sequences",
        "completed_sequences",
        "malformed_sequences",
        "malformed_events",
        "invalid_identity_events",
    )
    if any(not isinstance(summary.get(field), int) or summary[field] < 0 for field in fields):
        return "notification delivery summary has invalid counters"
    dropped = summary["dropped"]
    skipped = summary["skipped"]
    unresolved = summary["unresolved"]
    malformed_sequences = summary["malformed_sequences"]
    malformed_events = summary["malformed_events"]
    invalid_identity_events = summary["invalid_identity_events"]
    loss_events = summary.get("loss_events")
    if loss_events != dropped + skipped:
        return "notification delivery loss counter is inconsistent"
    if summary["terminal_outcomes"] != summary["delivered"] + dropped + skipped:
        return "notification delivery terminal counter is inconsistent"
    if summary["attempted"] != summary["delivered"] + summary["delayed_attempts"]:
        return "notification delivery attempt counter is inconsistent"
    if summary["events"] != (
        summary["requested"]
        + summary["delayed_attempts"]
        + summary["terminal_outcomes"]
    ):
        return "notification delivery event counter is inconsistent"
    if summary["completed_sequences"] + malformed_sequences != summary["identified_sequences"]:
        return "notification delivery sequence counters are inconsistent"
    if malformed_sequences or malformed_events or invalid_identity_events:
        return (
            "notification delivery sequence lifecycle was malformed: "
            f"identified={summary['identified_sequences']} "
            f"completed={summary['completed_sequences']} "
            f"malformed_sequences={malformed_sequences} "
            f"malformed_events={malformed_events} "
            f"invalid_identity_events={invalid_identity_events}"
        )
    if summary["requested"] == 0:
        return (
            "notification delivery evidence is empty: requested=0"
            if required
            else ""
        )
    if dropped or skipped or unresolved:
        return (
            "notification delivery was incomplete: "
            f"requested={summary['requested']} attempted={summary['attempted']} "
            f"delivered={summary['delivered']} dropped={dropped} "
            f"skipped={skipped} unresolved={unresolved}"
        )
    if summary.get("complete") is not True:
        return "notification delivery summary is not complete"
    return ""


def install_signal_handlers() -> None:
    handled = False

    def interrupt(signum: int, _frame: Any) -> None:
        nonlocal handled
        if handled:
            return
        handled = True
        for managed_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(managed_signal, signal.SIG_IGN)
        raise InterruptedError(f"received signal {signum}")

    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, interrupt)


def detect_port() -> str:
    candidates: list[str] = []
    for pattern in (
        "/dev/cu.usbmodem*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
        "/dev/cu.usbserial*",
        "/dev/tty.usbserial*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/tty.SLAB_USBtoUART*",
    ):
        candidates.extend(glob.glob(pattern))
    return sorted(dict.fromkeys(candidates))[0] if candidates else ""


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
    command = [str(BUILD_SH), "-f", "-u"]
    if skip_web:
        command.append("--skip-web")
    if port:
        command.extend(["--upload-port", port])
    subprocess.run(command, cwd=ROOT, check=True)


class BenchSerial:
    """Read-only serial continuity observer; it never sends firmware commands."""

    def __init__(self, port: str, baud: int, log_path: Path, timeline: BenchTimeline):
        if serial is None:
            raise RuntimeError("pyserial is required for live bench collection")
        self.log_path = log_path
        self.log = log_path.open("x", encoding="utf-8")
        self.ser = serial.Serial()  # type: ignore[union-attr]
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.25
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.reset_input_buffer()
        self.timeline = timeline
        self.identity_tracker = RuntimeIdentityTracker()
        self.line_count = 0

    @property
    def boot_marker_count(self) -> int:
        return self.identity_tracker.boot_marker_count

    @property
    def runtime_identity(self) -> dict[str, Any] | None:
        return self.identity_tracker.identity

    def read_line(self, timeout_s: float = 0.25) -> str:
        self.ser.timeout = timeout_s
        raw = self.ser.readline()
        if not raw:
            return ""
        text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        safe = redact_artifact_text(text)
        self.log.write(safe + "\n")
        self.log.flush()
        self.line_count += 1
        self.timeline.record("serial_receive", line=safe)
        self.identity_tracker.observe(text)
        return text

    def close(self) -> None:
        try:
            if self.ser.is_open:
                self.ser.close()
        finally:
            self.log.close()


def establish_serial_boundary(
    observer: BenchSerial,
    ready_timeout_s: float,
    *,
    monotonic: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    """Keep an attach-time boot outside the external evidence window."""
    if ready_timeout_s <= 0:
        raise ValueError("serial readiness timeout must be positive")

    started = monotonic()
    readiness_deadline = started + ready_timeout_s
    initial_boot_markers = observer.boot_marker_count
    startup_detected = False

    while True:
        if observer.runtime_identity is not None:
            mode = "startup_completed" if startup_detected else "identity_observed"
            break

        now = monotonic()
        if now >= readiness_deadline:
            raise RuntimeIdentityFailure(
                "runtime BOOT identity was not observed before the external evidence window"
            )

        line = observer.read_line(min(0.25, readiness_deadline - now))
        if line.startswith(BOOT_START_PREFIXES):
            startup_detected = True

    result = {
        "mode": mode,
        "startup_detected": startup_detected,
        "boot_markers_observed": observer.boot_marker_count - initial_boot_markers,
        "runtime_identity": observer.runtime_identity,
        "duration_seconds": max(0.0, monotonic() - started),
    }
    observer.timeline.record("serial_boundary_established", **result)
    return result


class V1Emulator:
    """Own one managed external V1 input source for the complete host window."""

    def __init__(
        self,
        executable: Path,
        out_dir: Path,
        suite: str,
        blink_profile: str,
        *,
        lease_fd: int,
        scenario: str,
        machine_event: Callable[[dict[str, Any]], None],
    ) -> None:
        self.executable = executable
        self.suite = suite
        self.mode = "bench" if suite == "replay" else "idle"
        self.blink_profile = blink_profile
        self.lease_fd = lease_fd
        self.scenario = scenario
        self.machine_event = machine_event
        self.log_path = out_dir / "v1replay.log"
        self.scenario_path = (
            out_dir / REPLAY_SCENARIO_EVIDENCE_NAME if self.mode == "bench" else None
        )
        self.process: subprocess.Popen[bytes] | None = None
        self.log_handle: Any = None
        self.observed_events = 0

    def _events(self, *, strict: bool = False) -> list[dict[str, Any]]:
        try:
            lines = self.log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return []
        events: list[dict[str, Any]] = []
        prefix = "V1REPLAY_EVENT "
        for line_number, line in enumerate(lines, 1):
            marker = line.find(prefix)
            if marker < 0:
                continue
            try:
                event = json.loads(line[marker + len(prefix) :])
            except json.JSONDecodeError as exc:
                if strict:
                    raise RuntimeError(
                        f"malformed V1 emulator event at log line {line_number}"
                    ) from exc
                continue
            if isinstance(event, dict) and isinstance(event.get("state"), str):
                events.append(event)
            elif strict:
                raise RuntimeError(f"invalid V1 emulator event at log line {line_number}")
        return events

    def _observe_events(self) -> None:
        events = self._events()
        for event in events[self.observed_events :]:
            self.machine_event(dict(event))
            self.observed_events += 1

    def start(self) -> None:
        if not self.executable.is_file() or not os.access(self.executable, os.X_OK):
            raise RuntimeError("v1replay executable is missing or not executable")
        if self.log_path.exists() or (self.scenario_path and self.scenario_path.exists()):
            raise RuntimeError("refusing to overwrite existing replay evidence")
        self.log_handle = self.log_path.open("xb")
        command = [str(self.executable), self.mode]
        if self.mode == "bench":
            if self.scenario:
                command.extend(["--scenario", self.scenario])
            assert self.scenario_path is not None
            command.extend(["--scenario-evidence", str(self.scenario_path)])
        command.extend(
            [
                "--machine-events",
                "--owner-pid",
                str(os.getpid()),
                "--blink-profile",
                self.blink_profile,
            ]
        )
        self.process = subprocess.Popen(
            command,
            cwd=self.executable.parent.parent,
            stdout=self.log_handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            pass_fds=(self.lease_fd,),
        )

    def health_problem(self) -> str:
        self._observe_events()
        if self.process is None:
            return "V1 emulator did not start"
        code = self.process.poll()
        return "" if code is None else f"V1 emulator exited early with code {code}"

    def wait_for_transport(self, timeout_s: float) -> None:
        if self.mode != "idle":
            return
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            problem = self.health_problem()
            if problem:
                raise RuntimeError(problem)
            if any(
                event.get("state") == "session_transport" and event.get("active") is True
                for event in self._events()
            ):
                return
            time.sleep(0.05)
        raise RuntimeError("managed V1 emulator did not establish its input transport")

    def finish(self, window_completed: bool) -> dict[str, Any]:
        process_was_running = self.process is not None and self.process.poll() is None
        if process_was_running:
            assert self.process is not None
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=2)
        if self.log_handle is not None:
            self.log_handle.close()
            self.log_handle = None
        self._observe_events()
        events = self._events(strict=True)
        states = [event["state"] for event in events]
        replay_complete = self.mode != "bench" or "complete" in states
        stopped = bool(states and states[-1] == "stopped")
        returncode = self.process.poll() if self.process is not None else None
        lifecycle_completed = bool(
            window_completed
            and process_was_running
            and replay_complete
            and stopped
            and returncode == 0
        )
        stimulus_events = [
            event for event in events if event.get("state") == REPLAY_STIMULUS_EVENT_STATE
        ]
        delivery_events = [
            event for event in events if event.get("state") in REPLAY_DELIVERY_EVENT_STATES
        ]
        notification_delivery = summarize_notification_delivery(delivery_events)
        completed = lifecycle_completed and (
            self.mode != "bench" or notification_delivery["complete"]
        )
        raw = self.log_path.read_text(encoding="utf-8", errors="replace")
        safe = sanitize_artifact_value(raw, run_dir=self.log_path.parent)
        if safe != raw:
            self.log_path.write_text(safe, encoding="utf-8")
        return {
            "started": self.process is not None,
            "completed": completed,
            "lifecycle_completed": lifecycle_completed,
            "mode": self.mode,
            "blink_profile": self.blink_profile,
            "managed_stop": process_was_running,
            "graceful_stop_confirmed": stopped and returncode == 0,
            "returncode": returncode,
            "log": self.log_path.name,
            "scenario_evidence": self.scenario_path.name if self.scenario_path else "",
            "stimulus_events": stimulus_events,
            "delivery_events": delivery_events,
            "notification_delivery": notification_delivery,
        }


def _finish_camera(
    camera: CameraCapture | None,
    *,
    collection_completed: bool,
    suite: str,
) -> dict[str, Any]:
    if camera is None:
        return {}
    result = camera.stop(collection_completed)
    try:
        result = json.loads(camera.result_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        pass
    if result.get("result") == "CAPTURED":
        manifest = build_capture_manifest(
            camera_dir=camera.out_dir, camera_result=result, suite=suite
        )
        manifest_path, _created = publish_capture_manifest(camera.out_dir, manifest)
        result.update(
            {
                "capture_manifest": manifest_path.name,
                "capture_id": manifest["capture_id"],
                "preflight": camera.preflight_result_path.name,
                "preflight_result": "PASS",
            }
        )
    return result


def collect_live(
    args: argparse.Namespace,
    out_dir: Path,
    artifacts: dict[str, Any],
) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    reserved = [
        out_dir / "bench_serial.log",
        out_dir / BENCH_TIMELINE_NAME,
        out_dir / BUILD_UPLOAD_ARTIFACTS_NAME,
        out_dir / "window_result.json",
        out_dir / "v1replay.log",
        out_dir / REPLAY_STIMULUS_NAME,
        out_dir / REPLAY_DELIVERY_NAME,
        out_dir / REPLAY_SCENARIO_EVIDENCE_NAME,
    ]
    if args.camera:
        reserved.append(out_dir / "camera")
    existing = [path.name for path in reserved if path.exists()]
    if existing:
        raise RuntimeError("refusing to reuse existing live evidence: " + ", ".join(existing))

    with V1RadioLease() as lease:
        assert lease.fd is not None
        port = wait_for_port(args.port)
        if args.upload:
            run_upload(port, args.skip_web)
            artifacts["build_upload"] = retain_build_upload_artifacts(
                out_dir, upload_performed=True
            )
            port = wait_for_port(port, 30)
            deadline = time.monotonic() + args.post_upload_settle_seconds
            while time.monotonic() < deadline:
                time.sleep(min(1.0, deadline - time.monotonic()))
        else:
            artifacts["build_upload"] = retain_build_upload_artifacts(out_dir)

        timeline = BenchTimeline(out_dir / BENCH_TIMELINE_NAME)
        observer: BenchSerial | None = None
        camera = CameraCapture(out_dir / "camera", args.duration_seconds) if args.camera else None
        if camera is not None:
            camera.timeline_event = lambda payload: timeline.record_external(
                payload, "camera_recorder"
            )
        emulator = V1Emulator(
            Path(args.replay_executable).resolve(),
            out_dir,
            args.suite,
            args.blink_profile,
            lease_fd=lease.fd,
            scenario=args.scenario,
            machine_event=lambda payload: timeline.record_external(payload, "v1replay"),
        )
        emulator_result: dict[str, Any] = {}
        camera_result: dict[str, Any] = {}
        collection_completed = False
        completion: dict[str, Any] = {}
        try:
            if camera is not None:
                preflight = run_camera_preflight(camera)
                if preflight.get("result") != "PASS":
                    try:
                        camera_result = json.loads(
                            camera.result_path.read_text(encoding="utf-8")
                        )
                    except (OSError, json.JSONDecodeError):
                        camera_result = {"result": "CAPTURE_FAILED"}
                    camera_result.update(
                        {
                            "preflight_result": preflight.get("result"),
                            "preflight_diagnostics": preflight.get("diagnostics") or [],
                        }
                    )
                    raise CameraPreflightFailure(preflight, camera_result)

            observer = BenchSerial(port, args.baud, out_dir / "bench_serial.log", timeline)
            establish_serial_boundary(observer, args.ready_timeout_seconds)
            assert observer.runtime_identity is not None
            runtime_qualification = qualify_runtime_identity(
                observer.runtime_identity,
                intended_git_sha=args.git_sha,
                build_upload=artifacts["build_upload"],
                upload=args.upload,
            )
            initial_boot_markers = observer.boot_marker_count

            emulator.start()
            emulator.wait_for_transport(args.ready_timeout_seconds)
            started = time.monotonic()
            timeline.record("external_window_started", duration_seconds=args.duration_seconds)
            next_progress = started + RUN_PROGRESS_INTERVAL_S
            while True:
                now = time.monotonic()
                if now - started >= args.duration_seconds:
                    break
                observer.read_line(min(0.25, args.duration_seconds - (now - started)))
                problem = emulator.health_problem()
                if problem:
                    raise RuntimeError(problem)
                if camera is not None:
                    problem = camera.health_problem()
                    if problem:
                        raise CameraEvidenceFailure(problem, camera)
                if observer.boot_marker_count != initial_boot_markers:
                    raise RuntimeError("board rebooted during the external evidence window")
                if now >= next_progress:
                    print(
                        f"[bench] external window {int(now - started)}/{args.duration_seconds}s",
                        flush=True,
                    )
                    next_progress += RUN_PROGRESS_INTERVAL_S
            collection_completed = True
            completion = {
                "source": "external_only",
                "duration_seconds": args.duration_seconds,
                "serial_lines_observed": observer.line_count,
                "boot_markers_during_window": (
                    observer.boot_marker_count - initial_boot_markers
                ),
                "serial_session_continuous": True,
                "process_session_continuous": True,
                "runtime_identity_continuous": True,
            }
            timeline.record("external_window_completed", **completion)
        finally:
            primary_error = sys.exc_info()[1]
            cleanup_errors: list[Exception] = []
            try:
                emulator_result = emulator.finish(collection_completed)
            except Exception as exc:  # noqa: BLE001
                cleanup_errors.append(exc)
            if observer is not None:
                try:
                    observer.close()
                except Exception as exc:  # noqa: BLE001
                    cleanup_errors.append(exc)
            try:
                camera_result = _finish_camera(
                    camera, collection_completed=collection_completed, suite=args.suite
                )
            except Exception as exc:  # noqa: BLE001
                cleanup_errors.append(exc)
            timeline.close()
            artifacts["bench_timeline"] = file_artifact(timeline.path)
            serial_path = out_dir / "bench_serial.log"
            if serial_path.is_file():
                artifacts["bench_serial"] = file_artifact(serial_path)
            replay_path = out_dir / "v1replay.log"
            if replay_path.is_file():
                artifacts["v1replay"] = file_artifact(replay_path)
            if emulator_result:
                try:
                    stimulus = publish_replay_stimulus_evidence(
                        emulator_result, out_dir, suite=args.suite
                    )
                    if stimulus is not None:
                        artifacts["replay_stimulus"] = stimulus
                    delivery = publish_replay_delivery_evidence(
                        emulator_result, out_dir, suite=args.suite
                    )
                    if delivery is not None:
                        artifacts["replay_delivery"] = delivery
                except Exception as exc:  # noqa: BLE001
                    cleanup_errors.append(exc)
            if cleanup_errors:
                detail = "; ".join(str(error) for error in cleanup_errors)
                if primary_error is not None:
                    primary_error.args = (f"{primary_error}; cleanup failure: {detail}",)
                else:
                    raise RuntimeError(f"cleanup failure: {detail}") from cleanup_errors[0]

        if not emulator_result.get("lifecycle_completed"):
            raise RuntimeError("managed V1 input did not cover the complete external window")
        if camera is not None and camera_result.get("result") != "CAPTURED":
            raise CameraEvidenceFailure(
                "camera leg did not retain complete raw evidence", camera
            )
        return {
            "port": port,
            "completion": completion,
            "emulator": emulator_result,
            "camera": camera_result,
            "runtime_identity": observer.runtime_identity,
            "runtime_qualification": runtime_qualification,
        }


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
    artifacts: dict[str, Any] = {}

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
    if args.ready_timeout_seconds < 1:
        return fail("readiness timeout must be positive")
    if args.post_upload_settle_seconds < 0:
        return fail("post-upload settle duration cannot be negative")
    if args.suite != "replay" and args.scenario:
        return fail("--scenario is valid only for replay")
    if args.git_worktree_clean != "1":
        return fail(
            "source worktree is dirty; qualification requires an exact clean source state",
            result="FAIL",
            failure_kind="source_provenance",
            git_worktree_clean=False,
            runtime_qualification={
                "status": "unqualified",
                "reason": "source_worktree_dirty",
            },
        )
    if not args.replay_executable:
        return fail("managed v1replay is required for live collection")
    if serial is None:
        return fail("pyserial is required for live collection")

    try:
        result = collect_live(args, out_dir, artifacts)
        qualification = result["runtime_qualification"]
        delivery_summary = result["emulator"].get("notification_delivery")
        delivery_problem = notification_delivery_problem(
            delivery_summary,
            required=args.suite == "replay",
        )
        if delivery_problem:
            verdict = "FAIL"
        else:
            verdict = (
                "PASS"
                if qualification.get("status") == "qualified"
                else "COLLECTION_ONLY"
            )
        write_window_result(
            out_dir,
            {
                "result": verdict,
                "evidence_contract": "external_only",
                "suite": args.suite,
                "duration_seconds": args.duration_seconds,
                "board_id": args.board_id,
                "git_sha": args.git_sha,
                "git_ref": args.git_ref,
                "git_worktree_clean": args.git_worktree_clean == "1",
                "device_port": redact_artifact_text(result["port"]),
                "completion": result["completion"],
                "emulator": result["emulator"],
                "camera": result["camera"],
                "runtime_identity": result["runtime_identity"],
                "runtime_qualification": qualification,
                "artifacts": artifacts,
                **(
                    {
                        "failure_kind": "replay_delivery",
                        "qualification_reason": delivery_problem,
                    }
                    if verdict == "FAIL"
                    else {}
                ),
                **(
                    {"qualification_reason": qualification.get("reason", "unqualified")}
                    if verdict == "COLLECTION_ONLY"
                    else {}
                ),
            },
        )
        if verdict == "COLLECTION_ONLY":
            print(
                f"[bench] collection_only: {qualification.get('reason', 'unqualified')}",
                file=sys.stderr,
                flush=True,
            )
            return 1
        if verdict == "FAIL":
            print(f"[bench] fail: {delivery_problem}", file=sys.stderr, flush=True)
            return 2
        return 0
    except RuntimeIdentityFailure as exc:
        return fail(
            str(exc),
            failure_kind="runtime_identity",
            runtime_identity=exc.identity,
            runtime_qualification=exc.qualification,
        )
    except CameraPreflightFailure as exc:
        return fail(
            str(exc),
            result="FAIL",
            camera=exc.camera_result,
            failure_kind="camera_preflight",
        )
    except CameraEvidenceFailure as exc:
        try:
            camera_result = json.loads(exc.camera.result_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            camera_result = {"result": "CAPTURE_FAILED", "errors": list(exc.camera.errors)}
        return fail(str(exc), camera=camera_result, failure_kind="camera_evidence")
    except (InterruptedError, KeyboardInterrupt) as exc:
        write_window_result(
            out_dir,
            {
                "result": "INTERRUPTED",
                "suite": args.suite,
                "board_id": args.board_id,
                "artifacts": artifacts,
                "error": redact_artifact_text(str(exc) or "interrupted"),
            },
        )
        return 130
    except Exception as exc:  # noqa: BLE001
        return fail(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())
