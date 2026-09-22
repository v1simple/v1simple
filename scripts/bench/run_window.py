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
import urllib.parse
import urllib.request
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
PRODUCTION_PIO_ENV = "waveshare-349"
BUILD_OUTPUT_DIR = ROOT / ".pio" / "build" / PRODUCTION_PIO_ENV
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
PRESENTATION_SNAPSHOT_NAME = "presentation_snapshot.json"
REPLAY_DISPLAY_CONTRACT_NAME = "replay_display_contract.json"
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
AUTO_PUSH_SELECTION_RE = re.compile(
    r"\[AutoPush\] onV1Connected autoPush=(on|off) activeSlot=([0-2]) "
    r"selectedSlot=([0-2]) defaultProfile=([0-3]) mode=([0-3])"
)
PRESENTATION_HTTP_TIMEOUT_S = 5.0
PRESENTATION_HTTP_MAX_BYTES = 16 * 1024
PRESENTATION_CONNECT_TIMEOUT_S = 120.0

DISPLAY_DIRECT_FIELDS = (
    "bogey", "freq", "arrowFront", "arrowSide", "arrowRear", "bandL", "bandKa",
    "bandK", "bandX", "bandPhoto", "wifiConnected", "bleConnected", "bleDisconnected",
    "bar1", "bar2", "bar3", "bar4", "bar5", "bar6", "muted", "persisted",
    "volumeMain", "volumeMute", "rssiV1", "rssiProxy", "obd", "alpConnected", "alpDli",
    "alpLidActive", "alpAlert", "brightness",
    "freqUseBandColor", "hideWifiIcon", "hideProfileIndicator", "hideBatteryIcon",
    "showBatteryPercent", "hideBleIcon", "hideVolumeIndicator", "hideRssiIndicator",
)
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


class SourceProvenanceFailure(RuntimeError):
    def __init__(self, message: str, *, reason: str) -> None:
        super().__init__(message)
        self.reason = reason


class PresentationConnectionFailure(RuntimeError):
    pass


def _git_output(repo: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repo), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise SourceProvenanceFailure(
            "could not inspect the source repository",
            reason="source_repository_uninspectable",
        )
    return completed.stdout.strip()


def require_current_source_identity(
    *, expected_git_sha: str, expected_git_ref: str, repo: Path = ROOT
) -> None:
    current_git_sha = _git_output(repo, "rev-parse", "HEAD")
    current_git_ref = _git_output(repo, "rev-parse", "--abbrev-ref", "HEAD")
    status = _git_output(
        repo,
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
        "--ignore-submodules=none",
    )
    if current_git_sha != expected_git_sha or current_git_ref != expected_git_ref:
        raise SourceProvenanceFailure(
            "source identity changed during raw collection",
            reason="source_identity_changed",
        )
    if status:
        raise SourceProvenanceFailure(
            "source worktree changed during raw collection",
            reason="source_worktree_dirty",
        )


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
    parser.add_argument("--skip-web", action="store_true")
    parser.add_argument("--post-upload-settle-seconds", type=int, default=90)
    parser.add_argument("--replay-executable", default="")
    parser.add_argument("--scenario", default="")
    parser.add_argument(
        "--presentation-api-base-url",
        default=os.environ.get("BENCH_PRESENTATION_API_BASE_URL", ""),
        help="optional replay-only HTTP base URL used for bound presentation evidence",
    )
    parser.add_argument("--ku-qualification", action="store_true")
    parser.add_argument("--photo-label-qualification", action="store_true")
    parser.add_argument("--junk-qualification", action="store_true")
    parser.add_argument(
        "--blink-profile", choices=["scenario", "steady", "stress"], default=None
    )
    parser.add_argument("--camera", action="store_true")
    parser.add_argument("--ready-timeout-seconds", type=int, default=45)
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


def _canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")


def _sha256_value(value: Any) -> str:
    return hashlib.sha256(_canonical_json_bytes(value)).hexdigest()


def normalize_presentation_api_base_url(raw: str) -> str:
    value = raw.strip()
    if not value:
        return ""
    parsed = urllib.parse.urlsplit(value)
    if (
        parsed.scheme != "http"
        or not parsed.hostname
        or parsed.username is not None or parsed.password is not None
        or parsed.query or parsed.fragment
        or parsed.path not in ("", "/")
    ):
        raise ValueError("presentation API base URL must be an HTTP origin without credentials or a path")
    return value.rstrip("/")


def _http_get_json(base_url: str, endpoint: str, *, query: dict[str, str] | None = None,
                   max_bytes: int = PRESENTATION_HTTP_MAX_BYTES,
                   timeout_s: float = PRESENTATION_HTTP_TIMEOUT_S) -> dict[str, Any]:
    url = base_url + endpoint
    if query: url += "?" + urllib.parse.urlencode(query)
    try:
        request = urllib.request.Request(url, headers={"Accept": "application/json"})
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(request, timeout=timeout_s) as response:
            body = response.read(max_bytes + 1)
    except OSError as exc:
        raise PresentationConnectionFailure(
            f"presentation API request failed for {endpoint}"
        ) from exc
    if len(body) > max_bytes:
        raise RuntimeError(f"presentation API {endpoint} response exceeds its bound")
    try:
        value = json.loads(body.decode("utf-8"))
    except (UnicodeError, ValueError) as exc:
        raise RuntimeError(f"presentation API {endpoint} did not return JSON") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"presentation API {endpoint} response is not an object")
    return value


def capture_presentation_configuration(
    base_url: str,
    *,
    get_json: Callable[..., dict[str, Any]] = _http_get_json,
    timeout_s: float | None = None,
    monotonic: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    deadline = monotonic() + timeout_s if timeout_s is not None else None

    def fetch(endpoint: str) -> dict[str, Any]:
        if deadline is None:
            return get_json(base_url, endpoint)
        remaining = deadline - monotonic()
        if remaining <= 0:
            raise PresentationConnectionFailure("presentation API deadline expired")
        return get_json(
            base_url,
            endpoint,
            timeout_s=min(PRESENTATION_HTTP_TIMEOUT_S, remaining),
        )

    display_payload = fetch("/api/display/settings")
    if not set(DISPLAY_DIRECT_FIELDS).issubset(display_payload):
        raise RuntimeError("display settings response is incomplete")
    display = {field: display_payload[field] for field in DISPLAY_DIRECT_FIELDS}
    quiet = fetch("/api/quiet/settings")
    if not quiet or any(not isinstance(value, (bool, int)) for value in quiet.values()):
        raise RuntimeError("quiet settings response is invalid")
    slots_payload = fetch("/api/autopush/slots")
    enabled = slots_payload.get("enabled")
    active_slot = slots_payload.get("activeSlot")
    raw_slots = slots_payload.get("slots")
    if (type(enabled) is not bool or type(active_slot) is not int or
            active_slot not in range(3) or not isinstance(raw_slots, list) or
            len(raw_slots) != 3):
        raise RuntimeError("Auto-Push slots response is incomplete")
    if slots_payload.get("schemaVersion") != 3 or \
            slots_payload.get("detectorConfigurationOwner") != "profile":
        raise RuntimeError("Auto-Push slots do not use the current profile-owned schema")

    canonical_slots: list[dict[str, Any]] = []
    for index, slot in enumerate(raw_slots):
        if not isinstance(slot, dict):
            raise RuntimeError("Auto-Push slot is not an object")
        slot_name = slot.get("name")
        profile_name = slot.get("profile")
        color = slot.get("color")
        persistence = slot.get("alertPersist")
        priority_only = slot.get("priorityArrowOnly")
        if (not isinstance(slot_name, str) or not slot_name or
                not isinstance(profile_name, str) or not profile_name or
                type(color) is not int or not 0 <= color <= 0xFFFF or
                type(persistence) is not int or not 0 <= persistence <= 5 or
                type(priority_only) is not bool):
            raise RuntimeError("Auto-Push slot response is invalid")
        display_label = privacy_safe_identifier(slot_name, namespace="slot-label")
        canonical_slots.append(
            {
                "slot": index,
                "display_label": display_label,
                "display_label_is_opaque": display_label.startswith("private-slot-label-"),
                "color": color,
                "alert_persistence_seconds": persistence,
                "priority_arrow_only": priority_only,
            }
        )
    return {
        "display": display,
        "quiet": quiet,
        "auto_push": {
            "enabled": enabled,
            "configured_active_slot": active_slot,
            "profile_schema_version": 3,
            "detector_configuration_owner": "profile",
            "slots": canonical_slots,
        },
    }


def wait_for_presentation_configuration(
    base_url: str,
    *,
    timeout_s: float = PRESENTATION_CONNECT_TIMEOUT_S,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
    capture: Callable[..., dict[str, Any]] = capture_presentation_configuration,
    reconnect: Callable[[float], bool] | None = None,
) -> dict[str, Any]:
    deadline = monotonic() + timeout_s
    next_progress = monotonic()
    next_reconnect = monotonic()
    last_error = PresentationConnectionFailure("maintenance HTTP was not reached")
    while True:
        now = monotonic()
        if now >= deadline:
            raise RuntimeError(
                "maintenance HTTP was not reachable; join the V1-Simple WiFi network"
            ) from last_error
        if reconnect is not None and now >= next_reconnect:
            reconnect(deadline - now)
            now = monotonic()
            next_reconnect = now + 15.0
            if now >= deadline:
                raise RuntimeError(
                    "maintenance HTTP was not reachable; join the V1-Simple WiFi network"
                ) from last_error
        try:
            configuration = capture(base_url, timeout_s=deadline - now)
            if monotonic() > deadline:
                raise PresentationConnectionFailure("presentation API deadline expired")
            return configuration
        except PresentationConnectionFailure as exc:
            last_error = exc
            now = monotonic()
            if now >= deadline:
                raise RuntimeError(
                    "maintenance HTTP was not reachable; join the V1-Simple WiFi network"
                ) from exc
            if now >= next_progress:
                remaining = max(0, int(deadline - now))
                print(
                    f"[bench] join V1-Simple WiFi; waiting for maintenance HTTP ({remaining}s remaining)",
                    flush=True,
                )
                next_progress = now + 15.0
            sleep(min(1.0, max(0.0, deadline - now)))


def try_join_maintenance_wifi(
    *,
    ssid: str = "V1-Simple",
    run: Callable[..., Any] | None = None,
    timeout_s: float = 30.0,
    monotonic: Callable[[], float] = time.monotonic,
) -> bool:
    execute = run or subprocess.run
    deadline = monotonic() + max(0.0, timeout_s)

    def bounded_execute(command: list[str], command_timeout_s: float) -> Any | None:
        remaining = deadline - monotonic()
        if remaining <= 0:
            return None
        try:
            return execute(
                command,
                check=False,
                capture_output=True,
                text=True,
                timeout=min(command_timeout_s, remaining),
            )
        except (OSError, subprocess.SubprocessError):
            return None

    inventory = bounded_execute(
        ["/usr/sbin/networksetup", "-listallhardwareports"], 10.0
    )
    if inventory is None:
        return False
    if inventory.returncode != 0:
        return False
    wifi_device = ""
    for block in inventory.stdout.split("\n\n"):
        fields = dict(
            line.split(":", 1) for line in block.splitlines() if ":" in line
        )
        if fields.get("Hardware Port", "").strip() in ("Wi-Fi", "AirPort"):
            wifi_device = fields.get("Device", "").strip()
            break
    if not wifi_device:
        return False

    def joined_expected_network() -> bool:
        current = bounded_execute(
            ["/usr/sbin/networksetup", "-getairportnetwork", wifi_device], 10.0
        )
        if current is None:
            return False
        return current.returncode == 0 and current.stdout.strip().endswith(f": {ssid}")

    if joined_expected_network():
        return True
    passwords = [None]
    configured = os.environ.get("BENCH_MAINTENANCE_WIFI_PASSWORD", "")
    passwords.append(configured or "setupv1simple")
    for password in passwords:
        command = ["/usr/sbin/networksetup", "-setairportnetwork", wifi_device, ssid]
        if password is not None:
            command.append(password)
        joined = bounded_execute(command, 20.0)
        if joined is None:
            continue
        if joined.returncode == 0 and joined_expected_network():
            return True
    return False


def parse_auto_push_selection(line: str) -> dict[str, Any] | None:
    match = AUTO_PUSH_SELECTION_RE.fullmatch(line)
    if match is None:
        return None
    enabled = match.group(1) == "on"
    active_slot = int(match.group(2), 10)
    selected_slot = int(match.group(3), 10)
    default_profile = int(match.group(4), 10)
    mode = int(match.group(5), 10)
    if (default_profile == 0 and selected_slot != active_slot) or (
        default_profile != 0 and selected_slot != default_profile - 1
    ):
        raise RuntimeError("Auto-Push selected-slot evidence is internally inconsistent")
    return {
        "auto_push_enabled": enabled,
        "configured_active_slot": active_slot,
        "intended_detector_profile_slot": selected_slot if enabled else None,
        "intended_profile_indicator_slot": selected_slot if enabled else None,
        "presentation_policy_slot": active_slot,
        "device_default_profile": default_profile,
        "mode": mode,
    }


def publish_presentation_snapshot(
    out_dir: Path,
    *,
    configuration: dict[str, Any],
    selection: dict[str, Any],
    maintenance_capture_ns: int,
) -> dict[str, Any]:
    configured = configuration["auto_push"]
    if selection["auto_push_enabled"] != configured["enabled"]:
        raise RuntimeError("Auto-Push enablement drifted between HTTP and serial evidence")
    if selection["configured_active_slot"] != configured["configured_active_slot"]:
        raise RuntimeError("configured Auto-Push slot drifted between HTTP and serial evidence")
    payload = sanitize_artifact_value(
        {
            "schema_version": 1,
            "kind": "bench_presentation_snapshot",
            "http_source_mode_declared": "maintenance",
            "http_same_dut_as_normal_runtime_proven": False,
            "serial_port_opened_before_capture": False,
            "captured_before_application_upload": True,
            "normal_runtime_binding": "autopush_enablement_and_slot_selection_intent",
            "full_normal_ram_match_proven": False,
            "capture_host_monotonic_ns": maintenance_capture_ns,
            "configuration_sha256": _sha256_value(configuration),
            "configuration": configuration,
            "runtime_selection": selection,
        },
        run_dir=out_dir,
    )
    path = out_dir / PRESENTATION_SNAPSHOT_NAME
    with path.open("xb") as handle:
        handle.write(_canonical_json_bytes(payload) + b"\n")
        handle.flush()
        os.fsync(handle.fileno())
    return {**file_artifact(path), "status": "captured"}


def publish_replay_display_contract(
    out_dir: Path,
    *,
    artifacts: dict[str, Any],
    runtime_identity: dict[str, Any],
    camera_result: dict[str, Any],
) -> dict[str, Any]:
    required = (
        "presentation_snapshot", "replay_stimulus", "replay_delivery", "replay_scenario",
        "v1_emulator_state", "bench_timeline", "build_upload",
    )
    if any(name not in artifacts for name in required):
        raise RuntimeError("replay display contract inputs are incomplete")

    def verified_artifact(name: str) -> dict[str, Any]:
        metadata = artifacts[name]
        if not isinstance(metadata, dict) or not isinstance(metadata.get("path"), str):
            raise RuntimeError(f"{name} artifact metadata is invalid")
        relative = Path(metadata["path"])
        if relative.name != metadata["path"]:
            raise RuntimeError(f"{name} artifact path is invalid")
        actual = file_artifact(out_dir / relative)
        if any(metadata.get(key) != actual[key] for key in ("sha256", "size_bytes")):
            raise RuntimeError(f"{name} artifact changed before display-contract publication")
        result = dict(actual)
        for key in ("event_count", "status"):
            if key in metadata:
                result[key] = metadata[key]
        return result

    linked = {name: verified_artifact(name) for name in required}
    stimulus_artifact = linked["replay_stimulus"]
    try:
        stimulus_events = [
            json.loads(line) for line in
            (out_dir / stimulus_artifact["path"]).read_text(encoding="utf-8").splitlines()
            if line
        ]
        scenario = json.loads(
            (out_dir / linked["replay_scenario"]["path"]).read_text(encoding="utf-8")
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError("replay display contract input is not valid JSON") from exc
    if (
        not stimulus_events
        or stimulus_artifact.get("event_count") != len(stimulus_events)
        or any(
            not isinstance(event, dict)
            or event.get("state") != REPLAY_STIMULUS_EVENT_STATE
            or event.get("schemaVersion") != 4
            or not isinstance(event.get("expected"), dict)
            or not isinstance(event.get("notifications"), list)
            for event in stimulus_events
        )
    ):
        raise RuntimeError("replay stimulus is not a schema-4 display rubric")
    if not isinstance(scenario, dict) or scenario.get("schemaVersion") != 2 \
            or not isinstance(scenario.get("samples"), list):
        raise RuntimeError("replay scenario is not a schema-2 resolved scenario")
    camera_link: dict[str, Any] | None = None
    manifest_name = camera_result.get("capture_manifest")
    capture_id = camera_result.get("capture_id")
    if manifest_name or capture_id:
        if not isinstance(manifest_name, str) or not manifest_name or not isinstance(capture_id, str) \
                or SHA256_RE.fullmatch(capture_id) is None:
            raise RuntimeError("camera identity is incomplete")
        manifest_path = out_dir / "camera" / Path(manifest_name).name
        if not manifest_path.is_file():
            raise RuntimeError("camera capture manifest is unavailable")
        camera_link = {
            "capture_id": capture_id,
            "manifest": {
                "path": f"camera/{manifest_path.name}",
                "sha256": sha256_file(manifest_path),
                "size_bytes": manifest_path.stat().st_size,
            },
        }
    payload = {
        "schema_version": 1,
        "kind": "bench_replay_display_contract",
        "presentation_snapshot": linked["presentation_snapshot"],
        "expected_display_rubric": stimulus_artifact,
        "notification_delivery": linked["replay_delivery"],
        "scenario": linked["replay_scenario"],
        "terminal_persisted_emulator_state": linked["v1_emulator_state"],
        "timeline": linked["bench_timeline"],
        "runtime": {
            "boot_id": runtime_identity.get("boot_id"),
            "git_sha": runtime_identity.get("git_sha"),
            "image_id": runtime_identity.get("image_id"),
            "build_upload_manifest_sha256": linked["build_upload"]["sha256"],
        },
        "camera": camera_link,
    }
    safe = sanitize_artifact_value(payload, run_dir=out_dir)
    path = out_dir / REPLAY_DISPLAY_CONTRACT_NAME
    with path.open("xb") as handle:
        handle.write(_canonical_json_bytes(safe) + b"\n")
        handle.flush()
        os.fsync(handle.fileno())
    return {**file_artifact(path), "status": "captured"}


class BenchTimeline:
    def __init__(self, path: Path):
        self.path = path
        self.run_dir = path.parent
        self.handle = path.open("x", encoding="utf-8")
        self.record("timeline_opened")

    def record(self, event: str, **fields: Any) -> dict[str, Any]:
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
        return payload

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
    upload_performed: bool,
) -> dict[str, Any]:
    files: list[dict[str, Any]] = []
    missing: list[str] = []
    for name in BUILD_UPLOAD_FILES:
        path = build_dir / name
        if not path.is_file():
            if name == "firmware.bin":
                raise FileNotFoundError("application binary is missing; exact image cannot be retained")
            missing.append(name)
            continue
        artifact = {
            "name": name,
            "size_bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        if name == "firmware.bin":
            if artifact["size_bytes"] == 0:
                raise RuntimeError("application binary is empty; exact image cannot be retained")
            retained = out_dir / name
            with path.open("rb") as source, retained.open("xb") as destination:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    destination.write(chunk)
            copy = file_artifact(retained)
            if any(copy[key] != artifact[key] for key in ("sha256", "size_bytes")):
                raise RuntimeError("application binary changed while retaining its exact image")
            artifact["path"] = copy["path"]
        files.append(artifact)
    elf_sha = next((item["sha256"] for item in files if item["name"] == "firmware.elf"), "")
    payload = {
        "schema_version": 1,
        "kind": "bench_build_upload_artifacts",
        "upload_performed": upload_performed,
        "upload_scope": "platformio_upload_target" if upload_performed else "none",
        "filesystem_upload_requested": False,
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
) -> dict[str, Any]:
    qualification: dict[str, Any] = {
        "status": "unqualified",
        "mode": "upload",
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

    if build_upload.get("upload_performed") is not True:
        raise RuntimeIdentityFailure(
            "artifact manifest does not record the required firmware upload",
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
    command = [str(BUILD_SH), "-u"]
    if skip_web:
        command.append("--skip-web")
    if port:
        command.extend(["--upload-port", port])
    subprocess.run(command, cwd=ROOT, check=True)


def native_usb_reset_strategy(port: Any) -> tuple[Any, dict[str, Any]]:
    """Normal-application reset for ESP32-S3 native USB Serial/JTAG."""
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise RuntimeError("bench reset requires pyserial in the bench Python environment") from exc
    device = os.path.realpath(port.port)
    matches = [item for item in list_ports.comports() if os.path.realpath(item.device) == device]
    if len(matches) != 1 or (matches[0].vid, matches[0].pid) != (0x303A, 0x1001):
        raise RuntimeError("bench reset requires the ESP32-S3 native USB Serial/JTAG port (303a:1001)")
    def reset() -> None:
        # The inspected esptool 5.3.0 HardReset(uses_usb=False) sequence:
        # assert RTS for 100 ms, release RTS, retaining DTR on each change.
        # ESP32S3ROM uses uses_usb=True only for OTG; USBJTAGSerialReset enters
        # download mode. Keep this small sequence here to need only pyserial.
        port.setRTS(True)
        port.setDTR(port.dtr)
        time.sleep(0.1)
        port.setRTS(False)
        port.setDTR(port.dtr)

    return reset, {
        "strategy": "esp32s3_usb_serial_jtag_hard_reset",
        "sequence_basis": "esptool_5.3.0_HardReset_uses_usb_false",
        "usb_vid": 0x303A,
        "usb_pid": 0x1001,
        "uses_usb_otg": False,
    }


def _is_rom_loader_prefix(text: str) -> bool:
    """Whether nonempty text can prefix ``load:0xHEX,len:0xHEX``.

    USB reset can interrupt the old loader write at any byte, including inside
    a fixed field label. Recognize the prefix language, not just complete fields.
    """
    header, length_header = "load:0x", "len:0x"
    if not text:
        return False
    if header.startswith(text):
        return True
    if not text.startswith(header):
        return False
    address, comma, length = text[len(header):].partition(",")
    hexadecimal = "0123456789abcdefABCDEF"
    if not address or any(value not in hexadecimal for value in address):
        return False
    if not comma or length_header.startswith(length):
        return True
    return (length.startswith(length_header)
            and all(value in hexadecimal for value in length[len(length_header):]))


def _is_interrupted_usb_reset_prefix(text: str) -> bool:
    """Whether text is a strict prefix of the expected USB reset record."""
    expected = "rst:0x15 (USB_UART_CHIP_RESET),boot:0xa (SPI_FAST_FLASH_BOOT)"
    return bool(text) and len(text) < len(expected) and expected.startswith(text)


def _is_rom_saved_pc_prefix(text: str) -> bool:
    """Whether nonempty text can prefix ``Saved PC:0xHEX``."""
    header = "Saved PC:0x"
    if not text:
        return False
    if header.startswith(text):
        return True
    return (text.startswith(header)
            and all(value in "0123456789abcdefABCDEF" for value in text[len(header):]))


class BenchSerial:
    """Serial continuity observer with an explicit reset, never firmware commands."""

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
        self.reset_performed = False
        self.reset_requested_ns: int | None = None
        self.last_receive_ns: int | None = None
        self.auto_push_selection: dict[str, Any] | None = None
        self._pending_lines: list[str] = []

    def reset_for_boot(self, *, reset_factory: Callable[..., Any] | None = None) -> None:
        strategy, metadata = (reset_factory or native_usb_reset_strategy)(self.ser)
        self.reset_performed = False
        self.ser.reset_input_buffer()
        self._pending_lines = []
        self.identity_tracker = RuntimeIdentityTracker()
        self.auto_push_selection = None
        request = self.timeline.record("serial_reset_requested", **metadata)
        if (
            not isinstance(request, dict)
            or type(request.get("host_monotonic_ns")) is not int
        ):
            raise RuntimeError("serial reset timeline anchor is unavailable")
        self.reset_requested_ns = request["host_monotonic_ns"]
        # A failed control-line operation must never acquire a timing anchor.
        strategy()
        self.timeline.record("serial_reset_completed", **metadata)
        self.reset_performed = True

    @property
    def boot_marker_count(self) -> int:
        return self.identity_tracker.boot_marker_count

    @property
    def runtime_identity(self) -> dict[str, Any] | None:
        return self.identity_tracker.identity

    def read_line(self, timeout_s: float = 0.25) -> str:
        if not self._pending_lines:
            self.ser.timeout = timeout_s
            raw = self.ser.readline()
            if not raw:
                return ""
            # USB reset can end an interrupted load line with CR before the
            # new ROM banner. Preserve both logical lines and their order;
            # never discard a preceding panic or repeated boot marker.
            self._pending_lines = []
            for line in raw.decode("utf-8", errors="replace").rstrip("\r\n").split("\r"):
                # Native USB reset can interrupt a ROM loader write without
                # any line terminator before the next exact ROM banner. Split
                # only that loader grammar and preserve both pieces. Unknown
                # prefixes, panic text and extra banners remain intact for the
                # boundary gates to reject; never search past arbitrary text.
                rom_banner = "ESP-ROM:esp32s3-20210327"
                loader_prefix = line[:-len(rom_banner)] if line.endswith(rom_banner) else ""
                self._pending_lines.extend(
                    [loader_prefix, rom_banner]
                    if (_is_rom_loader_prefix(loader_prefix)
                        or _is_interrupted_usb_reset_prefix(loader_prefix)
                        or _is_rom_saved_pc_prefix(loader_prefix))
                    else [line])
        text = self._pending_lines.pop(0)
        safe = redact_artifact_text(text)
        self.log.write(safe + "\n")
        self.log.flush()
        self.line_count += 1
        received = self.timeline.record("serial_receive", line=safe)
        self.last_receive_ns = received["host_monotonic_ns"]
        self.identity_tracker.observe(text)
        selection = parse_auto_push_selection(text)
        if selection is not None:
            selection["observed_host_monotonic_ns"] = received["host_monotonic_ns"]
            if self.auto_push_selection is not None:
                previous = dict(self.auto_push_selection)
                previous.pop("observed_host_monotonic_ns", None)
                current = dict(selection)
                current.pop("observed_host_monotonic_ns", None)
                if current != previous:
                    raise RuntimeError("Auto-Push selected-slot evidence changed during collection")
            else:
                self.auto_push_selection = selection
                self.timeline.record("auto_push_selection_observed", **selection)
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
    require_explicit_reset: bool = False,
) -> dict[str, Any]:
    """Keep an attach-time boot outside the external evidence window."""
    if ready_timeout_s <= 0:
        raise ValueError("serial readiness timeout must be positive")

    started = monotonic()
    readiness_deadline = started + ready_timeout_s
    initial_boot_markers = observer.boot_marker_count
    startup_detected = False
    rom_start_observed = False
    reset_reason_observed = False
    ready_gate_observed = False
    setup_completed = False
    if require_explicit_reset and not observer.reset_performed:
        raise RuntimeIdentityFailure("explicit serial reset did not complete")

    while True:
        if observer.runtime_identity is not None:
            if require_explicit_reset and not reset_reason_observed:
                raise RuntimeIdentityFailure("runtime BOOT identity preceded fresh reset-to-ready evidence")
            if not require_explicit_reset or (ready_gate_observed and setup_completed
                                             and not getattr(observer, "_pending_lines", [])):
                mode = "startup_completed" if startup_detected else "identity_observed"
                break

        now = monotonic()
        if now >= readiness_deadline:
            if require_explicit_reset and observer.runtime_identity is not None:
                raise RuntimeIdentityFailure("normal-runtime ready/setup evidence was not observed after explicit reset")
            raise RuntimeIdentityFailure(
                "runtime BOOT identity was not observed before the external evidence window"
            )

        line = observer.read_line(min(0.25, readiness_deadline - now))
        if require_explicit_reset:
            if re.search(r"Guru Meditation|panic(?:ked|'ed)|assert(?:ion)? failed|abort\(\)|stack canary|Brownout", line, re.IGNORECASE):
                raise RuntimeIdentityFailure("panic or brownout before reset-to-ready completed")
            if "ESP-ROM:" in line:
                if rom_start_observed or not re.fullmatch(r"ESP-ROM:esp32s3-[0-9]{8}", line):
                    raise RuntimeIdentityFailure("unexpected or repeated ROM start after explicit reset")
                rom_start_observed = True
                # Bytes delivered before the first exact ROM banner can be
                # delayed output from the upload reset. The fresh reset reason
                # must follow this banner when the banner is available.
                reset_reason_observed = False
            if line.startswith("rst:"):
                if _is_interrupted_usb_reset_prefix(line):
                    # Preserve a partial pre-banner reset write in the evidence
                    # log without treating it as the fresh reset reason.
                    continue
                if (reset_reason_observed
                        or "rst:0x15 (USB_UART_CHIP_RESET)" not in line
                        or "SPI_FAST_FLASH_BOOT" not in line):
                    raise RuntimeIdentityFailure("unexpected reset reason or boot mode after explicit reset")
                reset_reason_observed = True
            if line.startswith(BOOT_RECORD_PREFIX):
                if observer.boot_marker_count != initial_boot_markers + 1:
                    raise RuntimeIdentityFailure("repeated runtime BOOT before reset-to-ready completed")
                if not re.search(r"(?:^| )reset=USB(?: |$)", line):
                    raise RuntimeIdentityFailure("runtime BOOT reset reason does not match explicit USB reset")
            if re.fullmatch(r"\[Boot\] Ready gate opened at [0-9]+ ms(?: \(timeout\))?", line):
                if observer.runtime_identity is None:
                    raise RuntimeIdentityFailure("ready gate preceded runtime BOOT identity")
                ready_gate_observed = True
            if re.fullmatch(r"\[Boot\] setup total: [0-9]+ ms", line):
                if observer.runtime_identity is None:
                    raise RuntimeIdentityFailure("setup completion preceded runtime BOOT identity")
                setup_completed = True
        if line.startswith(BOOT_START_PREFIXES):
            startup_detected = True

    result = {
        "mode": mode,
        "startup_detected": startup_detected,
        "boot_markers_observed": observer.boot_marker_count - initial_boot_markers,
        "runtime_identity": observer.runtime_identity,
        "duration_seconds": max(0.0, monotonic() - started),
        "rom_start_observed": rom_start_observed,
        "reset_anchored": require_explicit_reset and reset_reason_observed and ready_gate_observed
                          and setup_completed,
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
        ku_qualification: bool,
        machine_event: Callable[[dict[str, Any]], None],
        photo_label_qualification: bool = False,
        junk_qualification: bool = False,
    ) -> None:
        self.executable = executable
        self.suite = suite
        self.mode = "bench" if suite == "replay" else "idle"
        self.blink_profile = blink_profile
        self.lease_fd = lease_fd
        self.scenario = scenario
        self.ku_qualification = ku_qualification
        self.photo_label_qualification = photo_label_qualification
        self.junk_qualification = junk_qualification
        self.machine_event = machine_event
        self.log_path = out_dir / "v1replay.log"
        self.state_path = out_dir / "v1_emulator_state.json"
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
        if (
            self.log_path.exists()
            or self.state_path.exists()
            or (self.scenario_path and self.scenario_path.exists())
        ):
            raise RuntimeError("refusing to overwrite existing replay evidence")
        self.log_handle = self.log_path.open("xb")
        command = [str(self.executable), self.mode]
        if self.mode == "bench":
            if self.scenario:
                command.extend(["--scenario", self.scenario])
            if self.ku_qualification:
                command.append("--ku-qualification")
            if self.photo_label_qualification:
                command.append("--photo-label-qualification")
            if self.junk_qualification:
                command.append("--junk-qualification")
            assert self.scenario_path is not None
            command.extend(["--scenario-evidence", str(self.scenario_path)])
        command.extend(
            [
                "--machine-events",
                "--state-file",
                str(self.state_path),
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
        raise RuntimeError("managed V1 input did not establish its transport before the external window")

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
        try:
            raw = self.log_path.read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            if self.process is not None:
                raise
            raw = ""
        safe = sanitize_artifact_value(raw, run_dir=self.log_path.parent)
        if safe != raw:
            self.log_path.write_text(safe, encoding="utf-8")
        return {
            "started": self.process is not None,
            "lifecycle_completed": lifecycle_completed,
            "mode": self.mode,
            "blink_profile": self.blink_profile,
            "managed_stop": process_was_running,
            "graceful_stop_confirmed": stopped and returncode == 0,
            "returncode": returncode,
            "log": self.log_path.name,
            "scenario_evidence": self.scenario_path.name if self.scenario_path else "",
            "detector_state": self.state_path.name if self.state_path.is_file() else "",
            "stimulus_events": stimulus_events,
            "delivery_events": delivery_events,
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


def require_unused_live_evidence(out_dir: Path, *, camera: bool) -> None:
    reserved = [
        out_dir / "bench_serial.log",
        out_dir / BENCH_TIMELINE_NAME,
        out_dir / BUILD_UPLOAD_ARTIFACTS_NAME,
        out_dir / "firmware.bin",
        out_dir / "window_result.json",
        out_dir / "v1replay.log",
        out_dir / "v1_emulator_state.json",
        out_dir / REPLAY_STIMULUS_NAME,
        out_dir / REPLAY_DELIVERY_NAME,
        out_dir / REPLAY_SCENARIO_EVIDENCE_NAME,
        out_dir / PRESENTATION_SNAPSHOT_NAME,
        out_dir / REPLAY_DISPLAY_CONTRACT_NAME,
    ]
    if camera:
        reserved.append(out_dir / "camera")
    existing = [path.name for path in reserved if path.exists() or path.is_symlink()]
    if existing:
        raise FileExistsError("refusing to reuse existing live evidence: " + ", ".join(existing))


def collect_live(
    args: argparse.Namespace,
    out_dir: Path,
    artifacts: dict[str, Any],
) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    require_unused_live_evidence(out_dir, camera=args.camera)

    intended_git_sha = args.git_sha

    with V1RadioLease() as lease:
        assert lease.fd is not None
        presentation_configuration: dict[str, Any] | None = None
        presentation_capture_ns: int | None = None
        if args.presentation_api_base_url:
            presentation_configuration = wait_for_presentation_configuration(
                args.presentation_api_base_url,
                reconnect=lambda remaining: try_join_maintenance_wifi(
                    timeout_s=remaining
                ),
            )
            presentation_capture_ns = time.monotonic_ns()
        port = wait_for_port(args.port)
        run_upload(port, args.skip_web)
        artifacts["build_upload"] = retain_build_upload_artifacts(
            out_dir,
            BUILD_OUTPUT_DIR,
            upload_performed=True,
        )
        port = wait_for_port(port, 30)
        deadline = time.monotonic() + args.post_upload_settle_seconds
        while time.monotonic() < deadline:
            time.sleep(min(1.0, deadline - time.monotonic()))

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
            ku_qualification=getattr(args, "ku_qualification", False),
            machine_event=lambda payload: timeline.record_external(payload, "v1replay"),
            photo_label_qualification=getattr(args, "photo_label_qualification", False),
            junk_qualification=getattr(args, "junk_qualification", False),
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
            observer.reset_for_boot()
            establish_serial_boundary(observer, args.ready_timeout_seconds, require_explicit_reset=True)
            assert observer.runtime_identity is not None
            runtime_qualification = qualify_runtime_identity(
                observer.runtime_identity,
                intended_git_sha=intended_git_sha,
                build_upload=artifacts["build_upload"],
            )
            initial_boot_markers = observer.boot_marker_count

            if presentation_configuration is not None:
                timeline.record(
                    "maintenance_presentation_configuration_captured_before_upload",
                    configuration_sha256=_sha256_value(presentation_configuration),
                    capture_host_monotonic_ns=presentation_capture_ns,
                )

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
            if presentation_configuration is not None:
                if observer.auto_push_selection is None:
                    raise RuntimeError("Auto-Push selected-slot evidence was not observed")
                if presentation_capture_ns is None:
                    raise RuntimeError("maintenance presentation capture timing is unavailable")
                artifacts["presentation_snapshot"] = publish_presentation_snapshot(
                    out_dir,
                    configuration=presentation_configuration,
                    selection=observer.auto_push_selection,
                    maintenance_capture_ns=presentation_capture_ns,
                )
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
            print(
                "[bench] external window complete; finalizing raw evidence",
                flush=True,
            )
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
            emulator_state_path = out_dir / "v1_emulator_state.json"
            if emulator_state_path.is_file():
                artifacts["v1_emulator_state"] = file_artifact(emulator_state_path)
            scenario_path = out_dir / REPLAY_SCENARIO_EVIDENCE_NAME
            if scenario_path.is_file():
                artifacts["replay_scenario"] = file_artifact(scenario_path)
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
                    if presentation_configuration is not None:
                        if observer is None or observer.runtime_identity is None:
                            raise RuntimeError("runtime identity is unavailable for display contract")
                        artifacts["replay_display_contract"] = publish_replay_display_contract(
                            out_dir,
                            artifacts=artifacts,
                            runtime_identity=observer.runtime_identity,
                            camera_result=camera_result,
                        )
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
        print("[bench] raw evidence finalized", flush=True)
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
    if args.blink_profile is None:
        args.blink_profile = "scenario" if args.suite == "replay" else "steady"

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    # Refusal must not adopt an earlier recording as this run's writable output,
    # including when argument validation would otherwise write a failure result.
    try:
        require_unused_live_evidence(out_dir, camera=args.camera)
    except FileExistsError as exc:
        print(f"[bench] {exc}", file=sys.stderr, flush=True)
        return 3
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
    try:
        args.presentation_api_base_url = normalize_presentation_api_base_url(
            getattr(args, "presentation_api_base_url", "")
        )
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
    if args.suite != "replay" and args.presentation_api_base_url:
        return fail("--presentation-api-base-url is valid only for replay")
    if args.suite != "replay" and getattr(args, "ku_qualification", False):
        return fail("--ku-qualification is valid only for replay")
    if args.suite != "replay" and getattr(args, "photo_label_qualification", False):
        return fail("--photo-label-qualification is valid only for replay")
    if args.suite != "replay" and getattr(args, "junk_qualification", False):
        return fail("--junk-qualification is valid only for replay")
    focused_qualifications = sum(
        bool(value)
        for value in (
            getattr(args, "ku_qualification", False),
            getattr(args, "photo_label_qualification", False),
            getattr(args, "junk_qualification", False),
        )
    )
    if focused_qualifications > 1:
        return fail("choose only one focused replay qualification")
    if args.scenario and focused_qualifications:
        return fail("--scenario cannot be combined with a focused replay qualification")
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
    try:
        require_current_source_identity(
            expected_git_sha=args.git_sha,
            expected_git_ref=args.git_ref,
        )
    except SourceProvenanceFailure as exc:
        return fail(
            str(exc),
            result="FAIL",
            failure_kind="source_provenance",
            git_worktree_clean=False,
            runtime_qualification={"status": "unqualified", "reason": exc.reason},
        )
    if not args.replay_executable:
        return fail("managed v1replay is required for live collection")
    if serial is None:
        return fail("pyserial is required for live collection")

    try:
        result = collect_live(args, out_dir, artifacts)
        require_current_source_identity(
            expected_git_sha=args.git_sha,
            expected_git_ref=args.git_ref,
        )
        qualification = result["runtime_qualification"]
        write_window_result(
            out_dir,
            {
                "result": "COMPLETE",
                "evidence_contract": "external_only",
                "suite": args.suite,
                "duration_seconds": args.duration_seconds,
                "board_id": args.board_id,
                "git_sha": args.git_sha,
                "git_ref": args.git_ref,
                "git_worktree_clean": args.git_worktree_clean == "1",
                "tooling_source": {"git_sha": args.git_sha, "git_ref": args.git_ref,
                                   "git_worktree_clean": args.git_worktree_clean == "1"},
                "device_port": redact_artifact_text(result["port"]),
                "completion": result["completion"],
                "emulator": result["emulator"],
                "camera": result["camera"],
                "runtime_identity": result["runtime_identity"],
                "runtime_qualification": qualification,
                "artifacts": artifacts,
            },
        )
        return 0
    except FileExistsError as exc:
        print(f"[bench] {exc}", file=sys.stderr, flush=True)
        return 3
    except RuntimeIdentityFailure as exc:
        return fail(
            str(exc),
            failure_kind="runtime_identity",
            runtime_identity=exc.identity,
            runtime_qualification=exc.qualification,
        )
    except SourceProvenanceFailure as exc:
        return fail(
            str(exc),
            result="FAIL",
            failure_kind="source_provenance",
            git_worktree_clean=False,
            runtime_qualification={"status": "unqualified", "reason": exc.reason},
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
