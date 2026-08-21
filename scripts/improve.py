#!/usr/bin/env python3
"""Safely evaluate one committed firmware optimization against fresh bench evidence.

The live controller is deliberately conservative:

* it never checks out, resets, cleans, or commits in the invoking worktree;
* it builds isolated base and candidate worktrees before touching the rig;
* it flashes firmware only, preserving the device's LittleFS contents;
* it requires every full all-suite/camera bench invocation to PASS;
* it accepts only strict, non-overlapping empirical improvement envelopes; and
* it restores the pinned base firmware after rejection, interruption, or an
  uncertain candidate flash.

``dry-run`` exercises the same decision and recovery engine with deterministic
fake hardware and a disposable local Git repository.  It never invokes build
tools, serial, camera, bench, or the real repository.
"""

from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import pwd
import re
import shutil
import signal
import stat
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, Iterable, Mapping, Protocol, Sequence


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
BENCH_TOOLS = ROOT / "scripts" / "bench"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(BENCH_TOOLS))

from bench_identity import (  # noqa: E402
    current_grader_fingerprint,
    current_hardware_scoring_fingerprint,
    current_product_fingerprint,
    scenario_manifest,
)
from bench_policy import (  # noqa: E402
    validate_qualification_evidence,
    validate_qualification_record,
)
from score_hardware_run import (  # noqa: E402
    SCORING_SCHEMA_VERSION,
    MetricPolicy,
    load_catalog,
    score_run,
)


SCHEMA_VERSION = 1
MIN_RUNS = 5
DEFAULT_RUNS = 5
DEFAULT_BOARD_ID = "release"
DEFAULT_ENV = "waveshare-349"
DEFAULT_SETTLE_SECONDS = 90
CHILD_TERMINATION_GRACE_SECONDS = 75
DEFAULT_COMMAND_TIMEOUT_SECONDS = 4 * 60 * 60
MAX_FIRMWARE_BYTES = 5_570_560
EXPECTED_LITTLEFS_BYTES = 2_424_832
RUN_DURATION_SECONDS = 300
REPLAY_DURATION_SECONDS = 300
PROFILE = "drive_wifi_off"
SEGMENT = "last"
BLINK_PROFILE = "scenario"
GIB = 1024**3
ESTIMATED_GIB_PER_FULL_RUN = 2.25
WORKTREE_AND_BUILD_RESERVE_GIB = 6
DISK_RESERVE_GIB = 10
RECOVERY_RESERVE_BYTES = 16 * 1024 * 1024
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX40 = re.compile(r"^[0-9a-f]{40}$")
SAFE_BRANCH = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]*$")
# Phase B v1 intentionally exposes only implementation translation units below
# the protected display-pipeline timing boundary.  Headers are excluded because
# project-wide forced includes can otherwise redefine the clock or recorder seen
# by unchanged instrumentation.  Tests may accompany at least one eligible
# firmware change, but cannot be the only candidate change.
DISPLAY_OPTIMIZATION_PATHS = frozenset(
    {
        "src/display_arrow.cpp",
        "src/display_bands.cpp",
        "src/display_cards.cpp",
        "src/display_font_manager.cpp",
        "src/display_frequency.cpp",
        "src/display_frequency_digit_atlas.cpp",
        "src/display_frequency_raster_cache.cpp",
        "src/display_indicators.cpp",
        "src/display_screens.cpp",
        "src/display_sliders.cpp",
        "src/display_status_bar.cpp",
        "src/display_top_counter.cpp",
        "src/modules/display/display_pipeline_module.cpp",
        "src/modules/display/render_frame_composer.cpp",
    }
)
SUPPORTED_TARGET_METRICS = frozenset({"disp_pipe_p95_us", "disp_pipe_max_peak_us"})
SUPPORTED_TARGET_SUITES = frozenset({"replay"})
UNSAFE_GIT_ENVIRONMENT = frozenset(
    {
        "GIT_DIR",
        "GIT_WORK_TREE",
        "GIT_COMMON_DIR",
        "GIT_INDEX_FILE",
        "GIT_OBJECT_DIRECTORY",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_NAMESPACE",
        "GIT_CONFIG_GLOBAL",
        "GIT_CONFIG_PARAMETERS",
        "GIT_CONFIG_SYSTEM",
    }
)
UNSAFE_PRODUCT_ENVIRONMENT = frozenset({"PIO_CMD"})
UNSAFE_SHELL_ENVIRONMENT = frozenset(
    {"BASH_ENV", "BASHOPTS", "ENV", "PS4", "SHELLOPTS"}
)
ACCOUNT_HOME = Path(pwd.getpwuid(os.geteuid()).pw_dir).resolve()
MANAGED_V1_RADIO_LEASE_PATH = (
    ACCOUNT_HOME / ".local" / "state" / "v1simple" / "managed-v1-radio.lock"
)
MANAGED_V1_RADIO_LEASE_FD_ENV = "V1SIMPLE_MANAGED_V1_LEASE_FD"
EXPECTED_TRACK = {
    "core": {
        "run_kind": "real_fw_soak",
        "suite_or_profile": PROFILE,
        "lane": "bench-core",
        "stress_class": "core",
        "source_type": "perf_csv",
    },
    "display": {
        "run_kind": "real_fw_soak",
        "suite_or_profile": PROFILE,
        "lane": "bench-display",
        "stress_class": "display_preview",
        "source_type": "perf_csv",
    },
    "replay": {
        "run_kind": "real_fw_soak",
        "suite_or_profile": PROFILE,
        "lane": "bench-replay",
        "stress_class": "core",
        "source_type": "perf_csv",
    },
}
TERMINAL_STATES = {
    "ACCEPTED",
    "REJECTED_NO_CHANGE",
    "REJECTED_NO_IMPROVEMENT",
    "REJECTED_GATE_FAILURE",
    "REJECTED_RESOURCE_BUDGET",
    "ABORTED_BASE_RESTORED",
    "ABORTED_NO_RESTORE",
}
UNRESOLVED_STATES = {"RESTORE_FAILED", "CLEANUP_FAILED"}


class ImproveError(RuntimeError):
    """Base error for a failed-closed controller decision."""


class InvalidInput(ImproveError):
    """The requested experiment is invalid before hardware work starts."""


class GateFailure(ImproveError):
    """A required build, resource, or bench gate failed."""


class ResourceFailure(GateFailure):
    """A build exceeded or changed a hardware resource budget."""


class NoChangeCandidate(InvalidInput):
    """The committed candidate produced byte-identical firmware."""


class ControllerInterrupted(ImproveError):
    """The user requested a controlled stop."""


class SimulatedPowerLoss(BaseException):
    """Dry-run-only abrupt stop that deliberately bypasses normal cleanup."""


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def controlled_git_argv(argv: Sequence[str]) -> list[str]:
    """Disable repository-local status caching for every controller Git call."""
    command = list(argv)
    if command and Path(command[0]).name == "git":
        command[1:1] = ["-c", "core.fsmonitor=false"]
    return command


def unsafe_shell_environment_key(key: str) -> bool:
    return key in UNSAFE_SHELL_ENVIRONMENT or key.startswith("BASH_FUNC_")


def sanitized_git_environment(
    extra: Mapping[str, str] | None = None,
) -> dict[str, str]:
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("GIT_")
        and not key.startswith("PLATFORMIO_")
        and not key.startswith("BENCH_")
        and key not in UNSAFE_PRODUCT_ENVIRONMENT
        and not unsafe_shell_environment_key(key)
    }
    if extra:
        if any(
            key.startswith("GIT_")
            or key.startswith("PLATFORMIO_")
            or key.startswith("BENCH_")
            or key in UNSAFE_PRODUCT_ENVIRONMENT
            or unsafe_shell_environment_key(key)
            for key in extra
        ):
            raise InvalidInput("unsafe Git child-process environment override requested")
        environment.update(extra)
    environment["HOME"] = str(ACCOUNT_HOME)
    environment["GIT_NO_REPLACE_OBJECTS"] = "1"
    return environment


def sanitized_product_environment(
    extra: Mapping[str, str] | None = None,
) -> dict[str, str]:
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("GIT_")
        and key not in UNSAFE_PRODUCT_ENVIRONMENT
        and not unsafe_shell_environment_key(key)
        and not key.startswith("PLATFORMIO_")
        and not key.startswith("BENCH_")
    }
    if extra:
        if any(
            key.startswith("GIT_")
            or key in UNSAFE_PRODUCT_ENVIRONMENT
            or unsafe_shell_environment_key(key)
            or key.startswith("PLATFORMIO_")
            or key.startswith("BENCH_")
            for key in extra
        ):
            raise InvalidInput("unsafe product-tool environment override requested")
        environment.update(extra)
    environment["HOME"] = str(ACCOUNT_HOME)
    environment["GIT_NO_REPLACE_OBJECTS"] = "1"
    return environment


def validate_git_environment() -> None:
    unsafe = sorted(
        key
        for key in os.environ
        if key in UNSAFE_GIT_ENVIRONMENT
        or key == "GIT_CONFIG_COUNT"
        or key.startswith(("GIT_CONFIG_KEY_", "GIT_CONFIG_VALUE_"))
    )
    if unsafe:
        raise InvalidInput(
            "unsafe ambient Git repository selectors are set: " + ", ".join(unsafe)
        )
    shell_unsafe = sorted(key for key in os.environ if unsafe_shell_environment_key(key))
    if shell_unsafe:
        raise InvalidInput(
            "unsafe ambient child shell controls are set: " + ", ".join(shell_unsafe)
        )
    product_unsafe = sorted(
        key
        for key in os.environ
        if key in UNSAFE_PRODUCT_ENVIRONMENT or key.startswith("PLATFORMIO_")
    )
    if product_unsafe:
        raise InvalidInput(
            "unsafe ambient PlatformIO/build overrides are set: "
            + ", ".join(product_unsafe)
        )
    bench_unsafe = sorted(key for key in os.environ if key.startswith("BENCH_"))
    if bench_unsafe:
        raise InvalidInput(
            "unsafe ambient bench/camera overrides are set: " + ", ".join(bench_unsafe)
        )


def assert_repository_hook_contract(root: Path) -> None:
    hooks_path = run_capture(
        ["git", "config", "--get", "core.hooksPath"], cwd=root
    )
    hooks_dir = root / ".githooks"
    reference_hook = hooks_dir / "reference-transaction"
    if (
        hooks_path != ".githooks"
        or not hooks_dir.is_dir()
        or hooks_dir.is_symlink()
        or not reference_hook.is_file()
        or reference_hook.is_symlink()
        or not os.access(reference_hook, os.X_OK)
    ):
        raise GateFailure("repository privacy-hook contract changed before a Git mutation")
    tracked = run_capture(
        ["git", "ls-files", "--error-unmatch", ".githooks/reference-transaction"],
        cwd=root,
    )
    expected_blob = run_capture(
        ["git", "rev-parse", "HEAD:.githooks/reference-transaction"], cwd=root
    )
    actual_blob = run_capture(
        ["git", "hash-object", ".githooks/reference-transaction"], cwd=root
    )
    if tracked != ".githooks/reference-transaction" or actual_blob != expected_blob:
        raise GateFailure("repository privacy-hook bytes changed before a Git mutation")


def read_json_object(path: Path, label: str) -> dict[str, Any]:
    try:
        encoded, _identity = read_owned_regular_bytes(path, label=label)
        payload = json.loads(encoded.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise GateFailure(f"invalid {label} JSON at {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise GateFailure(f"{label} is not a JSON object: {path}")
    return payload


def open_owned_regular_file(
    path: Path,
    flags: int,
    *,
    label: str,
    mode: int = 0o600,
    expected_identity: tuple[int, int] | None = None,
) -> int:
    try:
        descriptor = os.open(path, flags | getattr(os, "O_NOFOLLOW", 0), mode)
    except FileExistsError:
        raise
    except OSError as exc:
        raise GateFailure(f"{label} cannot be opened safely: {path}") from exc
    try:
        metadata = os.fstat(descriptor)
        path_metadata = path.lstat()
        identity = (metadata.st_dev, metadata.st_ino)
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or metadata.st_nlink != 1
            or identity != (path_metadata.st_dev, path_metadata.st_ino)
            or (expected_identity is not None and identity != expected_identity)
        ):
            raise GateFailure(f"{label} ownership is invalid: {path}")
        return descriptor
    except Exception:
        os.close(descriptor)
        raise


def read_owned_regular_bytes(path: Path, *, label: str) -> tuple[bytes, tuple[int, int]]:
    descriptor = open_owned_regular_file(path, os.O_RDONLY, label=label)
    try:
        metadata = os.fstat(descriptor)
        with os.fdopen(descriptor, "rb", closefd=False) as handle:
            content = handle.read()
        return content, (metadata.st_dev, metadata.st_ino)
    finally:
        os.close(descriptor)


def sha256_owned_regular_file(path: Path, *, label: str) -> str:
    descriptor = open_owned_regular_file(path, os.O_RDONLY, label=label)
    digest = hashlib.sha256()
    try:
        with os.fdopen(descriptor, "rb", closefd=False) as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    finally:
        os.close(descriptor)


def truncate_owned_regular_file(
    path: Path,
    length: int,
    *,
    label: str,
    expected_identity: tuple[int, int],
    append: bytes = b"",
) -> None:
    descriptor = open_owned_regular_file(
        path,
        os.O_RDWR,
        label=label,
        expected_identity=expected_identity,
    )
    try:
        os.ftruncate(descriptor, length)
        if append:
            os.lseek(descriptor, 0, os.SEEK_END)
            view = memoryview(append)
            while view:
                written = os.write(descriptor, view)
                if written <= 0:
                    raise OSError(f"{label} append made no progress")
                view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _ensure_owned_directory_chain(
    owner: Path,
    target: Path,
    *,
    label: str,
    create: bool,
) -> tuple[int, int]:
    """Inspect/create a user-owned directory chain without following symlinks."""
    if not owner.is_absolute() or not target.is_absolute():
        raise InvalidInput(f"{label} must be an absolute path")
    try:
        relative = target.relative_to(owner)
    except ValueError as exc:
        raise InvalidInput(f"{label} escapes its trusted owner") from exc

    current = owner
    components = (".", *relative.parts)
    for part in components:
        if part != ".":
            parent = current
            current = current / part
            if not os.path.lexists(current):
                if not create:
                    raise InvalidInput(f"{label} is unavailable: {current}")
                try:
                    parent_before = parent.lstat()
                    os.mkdir(current, 0o700)
                    parent_after = parent.lstat()
                except FileExistsError:
                    pass
                except OSError as exc:
                    raise InvalidInput(f"cannot create {label}: {current}") from exc
                else:
                    if (parent_before.st_dev, parent_before.st_ino) != (
                        parent_after.st_dev,
                        parent_after.st_ino,
                    ):
                        raise InvalidInput(f"{label} parent changed while it was created")
        try:
            metadata = current.lstat()
        except OSError as exc:
            raise InvalidInput(f"cannot inspect {label}: {current}") from exc
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISDIR(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
        ):
            raise InvalidInput(
                f"{label} must use user-owned directories without symlinks: {current}"
            )
    return (metadata.st_dev, metadata.st_ino)


def _lease_path_owner(path: Path) -> Path:
    """Anchor production leases at the resolved OS-account home directory."""
    try:
        path.relative_to(ACCOUNT_HOME)
    except ValueError:
        # Explicit alternate paths exist only for isolated tests. Their direct
        # parent is still inspected as a non-symlink owned directory.
        return path.parent
    return ACCOUNT_HOME


def _prepare_lease_parent(path: Path, *, label: str) -> tuple[int, int]:
    return _ensure_owned_directory_chain(
        _lease_path_owner(path),
        path.parent,
        label=f"{label} directory",
        create=True,
    )


def _verify_lease_parent(
    path: Path,
    expected_identity: tuple[int, int],
    *,
    label: str,
) -> None:
    actual = _ensure_owned_directory_chain(
        _lease_path_owner(path),
        path.parent,
        label=f"{label} directory",
        create=False,
    )
    if actual != expected_identity:
        raise InvalidInput(f"{label} directory changed while the lease was opened")


def durable_state_dir() -> Path:
    path = ACCOUNT_HOME / ".local" / "state" / "v1simple"
    _ensure_owned_directory_chain(
        ACCOUNT_HOME,
        path,
        label="durable controller state directory",
        create=True,
    )
    try:
        path.chmod(0o700)
    except OSError as exc:
        raise InvalidInput(f"durable controller state permissions cannot be secured: {path}") from exc
    _ensure_owned_directory_chain(
        ACCOUNT_HOME,
        path,
        label="durable controller state directory",
        create=False,
    )
    return path


def validate_serial_port(port: Any, *, require_exists: bool) -> str:
    """Validate a single literal macOS serial device without option injection."""
    if not isinstance(port, str) or not port:
        raise InvalidInput("serial port must be a non-empty device path")
    if any(character.isspace() or ord(character) < 32 for character in port):
        raise InvalidInput("serial port must not contain whitespace or control characters")
    path = Path(port)
    if not path.is_absolute() or path.parent != Path("/dev"):
        raise InvalidInput("serial port must be a direct /dev/cu.* or /dev/tty.* path")
    if not (path.name.startswith("cu.") or path.name.startswith("tty.")):
        raise InvalidInput("serial port must be a /dev/cu.* or /dev/tty.* device")
    if str(path) != port or port.startswith("-"):
        raise InvalidInput("serial port path is not canonical")
    if require_exists:
        try:
            metadata = path.stat()
        except OSError as exc:
            raise InvalidInput(f"serial port is unavailable: {path}") from exc
        if not stat.S_ISCHR(metadata.st_mode):
            raise InvalidInput(f"serial port is not a character device: {path}")
    return port


def assert_owned_path_chain(owner: Path, target: Path, *, label: str) -> None:
    """Require a lexical owned path whose existing components are not symlinks."""
    if not owner.is_absolute() or not target.is_absolute():
        raise InvalidInput(f"{label} must be an absolute owned path")
    try:
        relative = target.relative_to(owner)
    except ValueError as exc:
        raise InvalidInput(f"{label} escapes its owned session") from exc
    current = owner
    for part in (".", *relative.parts):
        if part != ".":
            current = current / part
        if os.path.lexists(current):
            try:
                metadata = current.lstat()
            except OSError as exc:
                raise InvalidInput(f"cannot inspect {label} component: {current}") from exc
            if stat.S_ISLNK(metadata.st_mode):
                raise InvalidInput(f"{label} must not traverse a symlink: {current}")


def ensure_owned_directory(owner: Path, target: Path, *, label: str) -> None:
    """Create one session-owned directory without following a planted symlink."""
    assert_owned_path_chain(owner, target, label=label)
    target.mkdir(parents=True, exist_ok=True)
    assert_owned_path_chain(owner, target, label=label)
    try:
        metadata = target.lstat()
    except OSError as exc:
        raise InvalidInput(f"cannot inspect {label}: {target}") from exc
    if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != os.geteuid():
        raise InvalidInput(f"{label} is not an owned directory: {target}")


def create_recovery_reserve(session_dir: Path) -> dict[str, Any]:
    """Reserve enough local blocks to publish a recovery result under ENOSPC."""
    path = session_dir / ".recovery-reserve"
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        if hasattr(os, "posix_fallocate"):
            os.posix_fallocate(descriptor, 0, RECOVERY_RESERVE_BYTES)
        else:  # pragma: no cover - current macOS/Linux hosts expose posix_fallocate
            block = b"\0" * (1024 * 1024)
            for _ in range(RECOVERY_RESERVE_BYTES // len(block)):
                os.write(descriptor, block)
        os.fsync(descriptor)
    except Exception:
        os.close(descriptor)
        path.unlink(missing_ok=True)
        raise
    else:
        os.close(descriptor)
    fsync_directory(session_dir)
    return {
        "path": str(path),
        "size_bytes": path.stat().st_size,
        "allocated_before_candidate": True,
    }


def release_recovery_reserve(session_dir: Path) -> bool:
    path = session_dir / ".recovery-reserve"
    if not os.path.lexists(path):
        return False
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise GateFailure(f"recovery reserve ownership is invalid: {path}")
    path.unlink()
    fsync_directory(session_dir)
    return True


def release_store_recovery_reserve(store: EvidenceStore) -> bool:
    session_dir = getattr(store, "session_dir", None)
    if not isinstance(session_dir, Path):
        return False
    released = release_recovery_reserve(session_dir)
    if released:
        store.update(recovery_reserve_released=True)
    return released


def write_exclusive(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
    finally:
        os.close(descriptor)
    fsync_directory(path.parent)


def write_exclusive_json(path: Path, payload: Mapping[str, Any]) -> None:
    write_exclusive(path, json.dumps(payload, indent=2, sort_keys=True).encode("utf-8") + b"\n")


def write_atomic_json(path: Path, payload: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        fsync_directory(path.parent)
    finally:
        if temporary.exists():
            temporary.unlink()


class EvidenceStore(Protocol):
    state: dict[str, Any]

    def event(self, name: str, details: Mapping[str, Any] | None = None) -> None: ...

    def update(self, **changes: Any) -> None: ...

    def finish(self, decision: Mapping[str, Any]) -> None: ...


class FileEvidenceStore:
    """Hash-chained durable session evidence."""

    def __init__(
        self,
        session_dir: Path,
        initial_state: Mapping[str, Any] | None = None,
        *,
        clock: Callable[[], str] = utc_now,
    ):
        self.session_dir = Path(os.path.abspath(session_dir))
        for component in (self.session_dir.parent, self.session_dir):
            if os.path.lexists(component) and component.is_symlink():
                raise InvalidInput(f"evidence session must not traverse a symlink: {component}")
        self.clock = clock
        self.state_path = self.session_dir / "state.json"
        self.events_path = self.session_dir / "events.jsonl"
        self.decision_path = self.session_dir / "decision.json"
        if initial_state is None:
            self.session_dir.mkdir(parents=True, exist_ok=False)
            self.state = {
                "schema_version": SCHEMA_VERSION,
                "kind": "improve_state",
                "status": "NEW",
                "event_count": 0,
                "last_event_sha256": "0" * 64,
                "candidate_may_be_installed": False,
                "restore_required": False,
                "evaluation_cleanup_required": False,
                "current_firmware": "unknown",
                "runs": [],
            }
            write_atomic_json(self.state_path, self.state)
        else:
            self.session_dir.mkdir(parents=True, exist_ok=True)
            self.state = dict(initial_state)

    @classmethod
    def open(cls, session_dir: Path) -> "FileEvidenceStore":
        session_dir = Path(os.path.abspath(session_dir))
        for component in (session_dir.parent, session_dir):
            if os.path.lexists(component) and component.is_symlink():
                raise InvalidInput(f"evidence session must not traverse a symlink: {component}")
        state = read_json_object(session_dir / "state.json", "improvement state")
        store = cls(session_dir, state)
        prior = "0" * 64
        count = 0
        repaired_torn_tail = False
        if store.events_path.is_file():
            try:
                journal, journal_identity = read_owned_regular_bytes(
                    store.events_path, label="improvement event journal"
                )
            except OSError as exc:
                raise GateFailure("improvement event journal cannot be read") from exc
            chunks = journal.splitlines(keepends=True)
            for line_number, chunk in enumerate(chunks, start=1):
                complete_line = chunk.endswith(b"\n")
                encoded = chunk[:-1] if complete_line else chunk
                try:
                    entry = json.loads(encoded.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    if line_number != len(chunks) or complete_line:
                        raise GateFailure(f"invalid event journal line {line_number}: {exc}") from exc
                    torn_path = store.session_dir / (
                        f"events.torn.{sha256_bytes(chunk)[:16]}.bin"
                    )
                    if not torn_path.exists():
                        write_exclusive(torn_path, chunk)
                    truncate_owned_regular_file(
                        store.events_path,
                        len(journal) - len(chunk),
                        label="improvement event journal",
                        expected_identity=journal_identity,
                    )
                    repaired_torn_tail = True
                    break
                if not isinstance(entry, dict):
                    invalid = True
                else:
                    stored_hash = entry.pop("event_sha256", None)
                    invalid = (
                        entry.get("sequence") != line_number
                        or entry.get("previous_sha256") != prior
                        or stored_hash != sha256_bytes(canonical_bytes(entry))
                    )
                if invalid:
                    if line_number != len(chunks) or complete_line:
                        raise GateFailure(f"event journal chain is invalid at line {line_number}")
                    torn_path = store.session_dir / (
                        f"events.torn.{sha256_bytes(chunk)[:16]}.bin"
                    )
                    if not torn_path.exists():
                        write_exclusive(torn_path, chunk)
                    truncate_owned_regular_file(
                        store.events_path,
                        len(journal) - len(chunk),
                        label="improvement event journal",
                        expected_identity=journal_identity,
                    )
                    repaired_torn_tail = True
                    break
                prior = str(stored_hash)
                count = line_number
                if not complete_line:
                    # A complete final event may have reached disk just before
                    # its newline. Preserve it and make future appends safe.
                    truncate_owned_regular_file(
                        store.events_path,
                        len(journal),
                        label="improvement event journal",
                        expected_identity=journal_identity,
                        append=b"\n",
                    )
                    repaired_torn_tail = True
        state_count = state.get("event_count")
        state_hash = state.get("last_event_sha256")
        if not isinstance(state_count, int) or state_count < 0:
            raise GateFailure("improvement state has an invalid event count")
        if state_count > count and not repaired_torn_tail:
            raise GateFailure("improvement state is ahead of its durable event journal")
        if state_count == count and state_hash != prior and not repaired_torn_tail:
            raise GateFailure("improvement state does not match its durable event journal")
        if state_count != count or (state_count == count and state_hash != prior and repaired_torn_tail):
            # The process stopped after fsyncing an event but before advancing
            # state.json, or the final journal write tore. The validated journal
            # prefix is authoritative and the torn bytes remain quarantined.
            store.state["event_count"] = count
            store.state["last_event_sha256"] = prior
            if repaired_torn_tail:
                store.state["journal_tail_repaired"] = True
            write_atomic_json(store.state_path, store.state)
        return store

    def event(self, name: str, details: Mapping[str, Any] | None = None) -> None:
        sequence = int(self.state.get("event_count", 0)) + 1
        entry: dict[str, Any] = {
            "schema_version": SCHEMA_VERSION,
            "sequence": sequence,
            "timestamp_utc": self.clock(),
            "event": name,
            "details": dict(details or {}),
            "previous_sha256": str(self.state.get("last_event_sha256") or ""),
        }
        entry["event_sha256"] = sha256_bytes(canonical_bytes(entry))
        encoded = canonical_bytes(entry) + b"\n"
        journal_existed = os.path.lexists(self.events_path)
        if journal_existed:
            descriptor = open_owned_regular_file(
                self.events_path,
                os.O_WRONLY | os.O_APPEND,
                label="improvement event journal",
            )
        else:
            try:
                descriptor = open_owned_regular_file(
                    self.events_path,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_APPEND,
                    label="improvement event journal",
                )
            except FileExistsError:
                journal_existed = True
                descriptor = open_owned_regular_file(
                    self.events_path,
                    os.O_WRONLY | os.O_APPEND,
                    label="improvement event journal",
                )
        original_size = os.lseek(descriptor, 0, os.SEEK_END)
        try:
            view = memoryview(encoded)
            while view:
                written = os.write(descriptor, view)
                if written <= 0:
                    raise OSError("event journal append made no progress")
                view = view[written:]
            os.fsync(descriptor)
            if not journal_existed:
                fsync_directory(self.session_dir)
        except Exception:
            try:
                os.ftruncate(descriptor, original_size)
                os.fsync(descriptor)
            except OSError as rollback_exc:
                raise GateFailure(
                    "event journal append failed and its partial tail could not be rolled back"
                ) from rollback_exc
            raise
        finally:
            os.close(descriptor)
        self.state["event_count"] = sequence
        self.state["last_event_sha256"] = entry["event_sha256"]
        write_atomic_json(self.state_path, self.state)

    def update(self, **changes: Any) -> None:
        self.state.update(changes)
        write_atomic_json(self.state_path, self.state)

    def finish(self, decision: Mapping[str, Any]) -> None:
        if os.path.lexists(self.decision_path):
            existing, _identity = read_owned_regular_bytes(
                self.decision_path, label="immutable improvement decision"
            )
            candidate = json.dumps(decision, indent=2, sort_keys=True).encode("utf-8") + b"\n"
            if existing != candidate:
                raise GateFailure(f"immutable decision already differs: {self.decision_path}")
            self.update(
                status=str(decision["result"]),
                decision_sha256=sha256_owned_regular_file(
                    self.decision_path, label="immutable improvement decision"
                ),
            )
            return
        write_exclusive_json(self.decision_path, decision)
        self.update(
            status=str(decision["result"]),
            decision_sha256=sha256_owned_regular_file(
                self.decision_path, label="immutable improvement decision"
            ),
        )


class MemoryEvidenceStore:
    """Deterministic evidence store used only by the no-hardware dry run."""

    def __init__(self):
        self.state: dict[str, Any] = {
            "status": "NEW",
            "candidate_may_be_installed": False,
            "restore_required": False,
            "evaluation_cleanup_required": False,
            "current_firmware": "unknown",
            "runs": [],
        }
        self.events: list[dict[str, Any]] = []
        self.decision: dict[str, Any] | None = None

    def event(self, name: str, details: Mapping[str, Any] | None = None) -> None:
        self.events.append({"event": name, "details": dict(details or {})})

    def update(self, **changes: Any) -> None:
        self.state.update(changes)

    def finish(self, decision: Mapping[str, Any]) -> None:
        self.decision = dict(decision)
        self.state["status"] = str(decision["result"])


class GlobalLease:
    def __init__(self, path: Path | None = None):
        selected = path if path is not None else durable_state_dir() / "improve-controller.lock"
        self.path = Path(os.path.abspath(selected))
        self.handle: Any = None

    def __enter__(self) -> "GlobalLease":
        parent_identity = _prepare_lease_parent(self.path, label="controller lease")
        flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(self.path, flags, 0o600)
        try:
            metadata = os.fstat(descriptor)
            path_metadata = self.path.lstat()
            if (
                not stat.S_ISREG(metadata.st_mode)
                or not stat.S_ISREG(path_metadata.st_mode)
                or metadata.st_uid != os.geteuid()
                or metadata.st_nlink != 1
                or (metadata.st_dev, metadata.st_ino)
                != (path_metadata.st_dev, path_metadata.st_ino)
            ):
                raise InvalidInput("controller lease ownership is invalid")
            _verify_lease_parent(
                self.path,
                parent_identity,
                label="controller lease",
            )
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            os.close(descriptor)
            raise InvalidInput("another improvement controller owns the host-wide lease") from exc
        except Exception:
            os.close(descriptor)
            raise
        self.handle = os.fdopen(descriptor, "a+", encoding="utf-8")
        self.handle.seek(0)
        self.handle.truncate()
        self.handle.write(f"pid={os.getpid()}\n")
        self.handle.flush()
        return self

    def __exit__(self, *_: Any) -> None:
        if self.handle is not None:
            fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
            self.handle.close()
            self.handle = None


class CampaignRadioLease:
    """Own run_window's durable radio lock across every flash and bench run."""

    def __init__(self, path: Path = MANAGED_V1_RADIO_LEASE_PATH):
        self.path = path
        self.fd: int | None = None

    def acquire(self) -> int:
        if self.fd is not None:
            return self.fd
        parent_identity = _prepare_lease_parent(
            self.path,
            label="managed V1 radio lease",
        )
        descriptor = os.open(
            self.path,
            os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0),
            0o600,
        )
        try:
            metadata = os.fstat(descriptor)
            path_metadata = self.path.lstat()
            if (
                not stat.S_ISREG(metadata.st_mode)
                or not stat.S_ISREG(path_metadata.st_mode)
                or metadata.st_uid != os.geteuid()
                or metadata.st_nlink != 1
                or (metadata.st_dev, metadata.st_ino)
                != (path_metadata.st_dev, path_metadata.st_ino)
            ):
                raise InvalidInput("managed V1 radio lease ownership is invalid")
            _verify_lease_parent(
                self.path,
                parent_identity,
                label="managed V1 radio lease",
            )
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            os.set_inheritable(descriptor, True)
        except BlockingIOError as exc:
            os.close(descriptor)
            raise InvalidInput(
                "managed V1 radio lease unavailable; another bench or emulator owns it"
            ) from exc
        except Exception:
            os.close(descriptor)
            raise
        self.fd = descriptor
        return descriptor

    def child_contract(self) -> tuple[tuple[int, ...], dict[str, str]]:
        descriptor = self.acquire()
        return (descriptor,), {MANAGED_V1_RADIO_LEASE_FD_ENV: str(descriptor)}

    def close(self) -> None:
        if self.fd is not None:
            # Do not issue LOCK_UN: inherited duplicates share this open-file
            # description, and an orphaned child must keep excluding a new rig
            # owner until that child exits and closes its last descriptor.
            os.close(self.fd)
            self.fd = None


class ActiveSessionRegistry:
    """Stable cross-worktree pointer to recovery-required controller state."""

    def __init__(self, path: Path | None = None):
        selected = path if path is not None else durable_state_dir() / "improve-active.json"
        self.path = Path(os.path.abspath(selected))

    def _payload(self) -> dict[str, Any] | None:
        if not os.path.lexists(self.path):
            return None
        try:
            encoded, _identity = read_owned_regular_bytes(
                self.path, label="active-session registry"
            )
            payload = json.loads(encoded.decode("utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, GateFailure) as exc:
            raise InvalidInput(
                f"active-session registry is unreadable; inspect it before continuing: {self.path}"
            ) from exc
        if not isinstance(payload, dict) or payload.get("kind") != "improve_active_session":
            raise InvalidInput(
                f"active-session registry is invalid; inspect it before continuing: {self.path}"
            )
        return payload

    @staticmethod
    def _canonical_session_input(session: Path) -> Path:
        absolute = Path(os.path.abspath(session))
        return absolute.parent.resolve() / absolute.name

    def _bound_session(self, payload: Mapping[str, Any]) -> Path:
        session_text = payload.get("session_dir")
        if not isinstance(session_text, str) or not session_text:
            raise InvalidInput("active-session registry has no session path")
        session = Path(os.path.abspath(session_text))
        if session_text != str(session):
            raise InvalidInput("active-session registry has a noncanonical session path")
        assert_owned_path_chain(Path(session.anchor), session, label="active session directory")
        try:
            metadata = session.lstat()
        except OSError as exc:
            raise InvalidInput(
                f"active session directory is unavailable; recovery remains required: {session}"
            ) from exc
        if (
            not stat.S_ISDIR(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or payload.get("session_dev") != metadata.st_dev
            or payload.get("session_ino") != metadata.st_ino
        ):
            raise InvalidInput(
                f"active session directory identity changed; recovery remains required: {session}"
            )
        return session

    def unresolved(self) -> Path | None:
        payload = self._payload()
        if payload is None:
            return None
        session = self._bound_session(payload)
        plan_path = session / "plan.json"
        plan_hash = payload.get("plan_sha256")
        try:
            actual_plan_hash = sha256_owned_regular_file(
                plan_path, label="active-session plan"
            )
        except GateFailure as exc:
            raise InvalidInput(
                f"active session plan is missing or changed; recovery remains required: {session}"
            ) from exc
        if not valid_digest(plan_hash) or actual_plan_hash != plan_hash:
            raise InvalidInput(
                f"active session plan is missing or changed; recovery remains required: {session}"
            )
        decision_path = session / "decision.json"
        if decision_path.is_file():
            decision = read_json_object(decision_path, "active-session decision")
            if decision.get("result") in TERMINAL_STATES:
                plan = read_json_object(plan_path, "active-session plan")
                decision_plan = decision.get("plan")
                state = read_json_object(session / "state.json", "active-session state")
                identity_mismatch = (
                    decision.get("kind") != "improve_decision"
                    or decision.get("base_sha") != plan.get("base_sha")
                    or decision.get("candidate_sha") != plan.get("candidate_sha")
                    or not isinstance(decision_plan, dict)
                    or decision_plan.get("sha256") != plan_hash
                )
                if identity_mismatch:
                    raise InvalidInput(
                        f"active session has an unsafe or mismatched terminal record: {session}"
                    )
                cleanup_pending = (
                    state.get("restore_required") is True
                    or state.get("evaluation_cleanup_required") is True
                    or (
                        decision.get("result") != "ACCEPTED"
                        and state.get("candidate_may_be_installed") is True
                    )
                )
                if cleanup_pending:
                    if decision.get("result") == "ACCEPTED":
                        # A crash after publishing ACCEPTED but before clearing
                        # its bookkeeping flags is recoverable without touching
                        # the intentionally installed candidate firmware/ref.
                        return session
                    raise InvalidInput(
                        f"active session has an unsafe terminal cleanup state: {session}"
                    )
                self.clear(session)
                return None
        return session

    def register(self, session: Path) -> None:
        existing = self.unresolved()
        session = self._canonical_session_input(session)
        assert_owned_path_chain(Path(session.anchor), session, label="active session directory")
        try:
            session_metadata = session.lstat()
        except OSError as exc:
            raise InvalidInput(f"cannot register missing improvement session: {session}") from exc
        if not stat.S_ISDIR(session_metadata.st_mode) or session_metadata.st_uid != os.geteuid():
            raise InvalidInput(f"cannot register unowned improvement session: {session}")
        if existing is not None and existing != session:
            raise InvalidInput(
                f"unfinished Phase-B session requires recovery first: {existing}"
            )
        if existing == session:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        write_atomic_json(
            self.path,
            {
                "schema_version": SCHEMA_VERSION,
                "kind": "improve_active_session",
                "session_dir": str(session),
                "session_dev": session_metadata.st_dev,
                "session_ino": session_metadata.st_ino,
                "plan_sha256": sha256_owned_regular_file(
                    session / "plan.json", label="active-session plan"
                ),
            },
        )

    def clear(self, session: Path) -> None:
        payload = self._payload()
        if payload is None:
            return
        registered = self._bound_session(payload)
        requested = self._canonical_session_input(session)
        if registered != requested:
            raise GateFailure("refusing to clear another improvement session's recovery pointer")
        self.path.unlink()
        fsync_directory(self.path.parent)


class StopController:
    def __init__(self):
        self.requested = False
        self.signal_number: int | None = None
        self._prior: dict[int, Any] = {}

    def _handle(self, signum: int, _frame: Any) -> None:
        if not self.requested:
            self.requested = True
            self.signal_number = signum

    def __enter__(self) -> "StopController":
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            self._prior[signum] = signal.getsignal(signum)
            signal.signal(signum, self._handle)
        return self

    def __exit__(self, *_: Any) -> None:
        for signum, prior in self._prior.items():
            signal.signal(signum, prior)

    def check(self) -> None:
        if self.requested:
            raise ControllerInterrupted(
                f"controlled stop requested by signal {self.signal_number or 'unknown'}"
            )


class CommandRunner:
    """Run one owned child process group with bounded signal cleanup."""

    def __init__(
        self,
        stop: StopController,
        *,
        poll_interval_seconds: float = 0.2,
        termination_grace_seconds: float = CHILD_TERMINATION_GRACE_SECONDS,
        command_timeout_seconds: float = DEFAULT_COMMAND_TIMEOUT_SECONDS,
        monotonic: Callable[[], float] = time.monotonic,
        sleeper: Callable[[float], None] = time.sleep,
    ):
        self.stop = stop
        self.poll_interval_seconds = poll_interval_seconds
        self.termination_grace_seconds = termination_grace_seconds
        self.command_timeout_seconds = command_timeout_seconds
        self.monotonic = monotonic
        self.sleeper = sleeper

    def run(
        self,
        argv: Sequence[str],
        *,
        cwd: Path,
        log_path: Path,
        recovery: bool = False,
        allowed_statuses: frozenset[int] = frozenset({0}),
        pass_fds: Sequence[int] = (),
        extra_env: Mapping[str, str] | None = None,
    ) -> int:
        command = controlled_git_argv(argv)
        if not recovery:
            self.stop.check()
        log_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            log = log_path.open("xb")
        except FileExistsError as exc:
            raise GateFailure(f"command log already exists and is immutable: {log_path}") from exc
        with log:
            log.write(("$ " + " ".join(command) + "\n").encode("utf-8"))
            log.flush()
            popen_options: dict[str, Any] = {
                "cwd": cwd,
                "stdin": subprocess.DEVNULL,
                "stdout": log,
                "stderr": subprocess.STDOUT,
                "start_new_session": True,
            }
            if pass_fds:
                popen_options["pass_fds"] = tuple(pass_fds)
            if command and Path(command[0]).name == "git":
                environment = sanitized_git_environment(extra_env)
                popen_options["env"] = environment
            else:
                popen_options["env"] = sanitized_product_environment(extra_env)
            process = subprocess.Popen(command, **popen_options)
            started_at = self.monotonic()
            termination_started_at: float | None = None
            termination_reason = ""
            killed = False
            while process.poll() is None:
                now = self.monotonic()
                timed_out = now - started_at >= self.command_timeout_seconds
                interrupted = self.stop.requested and not recovery
                if (timed_out or interrupted) and termination_started_at is None:
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    termination_started_at = now
                    termination_reason = "timeout" if timed_out else "interrupt"
                if (
                    termination_started_at is not None
                    and not killed
                    and now - termination_started_at >= self.termination_grace_seconds
                ):
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    killed = True
                self.sleeper(self.poll_interval_seconds)
            status = int(process.returncode or 0)
        if self.stop.requested and not recovery:
            raise ControllerInterrupted(
                f"controlled stop requested while running {Path(argv[0]).name}"
            )
        if termination_reason == "interrupt":
            raise ControllerInterrupted(
                f"controlled stop requested while running {Path(argv[0]).name}"
            )
        if termination_reason == "timeout":
            raise GateFailure(
                f"command timed out after {self.command_timeout_seconds:.0f}s: "
                f"{' '.join(argv)} (log: {log_path})"
            )
        if status not in allowed_statuses:
            raise GateFailure(
                f"command failed with exit {status}: {' '.join(argv)} (log: {log_path})"
            )
        return status


def run_capture(argv: Sequence[str], *, cwd: Path) -> str:
    command = controlled_git_argv(argv)
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=(
            sanitized_git_environment()
            if command and Path(command[0]).name == "git"
            else None
        ),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise InvalidInput(f"command failed: {' '.join(argv)}: {detail}")
    return completed.stdout.rstrip("\n")


def run_capture_optional(argv: Sequence[str], *, cwd: Path) -> tuple[int, str, str]:
    command = controlled_git_argv(argv)
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=(
            sanitized_git_environment()
            if command and Path(command[0]).name == "git"
            else None
        ),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    return completed.returncode, completed.stdout.rstrip("\n"), completed.stderr.rstrip("\n")


def run_capture_bytes(argv: Sequence[str], *, cwd: Path) -> bytes:
    command = controlled_git_argv(argv)
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=(
            sanitized_git_environment()
            if command and Path(command[0]).name == "git"
            else None
        ),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise GateFailure(f"command failed: {' '.join(argv)}: {detail}")
    return completed.stdout


def _git_blob_oid(path: Path, object_format: str, *, label: str) -> str:
    if object_format not in {"sha1", "sha256"}:
        raise GateFailure(f"unsupported Git object format: {object_format}")
    descriptor = open_owned_regular_file(path, os.O_RDONLY, label=label)
    try:
        before = os.fstat(descriptor)
        digest = hashlib.new(object_format)
        digest.update(f"blob {before.st_size}\0".encode("ascii"))
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
        after = os.fstat(descriptor)
        if (before.st_dev, before.st_ino, before.st_size) != (
            after.st_dev,
            after.st_ino,
            after.st_size,
        ):
            raise GateFailure(f"{label} changed while its raw bytes were verified")
        return digest.hexdigest()
    finally:
        os.close(descriptor)


def verify_worktree_matches_commit(worktree: Path, commit: str) -> dict[str, Any]:
    """Compare raw tracked bytes/modes to Git objects without clean/smudge filters."""
    root = Path(os.path.abspath(worktree))
    object_format = run_capture(["git", "rev-parse", "--show-object-format"], cwd=root)
    listing = run_capture_bytes(
        ["git", "ls-tree", "-rz", "--full-tree", commit],
        cwd=root,
    )
    entries = [entry for entry in listing.split(b"\0") if entry]
    if not entries:
        raise GateFailure("pinned commit has no tracked files")
    checked = 0
    for entry in entries:
        try:
            header, encoded_path = entry.split(b"\t", 1)
            encoded_mode, encoded_type, encoded_oid = header.split(b" ", 2)
            relative_text = encoded_path.decode("utf-8")
            mode = encoded_mode.decode("ascii")
            object_type = encoded_type.decode("ascii")
            expected_oid = encoded_oid.decode("ascii")
        except (ValueError, UnicodeDecodeError) as exc:
            raise GateFailure("pinned commit tree contains an unsupported entry") from exc
        relative = PurePosixPath(relative_text)
        if (
            relative.is_absolute()
            or not relative.parts
            or any(part in {"", ".", ".."} for part in relative.parts)
        ):
            raise GateFailure(f"pinned commit tree contains an unsafe path: {relative_text!r}")
        if object_type != "blob" or mode not in {"100644", "100755"}:
            raise GateFailure(
                f"pinned commit contains an unsupported tracked entry: {relative_text} ({mode})"
            )
        actual = root.joinpath(*relative.parts)
        assert_owned_path_chain(root, actual.parent, label="tracked worktree path")
        try:
            metadata = actual.lstat()
        except OSError as exc:
            raise GateFailure(f"tracked file is missing from worktree: {relative_text}") from exc
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise GateFailure(f"tracked worktree path is not an owned regular file: {relative_text}")
        expected_executable = mode == "100755"
        if bool(metadata.st_mode & 0o111) != expected_executable:
            raise GateFailure(f"tracked executable mode differs from pinned commit: {relative_text}")
        actual_oid = _git_blob_oid(
            actual,
            object_format,
            label=f"tracked worktree file {relative_text}",
        )
        if actual_oid != expected_oid:
            raise GateFailure(f"tracked raw bytes differ from pinned commit: {relative_text}")
        checked += 1
    return {
        "commit": commit,
        "object_format": object_format,
        "tracked_file_count": checked,
    }


def valid_digest(value: Any) -> bool:
    return isinstance(value, str) and HEX64.fullmatch(value) is not None


def validate_branch_name(name: str) -> None:
    if (
        not SAFE_BRANCH.fullmatch(name)
        or name.startswith("-")
        or ".." in name
        or "@{" in name
        or name.endswith(("/", ".", ".lock"))
        or "//" in name
    ):
        raise InvalidInput("candidate branch name is not a safe local branch name")


def validate_live_plan(
    plan: Mapping[str, Any], session_dir: Path, *, require_port: bool
) -> None:
    """Validate every recovery-relevant field before trusting plan paths/argv."""
    session_dir = session_dir.resolve()
    if (
        plan.get("schema_version") != SCHEMA_VERSION
        or plan.get("kind") != "improve_plan"
        or plan.get("simulated") is not False
    ):
        raise InvalidInput("session plan has an unsupported schema or kind")
    configured_session = Path(str(plan.get("session_dir") or ""))
    if configured_session != session_dir:
        raise InvalidInput("session plan path does not own the requested session")
    assert_owned_path_chain(session_dir, session_dir, label="session path")
    if Path(str(plan.get("source_root") or "")) != ROOT.resolve():
        raise InvalidInput("session plan belongs to another source repository")
    for key in ("base_sha", "candidate_sha"):
        if not isinstance(plan.get(key), str) or HEX40.fullmatch(str(plan[key])) is None:
            raise InvalidInput(f"session plan has an invalid {key}")
    if plan["base_sha"] == plan["candidate_sha"]:
        raise InvalidInput("session plan base and candidate commits are identical")
    for key in ("candidate_branch", "evaluation_branch"):
        value = plan.get(key)
        if not isinstance(value, str):
            raise InvalidInput(f"session plan has an invalid {key}")
        validate_branch_name(value)
    if plan["candidate_branch"] == plan["evaluation_branch"]:
        raise InvalidInput("controller evaluation branch must not be the submitted candidate branch")
    worktree_root = session_dir / "worktrees"
    expected_worktrees = {
        "base_worktree": worktree_root / "base",
        "candidate_worktree": worktree_root / "candidate",
    }
    for key, expected in expected_worktrees.items():
        configured = Path(str(plan.get(key) or ""))
        if configured != expected:
            raise InvalidInput(f"session plan {key} escapes its owned worktree root")
        assert_owned_path_chain(session_dir, configured, label=f"session plan {key}")
    runs = plan.get("runs_per_arm")
    if isinstance(runs, bool) or not isinstance(runs, int):
        raise InvalidInput("session plan has an invalid run count")
    validate_runs(runs)
    if plan.get("schedule") != counterbalanced_schedule(runs):
        raise InvalidInput("session plan schedule is not the frozen counterbalanced schedule")
    target = plan.get("target")
    if not isinstance(target, dict):
        raise InvalidInput("session plan target is invalid")
    if (
        target.get("suite") not in SUPPORTED_TARGET_SUITES
        or target.get("metric") not in SUPPORTED_TARGET_METRICS
        or target.get("direction") not in {"lower_better", "higher_better"}
        or not valid_digest(target.get("catalog_sha256"))
    ):
        raise InvalidInput("session plan target is outside the frozen Phase-B contract")
    if plan.get("board_id") != DEFAULT_BOARD_ID or plan.get("env") != DEFAULT_ENV:
        raise InvalidInput("session plan board/environment contract changed")
    expected_bench_contract = {
        "all_suites": True,
        "camera": True,
        "duration_seconds": RUN_DURATION_SECONDS,
        "replay_duration_seconds": REPLAY_DURATION_SECONDS,
        "profile": PROFILE,
        "segment": SEGMENT,
        "blink_profile": BLINK_PROFILE,
        "baseline_comparison": False,
        "upload_during_bench": False,
    }
    if plan.get("bench_contract") != expected_bench_contract:
        raise InvalidInput("session plan bench contract changed")
    if plan.get("post_flash_settle_seconds") != DEFAULT_SETTLE_SECONDS:
        raise InvalidInput("session plan flash-settle contract changed")
    components = plan.get("controller_components")
    if (
        not isinstance(components, dict)
        or set(components) != {"scripts/improve.py", "scripts/improve_git_dryrun.py"}
        or any(not valid_digest(value) for value in components.values())
        or plan.get("controller_sha256") != sha256_bytes(canonical_bytes(components))
    ):
        raise InvalidInput("session plan controller digest is invalid")
    if not valid_digest(plan.get("dry_run_report_sha256")):
        raise InvalidInput("session plan dry-run proof digest is invalid")
    validate_serial_port(plan.get("port"), require_exists=require_port)
    source = plan.get("source_snapshot")
    if not isinstance(source, dict):
        raise InvalidInput("session plan has no source snapshot")
    if source.get("head") != plan["base_sha"]:
        raise InvalidInput("session base must equal the clean invoking HEAD")
    changed_paths = plan.get("changed_paths")
    if not isinstance(changed_paths, list) or not all(isinstance(item, str) for item in changed_paths):
        raise InvalidInput("session plan changed-path list is invalid")
    validate_candidate_paths(changed_paths)


def validate_runs(runs: int) -> None:
    if isinstance(runs, bool) or runs < MIN_RUNS:
        raise InvalidInput(f"acceptance requires at least {MIN_RUNS} runs per arm")


def counterbalanced_schedule(runs: int) -> list[str]:
    """Return a balanced B,C,C,B sequence that always ends on candidate."""
    validate_runs(runs)
    pairs = runs // 2
    if runs % 2:
        schedule = [item for _ in range(pairs) for item in ("baseline", "candidate", "candidate", "baseline")]
        schedule.extend(("baseline", "candidate"))
    else:
        schedule = [
            item
            for _ in range(max(0, pairs - 1))
            for item in ("baseline", "candidate", "candidate", "baseline")
        ]
        schedule.extend(("baseline", "candidate", "baseline", "candidate"))
    if schedule.count("baseline") != runs or schedule.count("candidate") != runs:
        raise AssertionError("counterbalanced schedule is not balanced")
    if schedule[-1] != "candidate":
        raise AssertionError("counterbalanced schedule must leave accepted candidate installed")
    return schedule


def validate_candidate_paths(paths: Sequence[str]) -> None:
    if not paths:
        raise InvalidInput("candidate has no committed diff")
    firmware_change = False
    for raw in paths:
        path = PurePosixPath(raw)
        if (
            not raw
            or path.is_absolute()
            or "\\" in raw
            or any(part in {"", ".", ".."} for part in path.parts)
            or path.as_posix() != raw
        ):
            raise InvalidInput(f"candidate path is unsafe: {raw!r}")
        if raw in DISPLAY_OPTIMIZATION_PATHS:
            firmware_change = True
        elif not raw.startswith("test/"):
            raise InvalidInput(
                f"candidate path is outside the Phase-B display implementation allowlist: {raw}"
            )
    if not firmware_change:
        raise InvalidInput("candidate must change at least one eligible display implementation file")


def _selector_matches(policy: MetricPolicy, record: Mapping[str, Any], manifest: Mapping[str, Any]) -> bool:
    if policy.run_kind != record.get("run_kind") or policy.metric != record.get("metric"):
        return False
    sources = {
        "suite_or_profile": record.get("suite_or_profile"),
        "stress_class": manifest.get("stress_class"),
        "lane": manifest.get("lane"),
        "source_type": manifest.get("source_type"),
    }
    return all(sources.get(key) == value for key, value in policy.selector.items())


def resolve_target_policy(catalog_path: Path, suite: str, metric: str) -> tuple[MetricPolicy, str]:
    if suite not in SUPPORTED_TARGET_SUITES:
        raise InvalidInput("Phase-B v1 targets only the gated replay suite")
    if not metric or not re.fullmatch(r"[a-z][a-z0-9_]*", metric):
        raise InvalidInput("target metric must be a canonical metric name")
    manifest = dict(EXPECTED_TRACK[suite])
    record = {
        "run_kind": manifest["run_kind"],
        "suite_or_profile": manifest["suite_or_profile"],
        "metric": metric,
    }
    policies = load_catalog(catalog_path)
    matches = [policy for policy in policies if _selector_matches(policy, record, manifest)]
    if len(matches) != 1:
        raise InvalidInput(
            f"target must resolve to exactly one catalog policy; found {len(matches)} for {suite}.{metric}"
        )
    policy = matches[0]
    if policy.direction not in {"lower_better", "higher_better"}:
        raise InvalidInput(f"target direction is not optimizable: {policy.direction}")
    if policy.score_level == "info":
        raise InvalidInput("informational diagnostics cannot be Phase-B acceptance targets")
    if policy.metric not in SUPPORTED_TARGET_METRICS:
        raise InvalidInput(
            "target has no frozen Phase-B instrumentation-ownership contract; "
            f"supported targets: {', '.join(sorted(SUPPORTED_TARGET_METRICS))}"
        )
    if policy.aggregation not in {"last", "min", "max", "delta", "p95"}:
        raise InvalidInput(f"unsupported target aggregation: {policy.aggregation}")
    return policy, sha256_file(catalog_path)


def policy_record(policy: MetricPolicy, suite: str, catalog_sha256: str) -> dict[str, Any]:
    return {
        "suite": suite,
        "metric": policy.metric,
        "run_kind": policy.run_kind,
        "selector": dict(policy.selector),
        "unit": policy.unit,
        "aggregation": policy.aggregation,
        "direction": policy.direction,
        "score_level": policy.score_level,
        "required": policy.required,
        "catalog_sha256": catalog_sha256,
    }


def _percentile(values: Sequence[float], pct: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise GateFailure("cannot aggregate an empty target metric")
    if len(ordered) == 1:
        return ordered[0]
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return ordered[lo]
    fraction = rank - lo
    return ordered[lo] + (ordered[hi] - ordered[lo]) * fraction


def aggregate_values(aggregation: str, values: Sequence[float]) -> float:
    if not values:
        raise GateFailure("target metric has no samples")
    if aggregation == "last":
        return values[-1]
    if aggregation == "min":
        return min(values)
    if aggregation == "max":
        return max(values)
    if aggregation == "delta":
        return values[-1] - values[0] if len(values) > 1 else values[0]
    if aggregation == "p95":
        return _percentile(values, 95.0)
    raise GateFailure(f"unsupported aggregation: {aggregation}")


def extract_target_value(manifest_path: Path, policy: MetricPolicy, suite: str) -> float:
    manifest = read_json_object(manifest_path, f"{suite} metric manifest")
    expected = EXPECTED_TRACK[suite]
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise GateFailure(f"{suite} manifest has unexpected {key}: {manifest.get(key)!r}")
    unsupported = manifest.get("unsupported_metrics") or []
    if not isinstance(unsupported, list):
        raise GateFailure(f"{suite} manifest unsupported_metrics is not a list")
    if policy.metric in {str(item) for item in unsupported}:
        raise GateFailure(f"target metric is unsupported in {suite}")
    metrics_ref = manifest.get("metrics_file")
    if not isinstance(metrics_ref, str) or not metrics_ref:
        raise GateFailure(f"{suite} manifest has no owned metrics file")
    relative = PurePosixPath(metrics_ref)
    if relative.is_absolute() or any(part in {"", ".", ".."} for part in relative.parts):
        raise GateFailure(f"{suite} metrics path is unsafe")
    root = manifest_path.resolve().parent
    metrics_path = root.joinpath(*relative.parts).resolve()
    try:
        metrics_path.relative_to(root)
    except ValueError as exc:
        raise GateFailure(f"{suite} metrics path escapes its artifact") from exc
    values: list[float] = []
    try:
        lines = metrics_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise GateFailure(f"cannot read target metrics: {metrics_path}") from exc
    for line_number, line in enumerate(lines, start=1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as exc:
            raise GateFailure(f"invalid metrics line {line_number}: {exc}") from exc
        if not isinstance(record, dict):
            raise GateFailure(f"invalid metrics line {line_number}: expected object")
        if record.get("metric") != policy.metric:
            continue
        required_fields = {
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
        }
        if not required_fields.issubset(record):
            raise GateFailure(f"target metrics line {line_number} is missing required fields")
        if (
            record.get("run_id") != manifest.get("run_id")
            or record.get("git_sha") != manifest.get("git_sha")
            or record.get("run_kind") != manifest.get("run_kind")
            or record.get("suite_or_profile") != manifest.get("suite_or_profile")
        ):
            raise GateFailure(f"target metrics line {line_number} is not owned by its manifest")
        if not isinstance(record.get("tags"), dict):
            raise GateFailure(f"target metrics line {line_number} has invalid tags")
        matches = _selector_matches(policy, record, manifest)
        if not matches:
            continue
        if record.get("unit") != policy.unit:
            raise GateFailure(
                f"target unit mismatch in {suite}: {record.get('unit')!r} != {policy.unit!r}"
            )
        raw = record.get("value")
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            raise GateFailure(f"target value in {suite} is not a non-boolean number")
        value = float(raw)
        if not math.isfinite(value):
            raise GateFailure(f"target value in {suite} is not finite")
        values.append(value)
    if not values:
        raise GateFailure(f"target metric {suite}.{policy.metric} is missing")
    aggregate = aggregate_values(policy.aggregation, values)
    scoring_ref = manifest.get("scoring_file")
    if not isinstance(scoring_ref, str) or not scoring_ref:
        raise GateFailure(f"{suite} manifest has no owned scoring file")
    scoring_relative = PurePosixPath(scoring_ref)
    if scoring_relative.is_absolute() or any(
        part in {"", ".", ".."} for part in scoring_relative.parts
    ):
        raise GateFailure(f"{suite} scoring path is unsafe")
    scoring_path = root.joinpath(*scoring_relative.parts).resolve()
    try:
        scoring_path.relative_to(root)
    except ValueError as exc:
        raise GateFailure(f"{suite} scoring path escapes its artifact") from exc
    scoring = read_json_object(scoring_path, f"{suite} scoring result")
    scoring_rows = scoring.get("metrics")
    if not isinstance(scoring_rows, list):
        raise GateFailure(f"{suite} scoring result has no metric list")
    matches = [
        row
        for row in scoring_rows
        if isinstance(row, dict)
        and row.get("metric") == policy.metric
        and row.get("suite_or_profile") == manifest.get("suite_or_profile")
    ]
    if len(matches) != 1:
        raise GateFailure(f"{suite} scoring result does not own exactly one target aggregate")
    scored = matches[0].get("current_value")
    if isinstance(scored, bool) or not isinstance(scored, (int, float)) or not math.isfinite(float(scored)):
        raise GateFailure(f"{suite} scoring target aggregate is invalid")
    if not math.isclose(float(scored), aggregate, rel_tol=0.0, abs_tol=1e-9):
        raise GateFailure(f"{suite} target aggregate disagrees with scoring evidence")
    return aggregate


def sample_summary(values: Sequence[float]) -> dict[str, float | int]:
    if not values:
        raise InvalidInput("cannot summarize an empty arm")
    return {
        "count": len(values),
        "min": min(values),
        "median": float(statistics.median(values)),
        "max": max(values),
        "range": max(values) - min(values),
    }


def decide_improvement(
    baseline: Sequence[float], candidate: Sequence[float], direction: str, runs: int
) -> dict[str, Any]:
    validate_runs(runs)
    if len(baseline) != runs or len(candidate) != runs:
        raise InvalidInput("both arms must contain exactly N target values")
    all_values = [*baseline, *candidate]
    if any(isinstance(value, bool) or not isinstance(value, (int, float)) for value in all_values):
        raise InvalidInput("all target values must be non-boolean numbers")
    numeric = [float(value) for value in all_values]
    if not all(math.isfinite(value) for value in numeric):
        raise InvalidInput("all target values must be finite")
    baseline_values = numeric[:runs]
    candidate_values = numeric[runs:]
    baseline_summary = sample_summary(baseline_values)
    candidate_summary = sample_summary(candidate_values)
    if direction == "lower_better":
        gap = float(baseline_summary["min"]) - float(candidate_summary["max"])
        accepted = gap > 0
    elif direction == "higher_better":
        gap = float(candidate_summary["min"]) - float(baseline_summary["max"])
        accepted = gap > 0
    else:
        raise InvalidInput(f"unsupported target direction: {direction}")
    return {
        "accepted": accepted,
        "rule": "strict_empirical_envelope_separation",
        "all_cross_arm_comparisons_favor_candidate": accepted,
        "baseline": baseline_summary,
        "candidate": candidate_summary,
        "separation_gap": gap,
        "median_change": float(candidate_summary["median"]) - float(baseline_summary["median"]),
    }


def validate_memory_report(payload: Mapping[str, Any], label: str) -> dict[str, dict[str, int]]:
    if payload.get("env") != DEFAULT_ENV or not isinstance(payload.get("memory"), dict):
        raise ResourceFailure(f"{label} memory report has an invalid environment or memory map")
    rows: dict[str, dict[str, int]] = {}
    for name, raw in payload["memory"].items():
        if not isinstance(name, str) or not isinstance(raw, dict):
            raise ResourceFailure(f"{label} memory report has an invalid region")
        row: dict[str, int] = {}
        for field in ("used_bytes", "limit_bytes", "headroom_bytes"):
            value = raw.get(field)
            if isinstance(value, bool) or not isinstance(value, int):
                raise ResourceFailure(f"{label} {name}.{field} is not an integer")
            row[field] = value
        if row["used_bytes"] < 0 or row["limit_bytes"] <= 0 or row["headroom_bytes"] < 0:
            raise ResourceFailure(f"{label} {name} exceeds or corrupts its resource budget")
        if row["used_bytes"] + row["headroom_bytes"] != row["limit_bytes"]:
            raise ResourceFailure(f"{label} {name} resource arithmetic is inconsistent")
        rows[name] = row
    if not {"flash", "ram"}.issubset(rows):
        raise ResourceFailure(f"{label} memory report must include flash and ram")
    return rows


def compare_memory_reports(
    baseline_payload: Mapping[str, Any], candidate_payload: Mapping[str, Any]
) -> dict[str, Any]:
    baseline = validate_memory_report(baseline_payload, "baseline")
    candidate = validate_memory_report(candidate_payload, "candidate")
    if set(baseline) != set(candidate):
        raise ResourceFailure("candidate changed the reported memory-region set")
    deltas: dict[str, Any] = {}
    for name in sorted(baseline):
        if baseline[name]["limit_bytes"] != candidate[name]["limit_bytes"]:
            raise ResourceFailure(f"candidate changed the {name} resource limit")
        deltas[name] = {
            "baseline_used_bytes": baseline[name]["used_bytes"],
            "candidate_used_bytes": candidate[name]["used_bytes"],
            "delta_used_bytes": candidate[name]["used_bytes"] - baseline[name]["used_bytes"],
            "limit_bytes": baseline[name]["limit_bytes"],
            "candidate_headroom_bytes": candidate[name]["headroom_bytes"],
        }
    return deltas


def tree_digest(root: Path) -> dict[str, Any]:
    if not root.exists():
        return {"sha256": sha256_bytes(canonical_bytes([])), "files": []}
    files: list[dict[str, Any]] = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise InvalidInput(f"deployed asset tree contains a symlink: {path}")
        if path.is_file():
            files.append(
                {
                    "path": path.relative_to(root).as_posix(),
                    "size_bytes": path.stat().st_size,
                    "sha256": sha256_file(path),
                }
            )
    return {"sha256": sha256_bytes(canonical_bytes(files)), "files": files}


def owned_tree_size(root: Path) -> int:
    total = 0
    if not root.exists():
        return 0
    for path in root.rglob("*"):
        if path.is_symlink():
            raise GateFailure(f"owned evidence tree contains a symlink: {path}")
        if path.is_file():
            total += path.stat().st_size
    return total


def source_snapshot(root: Path) -> dict[str, Any]:
    return {
        "head": run_capture(["git", "rev-parse", "HEAD"], cwd=root),
        "status": run_capture(["git", "status", "--porcelain=v1", "-uall"], cwd=root),
        "branch": run_capture(["git", "symbolic-ref", "--short", "HEAD"], cwd=root),
    }


def scenario_fingerprints() -> dict[str, str]:
    return {
        suite: scenario_manifest(
            suite=suite,
            duration_seconds=REPLAY_DURATION_SECONDS if suite == "replay" else RUN_DURATION_SECONDS,
            profile=PROFILE,
            segment=SEGMENT,
            blink_profile=BLINK_PROFILE if suite == "replay" else None,
        )["fingerprint"]
        for suite in ("core", "display", "replay")
    }


def identity_for_worktree(root: Path) -> dict[str, Any]:
    ref_status, repository_ref, _ = run_capture_optional(
        ["git", "symbolic-ref", "--quiet", "--short", "HEAD"], cwd=root
    )
    return {
        "product_fingerprint": current_product_fingerprint(root),
        "grader_fingerprint": current_grader_fingerprint(root),
        "hardware_scoring_fingerprint": current_hardware_scoring_fingerprint(root),
        "scenario_fingerprints": scenario_fingerprints(),
        "repository_sha": run_capture(["git", "rev-parse", "HEAD"], cwd=root),
        "repository_ref": repository_ref if ref_status == 0 else "HEAD",
        "worktree_clean": run_capture(
            ["git", "status", "--porcelain=v1", "-uall"], cwd=root
        )
        == "",
    }


def validate_identity_pair(base: Mapping[str, Any], candidate: Mapping[str, Any]) -> None:
    for name in ("grader_fingerprint", "hardware_scoring_fingerprint"):
        if not valid_digest(base.get(name)) or base.get(name) != candidate.get(name):
            raise InvalidInput(f"candidate changed or invalidated {name}")
    if base.get("scenario_fingerprints") != candidate.get("scenario_fingerprints"):
        raise InvalidInput("candidate changed scenario identity")
    if not valid_digest(base.get("product_fingerprint")) or not valid_digest(
        candidate.get("product_fingerprint")
    ):
        raise InvalidInput("base or candidate product identity is invalid")
    if base.get("product_fingerprint") == candidate.get("product_fingerprint"):
        raise InvalidInput("live candidate did not change the deployed product identity")


def build_bench_command(port: str, artifact_root: Path) -> list[str]:
    return [
        "./bench.sh",
        "--all",
        "--camera",
        "--no-upload",
        "--no-baseline",
        "--duration-seconds",
        str(RUN_DURATION_SECONDS),
        "--replay-duration-seconds",
        str(REPLAY_DURATION_SECONDS),
        "--segment",
        SEGMENT,
        "--blink-profile",
        BLINK_PROFILE,
        "--port",
        port,
        "--board-id",
        DEFAULT_BOARD_ID,
        "--artifact-root",
        str(artifact_root),
    ]


def validate_bench_result(
    result: Mapping[str, Any],
    *,
    arm: str,
    arm_index: int,
    plan: Mapping[str, Any],
    identities: Mapping[str, Any],
) -> None:
    if arm not in {"baseline", "candidate"}:
        raise GateFailure(f"unknown bench arm: {arm}")
    expected_sha = plan["base_sha" if arm == "baseline" else "candidate_sha"]
    if (
        result.get("schema_version") != 5
        or result.get("kind") != "bench_result"
        or result.get("run_dir") != "."
        or result.get("result") != "PASS"
        or result.get("git_sha") != expected_sha
        or result.get("git_worktree_clean") is not True
        or result.get("product_fingerprint") != identities.get("product_fingerprint")
        or result.get("grader_fingerprint") != identities.get("grader_fingerprint")
        or result.get("hardware_scoring_fingerprint")
        != identities.get("hardware_scoring_fingerprint")
    ):
        raise GateFailure(f"{arm} run {arm_index} is not an identity-owned canonical PASS")
    windows = result.get("windows")
    if not isinstance(windows, list) or {
        item.get("suite") for item in windows if isinstance(item, dict)
    } != {"core", "display", "replay"}:
        raise GateFailure(f"{arm} run {arm_index} does not contain the exact full suite set")
    if any(
        not isinstance(item, dict)
        or item.get("result") != "PASS"
        or item.get("window_schema_version") != 3
        for item in windows
    ):
        raise GateFailure(f"{arm} run {arm_index} contains a non-PASS current suite")
    expected_scenarios = identities.get("scenario_fingerprints")
    if not isinstance(expected_scenarios, dict):
        raise GateFailure("expected scenario identity map is missing")
    for window in windows:
        suite = str(window["suite"])
        if window.get("scenario_fingerprint") != expected_scenarios.get(suite):
            raise GateFailure(f"{arm} run {arm_index} owns a stale {suite} scenario")


def validate_suite_artifacts(
    run_dir: Path,
    suite: str,
    *,
    arm: str,
    plan: Mapping[str, Any],
    identities: Mapping[str, Any],
) -> dict[str, Any]:
    if suite not in EXPECTED_TRACK:
        raise GateFailure(f"unknown suite artifact set: {suite}")
    run_root = Path(os.path.abspath(run_dir))
    root = run_root / suite
    assert_owned_path_chain(run_root, root, label=f"{suite} artifact root")
    try:
        root.relative_to(run_root)
    except ValueError as exc:
        raise GateFailure(f"{suite} artifact root escapes its bench run") from exc
    if not root.is_dir() or root.is_symlink():
        raise GateFailure(f"{suite} artifact root is unavailable or unsafe")
    paths = {
        name: root / name
        for name in ("identity.json", "manifest.json", "metrics.ndjson", "scoring.json")
    }
    for path in paths.values():
        assert_owned_path_chain(run_root, path, label=f"{suite} artifact file")
    if any(
        not path.is_file()
        or path.is_symlink()
        or not stat.S_ISREG(path.lstat().st_mode)
        for path in paths.values()
    ):
        raise GateFailure(f"{suite} artifact set is incomplete or contains a symlink")
    manifest = read_json_object(paths["manifest.json"], f"{suite} metric manifest")
    identity = read_json_object(paths["identity.json"], f"{suite} identity")
    expected_sha = str(plan["base_sha" if arm == "baseline" else "candidate_sha"])
    expected_track = EXPECTED_TRACK[suite]
    expected_scenario = identities["scenario_fingerprints"][suite]
    manifest_contract = {
        "schema_version": 1,
        "git_sha": expected_sha,
        "git_ref": identities.get("repository_ref"),
        "product_fingerprint": identities.get("product_fingerprint"),
        "grader_fingerprint": identities.get("grader_fingerprint"),
        "hardware_scoring_fingerprint": identities.get("hardware_scoring_fingerprint"),
        "scenario_fingerprint": expected_scenario,
        "run_kind": expected_track["run_kind"],
        "board_id": DEFAULT_BOARD_ID,
        "env": "perf-csv-import",
        "lane": expected_track["lane"],
        "suite_or_profile": expected_track["suite_or_profile"],
        "stress_class": expected_track["stress_class"],
        "source_type": expected_track["source_type"],
        "result": "NO_BASELINE",
        "base_result": "PASS",
        "metrics_file": "metrics.ndjson",
        "scoring_file": "scoring.json",
    }
    for key, expected in manifest_contract.items():
        if manifest.get(key) != expected:
            raise GateFailure(
                f"{suite} manifest {key} is not owned by the planned {arm} identity"
            )
    run_id = manifest.get("run_id")
    if not isinstance(run_id, str) or not run_id:
        raise GateFailure(f"{suite} manifest has no canonical run ID")
    traceability = identity.get("traceability")
    if (
        identity.get("schema_version") != 2
        or identity.get("kind") != "bench_identity"
        or identity.get("product_fingerprint") != identities.get("product_fingerprint")
        or identity.get("grader_fingerprint") != identities.get("grader_fingerprint")
        or identity.get("hardware_scoring_fingerprint")
        != identities.get("hardware_scoring_fingerprint")
        or identity.get("scenario_fingerprint") != expected_scenario
        or not isinstance(traceability, dict)
        or traceability.get("repository_sha") != expected_sha
        or traceability.get("repository_ref") != identities.get("repository_ref")
        or traceability.get("worktree_clean") is not True
    ):
        raise GateFailure(f"{suite} identity is not owned by the planned {arm} revision")
    source_input = manifest.get("source_input")
    if not isinstance(source_input, str) or not source_input:
        raise GateFailure(f"{suite} manifest has no source input")
    source_path = Path(os.path.abspath(source_input))
    try:
        source_path.relative_to(root)
    except ValueError as exc:
        raise GateFailure(f"{suite} source input escapes its owned artifact root") from exc
    assert_owned_path_chain(root, source_path, label=f"{suite} source input")
    if not source_path.is_file() or source_path.is_symlink():
        raise GateFailure(f"{suite} source input is missing or unsafe")

    required_metric_fields = {
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
    }
    metric_count = 0
    try:
        metric_lines = paths["metrics.ndjson"].read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise GateFailure(f"{suite} metrics evidence cannot be read") from exc
    for line_number, raw_line in enumerate(metric_lines, start=1):
        if not raw_line.strip():
            continue
        try:
            record = json.loads(raw_line)
        except json.JSONDecodeError as exc:
            raise GateFailure(f"{suite} metrics line {line_number} is invalid: {exc}") from exc
        if not isinstance(record, dict) or not required_metric_fields.issubset(record):
            raise GateFailure(f"{suite} metrics line {line_number} has an incomplete schema")
        if (
            record.get("schema_version") != 1
            or record.get("run_id") != run_id
            or record.get("git_sha") != expected_sha
            or record.get("run_kind") != manifest.get("run_kind")
            or record.get("suite_or_profile") != manifest.get("suite_or_profile")
        ):
            raise GateFailure(
                f"{suite} metrics line {line_number} is not owned by its manifest"
            )
        raw_value = record.get("value")
        if (
            not isinstance(record.get("metric"), str)
            or not record.get("metric")
            or not isinstance(record.get("unit"), str)
            or not record.get("unit")
            or not isinstance(record.get("tags"), dict)
            or isinstance(raw_value, bool)
            or not isinstance(raw_value, (int, float))
            or not math.isfinite(float(raw_value))
        ):
            raise GateFailure(f"{suite} metrics line {line_number} has invalid typed values")
        metric_count += 1
    if metric_count == 0:
        raise GateFailure(f"{suite} metrics evidence is empty")

    scoring = read_json_object(paths["scoring.json"], f"{suite} scoring result")
    scoring_manifest = scoring.get("manifest")
    scoring_summary = scoring.get("summary")
    if (
        scoring.get("schema_version") != SCORING_SCHEMA_VERSION
        or scoring.get("result") != "NO_BASELINE"
        or not isinstance(scoring_manifest, dict)
        or scoring_manifest.get("path") != "manifest.json"
        or any(
            scoring_manifest.get(key) != manifest.get(key)
            for key in (
                "run_id",
                "git_sha",
                "git_ref",
                "run_kind",
                "board_id",
                "env",
                "lane",
                "suite_or_profile",
                "stress_class",
                "hardware_scoring_fingerprint",
                "base_result",
                "source_type",
            )
        )
        or not isinstance(scoring_summary, dict)
        or scoring_summary.get("hard_failures") != 0
        or scoring_summary.get("advisory_failures") != 0
    ):
        raise GateFailure(f"{suite} scoring evidence is not owned by its manifest")
    return {
        "manifest": manifest,
        "identity": identity,
        "scoring": scoring,
        "paths": paths,
        "metric_count": metric_count,
    }


def build_flash_command(port: str) -> list[str]:
    return [
        "pio",
        "run",
        "-e",
        DEFAULT_ENV,
        "-t",
        "nobuild",
        "-t",
        "upload",
        "--upload-port",
        port,
        "--disable-auto-clean",
    ]


def assert_firmware_only_command(argv: Sequence[str]) -> None:
    forbidden = {"-f", "--upload-fs", "uploadfs", "--all"}
    if any(item in forbidden for item in argv):
        raise AssertionError(f"firmware-only command contains a filesystem upload token: {argv}")


class Adapter(Protocol):
    context: dict[str, Any]
    operations: list[str]

    def prepare(self) -> dict[str, Any]: ...

    def flash(self, arm: str, *, recovery: bool = False) -> None: ...

    def collect(self, arm: str, arm_index: int, sequence: int) -> dict[str, Any]: ...

    def validate_regressions(self, runs: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]: ...

    def finalize_evaluation(self) -> dict[str, Any]: ...

    def check_stop(self) -> None: ...


def verify_owned_citation(
    session_dir: Path,
    path_text: Any,
    digest: Any,
    *,
    label: str,
    expected_path: Path | None = None,
) -> Path:
    if not isinstance(path_text, str) or not path_text or not valid_digest(digest):
        raise GateFailure(f"{label} citation is incomplete")
    path = Path(os.path.abspath(path_text))
    if expected_path is not None and path != Path(os.path.abspath(expected_path)):
        raise GateFailure(f"{label} citation points at an unexpected file")
    assert_owned_path_chain(session_dir, path, label=label)
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise GateFailure(f"{label} citation is unavailable: {path}") from exc
    if (
        not stat.S_ISREG(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode)
        or metadata.st_nlink != 1
        or sha256_owned_regular_file(path, label=label) != digest
    ):
        raise GateFailure(f"{label} citation bytes or ownership changed: {path}")
    return path


def verify_decision_evidence_files(
    store: EvidenceStore,
    check_stop: Callable[[], None] | None = None,
) -> None:
    def checkpoint() -> None:
        if check_stop is not None:
            check_stop()

    checkpoint()
    session_dir = getattr(store, "session_dir", None)
    if not isinstance(session_dir, Path):
        return
    session_dir = Path(os.path.abspath(session_dir))
    runs = list(store.state.get("runs") or [])
    for index, run in enumerate(runs, start=1):
        checkpoint()
        if not isinstance(run, dict):
            raise GateFailure(f"run evidence {index} is not an object")
        if run.get("simulated") is True:
            continue
        verify_owned_citation(
            session_dir,
            run.get("bench_log"),
            run.get("bench_log_sha256"),
            label=f"run {index} bench log",
        )
        run_dir_text = run.get("run_dir")
        run_dir = Path(os.path.abspath(run_dir_text)) if isinstance(run_dir_text, str) else None
        if run_dir is not None:
            assert_owned_path_chain(session_dir, run_dir, label=f"run {index} directory")
        verified_paths: dict[str, Path] = {}
        for key in ("bench_result", "qualification"):
            path_text = run.get(key)
            if path_text is not None:
                verified_paths[key] = verify_owned_citation(
                    session_dir,
                    path_text,
                    run.get(f"{key}_sha256"),
                    label=f"run {index} {key}",
                )
        if run.get("result") == "PASS":
            if set(verified_paths) != {"bench_result", "qualification"}:
                raise GateFailure(f"PASS run {index} is missing qualification evidence")
            qualification = read_json_object(
                verified_paths["qualification"], f"run {index} qualification"
            )
            try:
                validate_qualification_record(qualification)
                validate_qualification_evidence(qualification)
            except Exception as exc:
                raise GateFailure(
                    f"run {index} qualification evidence changed or became unavailable: {exc}"
                ) from exc
            checkpoint()
            qualification_evidence = qualification.get("evidence")
            if (
                not isinstance(qualification_evidence, dict)
                or Path(
                    os.path.abspath(str(qualification_evidence.get("bench_result") or ""))
                )
                != verified_paths["bench_result"]
                or qualification_evidence.get("bench_result_sha256")
                != run.get("bench_result_sha256")
            ):
                raise GateFailure(f"run {index} qualification no longer owns its bench result")
        suites = run.get("suite_artifacts")
        if suites is None:
            continue
        if run_dir is None or not isinstance(suites, dict):
            raise GateFailure(f"run {index} suite citations are incomplete")
        if set(suites) != {"core", "display", "replay"}:
            raise GateFailure(f"run {index} suite citation set changed")
        for suite in ("core", "display", "replay"):
            checkpoint()
            artifacts = suites.get(suite)
            if not isinstance(artifacts, dict) or set(artifacts) != {
                "identity.json",
                "manifest.json",
                "metrics.ndjson",
                "scoring.json",
            }:
                raise GateFailure(f"run {index} {suite} citation set changed")
            for name, citation in artifacts.items():
                if not isinstance(citation, dict):
                    raise GateFailure(f"run {index} {suite}/{name} citation is invalid")
                verify_owned_citation(
                    session_dir,
                    citation.get("path"),
                    citation.get("sha256"),
                    label=f"run {index} {suite}/{name}",
                    expected_path=run_dir / suite / name,
                )

    for index, record in enumerate(store.state.get("cross_arm_scores") or [], start=1):
        checkpoint()
        if not isinstance(record, dict):
            raise GateFailure(f"cross-arm score {index} is not an object")
        if record.get("simulated") is True:
            continue
        verify_owned_citation(
            session_dir,
            record.get("scoring"),
            record.get("scoring_sha256"),
            label=f"cross-arm score {index}",
        )
        inputs = record.get("inputs")
        catalog = inputs.get("catalog") if isinstance(inputs, dict) else None
        if not isinstance(catalog, dict):
            raise GateFailure(f"cross-arm score {index} has no catalog citation")
        verify_owned_citation(
            session_dir,
            catalog.get("path"),
            catalog.get("sha256"),
            label=f"cross-arm score {index} catalog",
        )

    for index, citation in enumerate(store.state.get("command_logs") or [], start=1):
        checkpoint()
        if not isinstance(citation, dict):
            raise GateFailure(f"command log citation {index} is not an object")
        path = verify_owned_citation(
            session_dir,
            citation.get("path"),
            citation.get("sha256"),
            label=f"command log citation {index}",
        )
        if citation.get("size_bytes") != path.stat().st_size:
            raise GateFailure(f"command log citation {index} size changed")

    context = store.state.get("context")
    if isinstance(context, dict) and context:
        patch = context.get("patch")
        if not isinstance(patch, dict):
            raise GateFailure("candidate patch evidence is missing")
        patch_path = verify_owned_citation(
            session_dir,
            patch.get("path"),
            patch.get("sha256"),
            label="candidate patch",
            expected_path=session_dir / "candidate.patch",
        )
        if patch.get("size_bytes") != patch_path.stat().st_size:
            raise GateFailure("candidate patch size changed")
        builds = context.get("builds")
        if not isinstance(builds, dict) or set(builds) != {"baseline", "candidate"}:
            raise GateFailure("base/candidate build evidence is incomplete")
        for arm in ("baseline", "candidate"):
            checkpoint()
            build = builds.get(arm)
            if not isinstance(build, dict):
                raise GateFailure(f"{arm} build evidence is invalid")
            for key, expected in (
                ("build_log", session_dir / "builds" / arm / "build.log"),
                (
                    "memory_report",
                    session_dir / "builds" / arm / "memory" / f"{DEFAULT_ENV}.json",
                ),
                (
                    "flash_package_log",
                    session_dir / "logs" / f"{arm}-flash-package.log",
                ),
            ):
                verify_owned_citation(
                    session_dir,
                    build.get(key),
                    build.get(f"{key}_sha256"),
                    label=f"{arm} {key}",
                    expected_path=expected,
                )
            images = build.get("images")
            if not isinstance(images, dict) or set(images) != {
                "bootloader.bin",
                "partitions.bin",
                "firmware.bin",
            }:
                raise GateFailure(f"{arm} preserved image evidence is incomplete")
            for name, image in images.items():
                if not isinstance(image, dict):
                    raise GateFailure(f"{arm} {name} image citation is invalid")
                path = verify_owned_citation(
                    session_dir,
                    image.get("path"),
                    image.get("sha256"),
                    label=f"{arm} preserved {name}",
                    expected_path=session_dir / "builds" / arm / "images" / name,
                )
                if image.get("size_bytes") != path.stat().st_size:
                    raise GateFailure(f"{arm} preserved {name} size changed")

    for index, attempt in enumerate(store.state.get("flash_attempts") or [], start=1):
        checkpoint()
        if not isinstance(attempt, dict):
            raise GateFailure(f"flash attempt {index} is not an object")
        if attempt.get("status") != "COMPLETE":
            raise GateFailure(f"flash attempt {index} is not complete")
        verify_owned_citation(
            session_dir,
            attempt.get("log"),
            attempt.get("log_sha256"),
            label=f"flash attempt {index} log",
        )
        images = attempt.get("images")
        if not isinstance(images, dict):
            raise GateFailure(f"flash attempt {index} has no image citations")
        for name, image in images.items():
            if not isinstance(image, dict):
                raise GateFailure(f"flash attempt {index} {name} citation is invalid")
            verify_owned_citation(
                session_dir,
                image.get("preserved_path"),
                image.get("sha256"),
                label=f"flash attempt {index} {name}",
            )
    checkpoint()


def decision_payload(
    result: str,
    plan: Mapping[str, Any],
    store: EvidenceStore,
    *,
    analysis: Mapping[str, Any] | None = None,
    reason: str = "",
    cleanup: Sequence[str] = (),
    require_valid_evidence: bool = True,
    check_stop: Callable[[], None] | None = None,
) -> dict[str, Any]:
    evidence_integrity: dict[str, Any] = {"status": "PASS"}
    if plan.get("simulated") is not True:
        try:
            verify_decision_evidence_files(store, check_stop)
        except GateFailure as exc:
            if require_valid_evidence:
                raise
            evidence_integrity = {"status": "FAIL", "reason": str(exc)}
    if check_stop is not None:
        check_stop()
    runs = list(store.state.get("runs") or [])
    context = store.state.get("context") if isinstance(store.state.get("context"), dict) else {}
    cited_context = {
        key: context.get(key)
        for key in ("patch", "identities", "builds", "resource_deltas", "assets")
        if key in context
    }
    plan_path = getattr(store, "session_dir", None)
    plan_file = plan_path / "plan.json" if isinstance(plan_path, Path) else None
    dry_run_file = (
        plan_path / "dry_run_report.json" if isinstance(plan_path, Path) else None
    )
    plan_citation: dict[str, Any]
    dry_run_citation: dict[str, Any]
    if isinstance(plan_file, Path) and plan.get("simulated") is not True:
        expected_plan_hash = sha256_bytes(
            json.dumps(plan, indent=2, sort_keys=True).encode("utf-8") + b"\n"
        )
        expected_dry_hash = str(plan.get("dry_run_report_sha256") or "")
        try:
            actual_plan_hash = sha256_owned_regular_file(
                plan_file, label="immutable improvement plan"
            )
            if not isinstance(dry_run_file, Path):
                raise GateFailure("immutable dry-run proof path is missing")
            actual_dry_hash = sha256_owned_regular_file(
                dry_run_file, label="immutable dry-run proof"
            )
        except GateFailure as exc:
            if require_valid_evidence:
                raise
            actual_plan_hash = "invalid"
            actual_dry_hash = "invalid"
            evidence_integrity = {"status": "FAIL", "reason": str(exc)}
        plan_citation = {
            "path": str(plan_file),
            "sha256": actual_plan_hash,
            "expected_sha256": expected_plan_hash,
        }
        dry_run_citation = {
            "path": str(dry_run_file),
            "sha256": actual_dry_hash,
            "expected_sha256": expected_dry_hash,
        }
        if actual_plan_hash != expected_plan_hash or actual_dry_hash != expected_dry_hash:
            detail = "immutable plan or dry-run proof changed before decision publication"
            if require_valid_evidence:
                raise GateFailure(detail)
            evidence_integrity = {"status": "FAIL", "reason": detail}
    else:
        plan_citation = {"sha256": sha256_bytes(canonical_bytes(plan))}
        dry_run_citation = {"sha256": plan.get("dry_run_report_sha256", "")}
    command_logs = list(store.state.get("command_logs") or [])
    payload = {
        "schema_version": SCHEMA_VERSION,
        "kind": "improve_decision",
        "result": result,
        "reason": reason,
        "simulated": bool(plan.get("simulated")),
        "session_id": plan.get("session_id", "simulated"),
        "base_sha": plan.get("base_sha", "simulated"),
        "candidate_sha": plan.get("candidate_sha", "simulated"),
        "evaluation_branch": plan.get("evaluation_branch", "simulated"),
        "plan": plan_citation,
        "dry_run_proof": dry_run_citation,
        "target": dict(plan["target"]),
        "runs_per_arm": int(plan["runs_per_arm"]),
        "schedule": list(plan["schedule"]),
        "analysis": dict(analysis or {}),
        "evidence": runs,
        "evidence_run_count": len(runs),
        "build_and_resource_evidence": cited_context,
        "context_sha256": sha256_bytes(canonical_bytes(cited_context)),
        "flash_attempts": list(store.state.get("flash_attempts") or []),
        "disk_checks": list(store.state.get("disk_checks") or []),
        "cross_arm_scores": list(store.state.get("cross_arm_scores") or []),
        "command_logs": command_logs,
        "cleanup": list(cleanup),
        "evidence_integrity": evidence_integrity,
        "last_event_sha256": store.state.get("last_event_sha256", ""),
    }
    if check_stop is not None:
        check_stop()
    return payload


def finalize_pending_flash_evidence(store: EvidenceStore) -> None:
    attempts = list(store.state.get("flash_attempts") or [])
    changed = False
    for raw in attempts:
        if not isinstance(raw, dict) or raw.get("status") != "STARTED":
            continue
        path_text = raw.get("log")
        if isinstance(path_text, str):
            path = Path(path_text)
            if path.is_file():
                raw["log_sha256"] = sha256_file(path)
        raw["status"] = "FAILED_OR_INTERRUPTED"
        changed = True
    if changed:
        store.update(flash_attempts=attempts)


def execute_experiment(
    plan: Mapping[str, Any], adapter: Adapter, store: EvidenceStore
) -> dict[str, Any]:
    """Execute the common live/dry decision engine."""
    candidate_may_be_installed = bool(store.state.get("candidate_may_be_installed"))
    prepared = False
    cleanup_messages: list[str] = []
    current_arm = str(store.state.get("current_firmware") or "unknown")
    try:
        store.event("prepare_intent")
        # A partially completed prepare can already own an evaluation branch.
        # Mark it cleanup-eligible before delegating rather than only after the
        # adapter returns successfully.
        prepared = True
        context = adapter.prepare()
        store.update(
            status="PREPARED",
            context=context,
            evaluation_cleanup_required=True,
        )
        store.event("prepare_complete", {"context_sha256": sha256_bytes(canonical_bytes(context))})
        arm_counts = {"baseline": 0, "candidate": 0}
        for sequence, arm in enumerate(plan["schedule"], start=1):
            if arm != current_arm:
                if arm == "candidate":
                    candidate_may_be_installed = True
                    store.update(candidate_may_be_installed=True)
                store.update(status=f"{arm.upper()}_FLASH_INTENT", restore_required=True)
                store.event("flash_intent", {"arm": arm, "sequence": sequence})
                adapter.flash(arm)
                current_arm = arm
                if arm == "baseline":
                    candidate_may_be_installed = False
                store.update(
                    status=f"{arm.upper()}_FLASHED",
                    current_firmware=arm,
                    candidate_may_be_installed=candidate_may_be_installed,
                    restore_required=arm == "candidate",
                )
                store.event("flash_complete", {"arm": arm, "sequence": sequence})
            arm_counts[arm] += 1
            store.update(status=f"{arm.upper()}_RUN_INTENT")
            store.event(
                "run_intent",
                {"arm": arm, "arm_index": arm_counts[arm], "sequence": sequence},
            )
            evidence = adapter.collect(arm, arm_counts[arm], sequence)
            runs = list(store.state.get("runs") or [])
            runs.append(evidence)
            passed = evidence.get("result") == "PASS"
            store.update(
                status=f"{arm.upper()}_RUN_{'COMPLETE' if passed else 'FAILED'}",
                runs=runs,
            )
            store.event(
                "run_complete" if passed else "run_failed",
                {
                    "arm": arm,
                    "arm_index": arm_counts[arm],
                    "sequence": sequence,
                    "bench_result_sha256": evidence.get("bench_result_sha256", ""),
                    "result": evidence.get("result"),
                },
            )
            if not passed:
                detail = str(evidence.get("validation_error") or evidence.get("result") or "unknown")
                if evidence.get("controller_interrupted") is True:
                    raise ControllerInterrupted(detail)
                raise GateFailure(f"{arm} run {arm_counts[arm]} did not PASS: {detail}")
        baseline_values = [
            float(run["target_value"]) for run in store.state["runs"] if run["arm"] == "baseline"
        ]
        candidate_values = [
            float(run["target_value"]) for run in store.state["runs"] if run["arm"] == "candidate"
        ]
        run_ids = [str(run.get("metric_run_id") or "") for run in store.state["runs"]]
        if any(not run_id for run_id in run_ids) or len(set(run_ids)) != len(run_ids):
            raise GateFailure("experiment does not own 2N distinct metric run IDs")
        cross_arm_scores = adapter.validate_regressions(store.state["runs"])
        adapter.check_stop()
        store.update(cross_arm_scores=cross_arm_scores)
        store.event(
            "cross_arm_regression_complete",
            {"score_count": len(cross_arm_scores), "scores_sha256": sha256_bytes(canonical_bytes(cross_arm_scores))},
        )
        failed_scores = [score for score in cross_arm_scores if score.get("result") != "PASS"]
        if failed_scores:
            raise GateFailure(
                f"candidate introduced {len(failed_scores)} same-suite regression gate failure(s)"
            )
        analysis = decide_improvement(
            baseline_values,
            candidate_values,
            str(plan["target"]["direction"]),
            int(plan["runs_per_arm"]),
        )
        store.update(status="ANALYZED", analysis=analysis)
        store.event("analysis_complete", analysis)
        if analysis["accepted"]:
            adapter.check_stop()
            release_store_recovery_reserve(store)
            adapter.check_stop()
            store.event("accepted")
            adapter.check_stop()
            # The candidate is the intended installed state only after an
            # immutable decision exists. If a crash occurs after these flags
            # clear but before publication, recovery sees the still-installed
            # candidate, restores baseline, and (because it restored) reverts
            # the evaluation branch even when this cleanup flag is false.
            store.update(restore_required=False, evaluation_cleanup_required=False)
            decision = decision_payload(
                "ACCEPTED",
                plan,
                store,
                analysis=analysis,
                check_stop=adapter.check_stop,
            )
            adapter.check_stop()
            store.finish(decision)
            return decision

        if candidate_may_be_installed:
            release_store_recovery_reserve(store)
            store.update(status="RESTORE_INTENT")
            store.event("restore_intent", {"reason": "no_improvement"})
            adapter.flash("baseline", recovery=True)
            candidate_may_be_installed = False
            store.update(
                status="BASE_RESTORED",
                current_firmware="baseline",
                candidate_may_be_installed=False,
                restore_required=False,
            )
            store.event("restore_complete")
            cleanup_messages.append("baseline firmware restored")
        release_store_recovery_reserve(store)
        evaluation = adapter.finalize_evaluation()
        store.update(evaluation_cleanup_required=False)
        cleanup_messages.append(
            str(evaluation.get("message") or "evaluation evidence finalized")
        )
        rejection_result = (
            "REJECTED_NO_CHANGE"
            if plan.get("simulated") is True and context.get("candidate_diff") == "no-op"
            else "REJECTED_NO_IMPROVEMENT"
        )
        store.event("rejected", {"reason": "no_improvement"})
        decision = decision_payload(
            rejection_result,
            plan,
            store,
            analysis=analysis,
            reason="candidate envelope did not strictly clear baseline variability",
            cleanup=cleanup_messages,
        )
        store.finish(decision)
        return decision
    except SimulatedPowerLoss:
        raise
    except Exception as exc:
        decision_path = getattr(store, "decision_path", None)
        if isinstance(decision_path, Path) and decision_path.is_file():
            published = read_json_object(decision_path, "published improvement decision")
            if published.get("kind") == "improve_decision" and published.get("result") in TERMINAL_STATES:
                changes: dict[str, Any] = {
                    "status": published["result"],
                    "decision_sha256": sha256_file(decision_path),
                }
                if published.get("result") == "ACCEPTED":
                    changes.update(
                        restore_required=False,
                        evaluation_cleanup_required=False,
                    )
                store.update(**changes)
                return published
        primary = str(exc)
        try:
            release_store_recovery_reserve(store)
        except Exception as reserve_exc:
            primary = f"{primary}; recovery reserve release failed: {reserve_exc}"
        finalize_pending_flash_evidence(store)
        restore_error = ""
        evaluation_error = ""
        restore_required = bool(store.state.get("restore_required"))
        if (
            restore_required
            or candidate_may_be_installed
            or bool(store.state.get("candidate_may_be_installed"))
        ):
            try:
                store.update(status="RESTORE_INTENT")
                store.event("restore_intent", {"reason": "failure"})
                adapter.flash("baseline", recovery=True)
                store.update(
                    status="BASE_RESTORED",
                    current_firmware="baseline",
                    candidate_may_be_installed=False,
                    restore_required=False,
                )
                store.event("restore_complete")
                cleanup_messages.append("baseline firmware restored")
            except Exception as cleanup_exc:  # preserve the primary diagnosis
                restore_error = str(cleanup_exc)
                store.update(status="RESTORE_FAILED")
                store.event("restore_failed", {"error": restore_error})
        if prepared:
            try:
                evaluation = adapter.finalize_evaluation()
                store.update(evaluation_cleanup_required=False)
                cleanup_messages.append(
                    str(evaluation.get("message") or "evaluation evidence finalized")
                )
            except Exception as cleanup_exc:
                evaluation_error = str(cleanup_exc)
        # An analysis is only durably recorded once the experiment reached
        # ANALYZED, i.e. every batch, camera grade, and cross-arm regression
        # gate already passed and the envelope rule already decided the run.
        recorded_analysis = store.state.get("analysis")
        if not isinstance(recorded_analysis, dict) or not recorded_analysis:
            recorded_analysis = None
        stored_context = store.state.get("context")
        if not isinstance(stored_context, dict):
            stored_context = {}
        # A transient internal cleanup step must not overwrite an experiment that
        # is already decided. When the measured outcome exists and both baseline
        # restoration and evaluation-branch cleanup ultimately succeeded, publish
        # the experimental result rather than the cleanup diagnosis.
        decided_rejection = (
            not restore_error
            and not evaluation_error
            and recorded_analysis is not None
            and recorded_analysis.get("accepted") is False
        )
        if restore_error:
            result = "RESTORE_FAILED"
        elif evaluation_error:
            result = "CLEANUP_FAILED"
        elif decided_rejection:
            result = (
                "REJECTED_NO_CHANGE"
                if plan.get("simulated") is True
                and stored_context.get("candidate_diff") == "no-op"
                else "REJECTED_NO_IMPROVEMENT"
            )
        elif isinstance(exc, NoChangeCandidate):
            result = "REJECTED_NO_CHANGE"
        elif isinstance(exc, ResourceFailure):
            result = "REJECTED_RESOURCE_BUDGET"
        elif isinstance(exc, GateFailure):
            result = "REJECTED_GATE_FAILURE"
        elif candidate_may_be_installed:
            result = "ABORTED_BASE_RESTORED"
        else:
            result = "ABORTED_NO_RESTORE"
        if decided_rejection:
            details = [
                "candidate envelope did not strictly clear baseline variability",
                f"internal cleanup step recovered after: {primary}",
            ]
        else:
            details = [primary]
        if restore_error:
            details.append(f"baseline restore failed: {restore_error}")
        if evaluation_error:
            details.append(f"evaluation evidence finalization failed: {evaluation_error}")
        finalize_pending_flash_evidence(store)
        terminal_event = (
            "rejected"
            if result in {"REJECTED_NO_CHANGE", "REJECTED_NO_IMPROVEMENT"}
            else "terminal_failure"
        )
        store.event(terminal_event, {"result": result, "reason": details[0]})
        decision = decision_payload(
            result,
            plan,
            store,
            analysis=recorded_analysis,
            reason="; ".join(details),
            cleanup=cleanup_messages,
            require_valid_evidence=False,
        )
        if result in UNRESOLVED_STATES:
            store.update(
                status=result,
                restore_required=bool(restore_error),
                evaluation_cleanup_required=bool(evaluation_error),
            )
            if isinstance(store, FileEvidenceStore):
                failures = store.session_dir / "cleanup_failures"
                ensure_owned_directory(
                    store.session_dir,
                    failures,
                    label="cleanup-failure evidence directory",
                )
                failure_path = failures / f"attempt-{len(list(failures.glob('attempt-*.json'))) + 1:03d}.json"
                assert_owned_path_chain(
                    store.session_dir,
                    failure_path,
                    label="cleanup-failure evidence",
                )
                write_exclusive_json(failure_path, decision)
        else:
            store.finish(decision)
        return decision


def recover_experiment(plan: Mapping[str, Any], adapter: Adapter, store: EvidenceStore) -> dict[str, Any]:
    """Fail closed after an interrupted live session; never continue measurements."""
    decision_path = getattr(store, "decision_path", None)
    if isinstance(decision_path, Path) and decision_path.is_file():
        decision = read_json_object(decision_path, "improvement decision")
        if decision.get("kind") != "improve_decision" or decision.get("result") not in TERMINAL_STATES:
            raise GateFailure("existing improvement decision is invalid")
        changes: dict[str, Any] = {
            "status": decision["result"],
            "decision_sha256": sha256_file(decision_path),
        }
        if decision.get("result") == "ACCEPTED":
            changes.update(restore_required=False, evaluation_cleanup_required=False)
        store.update(**changes)
        return decision
    if store.state.get("status") in TERMINAL_STATES and store.state.get("decision_sha256"):
        return read_json_object(getattr(store, "decision_path"), "improvement decision")
    release_store_recovery_reserve(store)
    finalize_pending_flash_evidence(store)
    cleanup: list[str] = []
    restored = False
    restore_error = ""
    if (
        store.state.get("restore_required") is True
        or store.state.get("candidate_may_be_installed") is True
        or str(store.state.get("status") or "").endswith("FLASH_INTENT")
    ):
        try:
            store.event("recovery_restore_intent")
            adapter.flash("baseline", recovery=True)
            restored = True
            store.update(
                current_firmware="baseline",
                candidate_may_be_installed=False,
                restore_required=False,
            )
            store.event("recovery_restore_complete")
            cleanup.append("baseline firmware restored during recovery")
        except Exception as exc:
            restore_error = str(exc)
            store.event("recovery_restore_failed", {"error": restore_error})
    evaluation_error = ""
    if restored or store.state.get("evaluation_cleanup_required") is not False:
        try:
            evaluation = adapter.finalize_evaluation()
            store.update(evaluation_cleanup_required=False)
            cleanup.append(str(evaluation.get("message") or "evaluation evidence finalized"))
        except Exception as exc:
            evaluation_error = str(exc)
            cleanup.append(f"evaluation evidence finalization failed: {exc}")
    result = (
        "RESTORE_FAILED"
        if restore_error
        else "CLEANUP_FAILED"
        if evaluation_error
        else "ABORTED_BASE_RESTORED"
        if restored
        else "ABORTED_NO_RESTORE"
    )
    reason = (
        f"interrupted session could not restore baseline: {restore_error}"
        if restore_error
        else f"interrupted session could not finalize its evaluation evidence: {evaluation_error}"
        if evaluation_error
        else "interrupted session was recovered conservatively; measurements were not resumed"
    )
    finalize_pending_flash_evidence(store)
    store.event("recovery_terminal", {"result": result, "reason": reason})
    # Recovery never resumes measurements, but if the interrupted session had
    # already reached ANALYZED its stored envelope result is still the honest
    # description of what was measured. Carry it into the tolerant decision.
    recorded_analysis = store.state.get("analysis")
    if not isinstance(recorded_analysis, dict) or not recorded_analysis:
        recorded_analysis = None
    decision = decision_payload(
        result,
        plan,
        store,
        analysis=recorded_analysis,
        reason=reason,
        cleanup=cleanup,
        require_valid_evidence=False,
    )
    if result in UNRESOLVED_STATES:
        store.update(
            status=result,
            restore_required=bool(restore_error),
            evaluation_cleanup_required=bool(evaluation_error),
        )
        if isinstance(store, FileEvidenceStore):
            failures = store.session_dir / "cleanup_failures"
            ensure_owned_directory(
                store.session_dir,
                failures,
                label="cleanup-failure evidence directory",
            )
            path = failures / f"attempt-{len(list(failures.glob('attempt-*.json'))) + 1:03d}.json"
            assert_owned_path_chain(
                store.session_dir,
                path,
                label="cleanup-failure evidence",
            )
            write_exclusive_json(path, decision)
    else:
        store.finish(decision)
    return decision


class FakeAdapter:
    def __init__(
        self,
        baseline_values: Sequence[float],
        candidate_values: Sequence[float],
        *,
        crash_on_candidate_flash: bool = False,
    ):
        self.values = {
            "baseline": list(baseline_values),
            "candidate": list(candidate_values),
        }
        self.context = {
            "builds": "simulated_pass",
            "resources": "simulated_within_budget",
            "candidate_diff": "no-op" if baseline_values == candidate_values else "simulated",
        }
        self.operations: list[str] = []
        self.crash_on_candidate_flash = crash_on_candidate_flash
        self.crashed = False

    def prepare(self) -> dict[str, Any]:
        self.operations.append("prepare")
        if self.context.get("candidate_diff") == "no-op":
            raise NoChangeCandidate(
                "candidate source change produced the same firmware bytes as baseline"
            )
        return dict(self.context)

    def check_stop(self) -> None:
        return None

    def flash(self, arm: str, *, recovery: bool = False) -> None:
        self.operations.append(f"flash:{arm}:{'recovery' if recovery else 'normal'}")
        if arm == "candidate" and self.crash_on_candidate_flash and not self.crashed:
            self.crashed = True
            raise SimulatedPowerLoss("simulated power loss after candidate flash intent")

    def collect(self, arm: str, arm_index: int, sequence: int) -> dict[str, Any]:
        self.operations.append(f"collect:{arm}:{arm_index}")
        value = self.values[arm][arm_index - 1]
        digest = sha256_bytes(f"{arm}:{arm_index}:{sequence}:{value}".encode("ascii"))
        return {
            "arm": arm,
            "arm_index": arm_index,
            "sequence": sequence,
            "result": "PASS",
            "target_value": value,
            "bench_result": f"simulated/{arm}/{arm_index}/bench_result.json",
            "bench_result_sha256": digest,
            "metric_run_id": f"simulated-{arm}-{arm_index}",
            "qualification": f"simulated/{arm}/{arm_index}/qualification.json",
            "qualification_sha256": sha256_bytes((digest + ":qualification").encode("ascii")),
            "simulated": True,
        }

    def finalize_evaluation(self) -> dict[str, Any]:
        self.operations.append("finalize_evaluation")
        return {"message": "simulated evaluation evidence finalized"}

    def validate_regressions(self, runs: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
        self.operations.append("validate_cross_arm_regressions")
        return [
            {
                "suite": suite,
                "candidate_arm_index": index,
                "result": "PASS",
                "baseline_count": len([run for run in runs if run.get("arm") == "baseline"]),
                "simulated": True,
            }
            for index in range(1, len([run for run in runs if run.get("arm") == "candidate"]) + 1)
            for suite in ("core", "display", "replay")
        ]


def dry_plan() -> dict[str, Any]:
    runs = MIN_RUNS
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "improve_plan",
        "simulated": True,
        "runs_per_arm": runs,
        "schedule": counterbalanced_schedule(runs),
        "target": {
            "suite": "replay",
            "metric": "disp_pipe_p95_us",
            "unit": "us",
            "aggregation": "last",
            "direction": "lower_better",
            "score_level": "hard",
            "catalog_sha256": "0" * 64,
        },
    }


def build_dry_run_report() -> dict[str, Any]:
    plan = dry_plan()
    from improve_git_dryrun import run_disposable_git_noop_scenario

    no_op = run_disposable_git_noop_scenario(
        plan=plan,
        execute_experiment=execute_experiment,
        validate_candidate_paths=validate_candidate_paths,
        no_change_exception=NoChangeCandidate,
        evidence_store_factory=lambda path: FileEvidenceStore(
            path, clock=lambda: "2001-01-01T00:03:00Z"
        ),
    )
    no_op_decision = no_op["decision"]

    accept_store = MemoryEvidenceStore()
    accept_adapter = FakeAdapter([100, 102, 101, 99, 100], [90, 91, 89, 92, 90])
    accept_decision = execute_experiment(plan, accept_adapter, accept_store)

    crash_store = MemoryEvidenceStore()
    crash_adapter = FakeAdapter(
        [100, 101, 99, 100, 100],
        [90, 91, 89, 92, 90],
        crash_on_candidate_flash=True,
    )
    crashed = False
    try:
        execute_experiment(plan, crash_adapter, crash_store)
    except SimulatedPowerLoss:
        crashed = True
    crash_adapter.crash_on_candidate_flash = False
    recovery = recover_experiment(plan, crash_adapter, crash_store)

    passed = (
        no_op_decision["result"] == "REJECTED_NO_CHANGE"
        and not any(
            operation.startswith(("flash:", "collect:"))
            for operation in no_op["operations"]
        )
        and accept_decision["result"] == "ACCEPTED"
        and crashed
        and recovery["result"] == "ABORTED_BASE_RESTORED"
        and "flash:baseline:recovery" in crash_adapter.operations
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "improve_dry_run_report",
        "result": "PASS" if passed else "FAIL",
        "simulated": True,
        "hardware_actions": 0,
        "external_product_actions": 0,
        "git_actions": no_op["git"]["real_git_actions"],
        "plan": plan,
        "scenarios": {
            "no_op_rejection": no_op,
            "clear_improvement_acceptance": {
                "decision": accept_decision,
                "operations": accept_adapter.operations,
            },
            "candidate_flash_interruption_recovery": {
                "decision": recovery,
                "operations": crash_adapter.operations,
            },
        },
    }


def session_id(base_sha: str) -> str:
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d_%H%M%S")
    entropy = os.urandom(3).hex()
    return f"{stamp}_{base_sha[:7]}_{entropy}"


def make_live_plan(args: argparse.Namespace, *, dry_run_report_sha256: str) -> dict[str, Any]:
    validate_git_environment()
    validate_runs(args.runs)
    validate_branch_name(args.candidate_branch)
    validate_serial_port(args.port, require_exists=True)
    catalog_path = ROOT / "tools" / "hardware_metric_catalog.json"
    policy, catalog_sha256 = resolve_target_policy(catalog_path, args.target_suite, args.target_metric)
    root_snapshot = source_snapshot(ROOT)
    if root_snapshot["status"]:
        raise InvalidInput("invoking worktree must be clean before a live experiment")
    base_sha = run_capture(["git", "rev-parse", f"{args.base_ref}^{{commit}}"], cwd=ROOT)
    if base_sha != root_snapshot.get("head"):
        raise InvalidInput("Phase-B base-ref must resolve to the clean invoking HEAD")
    verify_worktree_matches_commit(ROOT, base_sha)
    candidate_ref = f"refs/heads/{args.candidate_branch}"
    candidate_sha = run_capture(["git", "rev-parse", f"{candidate_ref}^{{commit}}"], cwd=ROOT)
    if run_capture(["git", "rev-list", "--count", f"{base_sha}..{candidate_sha}"], cwd=ROOT) != "1":
        raise InvalidInput("candidate branch must contain exactly one commit above the pinned base")
    if run_capture(["git", "rev-parse", f"{candidate_sha}^"], cwd=ROOT) != base_sha:
        raise InvalidInput("candidate commit must be a direct, non-merge child of the pinned base")
    parents = run_capture(
        ["git", "rev-list", "--parents", "-n", "1", candidate_sha], cwd=ROOT
    ).split()
    if parents != [candidate_sha, base_sha]:
        raise InvalidInput("candidate commit must have exactly one parent: the pinned base")
    changed_raw = run_capture(
        ["git", "diff", "--no-renames", "--name-only", "-z", base_sha, candidate_sha, "--"],
        cwd=ROOT,
    )
    changed_paths = [item for item in changed_raw.split("\0") if item]
    validate_candidate_paths(changed_paths)
    for path in changed_paths:
        tree_entry = run_capture(["git", "ls-tree", candidate_sha, "--", path], cwd=ROOT)
        if tree_entry:
            mode = tree_entry.split(None, 1)[0]
            if mode == "120000":
                raise InvalidInput(f"candidate path is a symlink: {path}")
            if mode == "160000":
                raise InvalidInput(f"candidate path is a submodule: {path}")
    identifier = session_id(base_sha)
    artifact_root = Path(args.artifact_root).expanduser().resolve()
    session_dir = artifact_root / "sessions" / identifier
    branch = f"improve/{identifier}/candidate"
    worktree_root = session_dir / "worktrees"
    required_bytes = int(
        (
            ESTIMATED_GIB_PER_FULL_RUN * 2 * args.runs
            + WORKTREE_AND_BUILD_RESERVE_GIB
            + DISK_RESERVE_GIB
        )
        * GIB
    )
    artifact_root.mkdir(parents=True, exist_ok=True)
    available = shutil.disk_usage(artifact_root).free
    if available < required_bytes:
        raise InvalidInput(
            f"insufficient artifact space: need at least {required_bytes / GIB:.1f} GiB free, "
            f"found {available / GIB:.1f} GiB"
        )
    controller_components = {
        "scripts/improve.py": sha256_file(Path(__file__).resolve()),
        "scripts/improve_git_dryrun.py": sha256_file(ROOT / "scripts" / "improve_git_dryrun.py"),
    }
    plan = {
        "schema_version": SCHEMA_VERSION,
        "kind": "improve_plan",
        "simulated": False,
        "session_id": identifier,
        "session_dir": str(session_dir),
        "created_at_utc": utc_now(),
        "source_root": str(ROOT),
        "source_snapshot": root_snapshot,
        "base_ref": args.base_ref,
        "base_sha": base_sha,
        "candidate_branch": args.candidate_branch,
        "candidate_sha": candidate_sha,
        "evaluation_branch": branch,
        "base_worktree": str(worktree_root / "base"),
        "candidate_worktree": str(worktree_root / "candidate"),
        "changed_paths": changed_paths,
        "target": policy_record(policy, args.target_suite, catalog_sha256),
        "runs_per_arm": args.runs,
        "schedule": counterbalanced_schedule(args.runs),
        "board_id": DEFAULT_BOARD_ID,
        "env": DEFAULT_ENV,
        "port": args.port,
        "bench_contract": {
            "all_suites": True,
            "camera": True,
            "duration_seconds": RUN_DURATION_SECONDS,
            "replay_duration_seconds": REPLAY_DURATION_SECONDS,
            "profile": PROFILE,
            "segment": SEGMENT,
            "blink_profile": BLINK_PROFILE,
            "baseline_comparison": False,
            "upload_during_bench": False,
        },
        "post_flash_settle_seconds": DEFAULT_SETTLE_SECONDS,
        "disk_preflight": {
            "required_free_bytes": required_bytes,
            "available_free_bytes": available,
        },
        "controller_components": controller_components,
        "controller_sha256": sha256_bytes(canonical_bytes(controller_components)),
        "dry_run_report_sha256": dry_run_report_sha256,
    }
    validate_live_plan(plan, session_dir, require_port=True)
    return plan


class LiveAdapter:
    def __init__(
        self,
        plan: Mapping[str, Any],
        store: FileEvidenceStore,
        runner: CommandRunner,
        stop: StopController,
    ):
        self.plan = dict(plan)
        self.store = store
        self.runner = runner
        self.stop = stop
        self.session_dir = Path(str(plan["session_dir"]))
        self.base_worktree = Path(str(plan["base_worktree"]))
        self.candidate_worktree = Path(str(plan["candidate_worktree"]))
        for label, path in (
            ("base worktree", self.base_worktree),
            ("candidate worktree", self.candidate_worktree),
        ):
            assert_owned_path_chain(self.session_dir, path, label=label)
        self.context: dict[str, Any] = dict(store.state.get("context") or {})
        self.operations: list[str] = []
        self.campaign_lease = CampaignRadioLease()

    def _run(
        self,
        name: str,
        argv: Sequence[str],
        cwd: Path,
        *,
        recovery: bool = False,
        allowed_statuses: frozenset[int] = frozenset({0}),
    ) -> int:
        self.operations.append(name)
        print(f"[improve] {name}", flush=True)
        log_path = self.session_dir / "logs" / f"{name}.log"
        ensure_owned_directory(
            self.session_dir,
            log_path.parent,
            label="command-log directory",
        )
        assert_owned_path_chain(self.session_dir, log_path, label=f"{name} command log")
        pass_fds: tuple[int, ...] = ()
        extra_env: dict[str, str] | None = None
        if self.campaign_lease.fd is not None:
            pass_fds, extra_env = self.campaign_lease.child_contract()
        try:
            return self.runner.run(
                argv,
                cwd=cwd,
                log_path=log_path,
                recovery=recovery,
                allowed_statuses=allowed_statuses,
                pass_fds=pass_fds,
                extra_env=extra_env,
            )
        finally:
            if log_path.is_file() and not log_path.is_symlink():
                citation = {
                    "name": name,
                    "path": str(log_path),
                    "size_bytes": log_path.stat().st_size,
                    "sha256": sha256_file(log_path),
                }
                citations = list(self.store.state.get("command_logs") or [])
                if any(item.get("path") == str(log_path) for item in citations if isinstance(item, dict)):
                    raise GateFailure(f"command log was already cited: {log_path}")
                citations.append(citation)
                self.store.update(command_logs=citations)

    def _next_command_attempt_name(self, prefix: str) -> str:
        logs = self.session_dir / "logs"
        attempt = 1
        while (logs / f"{prefix}-attempt-{attempt:03d}.log").exists():
            attempt += 1
        return f"{prefix}-attempt-{attempt:03d}"

    def close(self) -> None:
        self.campaign_lease.close()

    def check_stop(self) -> None:
        self.stop.check()

    def _seed_assets(self, worktree: Path) -> dict[str, Any]:
        results: dict[str, Any] = {}
        for relative in (Path("data/audio"), Path("data/branding")):
            source = ROOT / relative
            destination = worktree / relative
            if not source.is_dir():
                raise InvalidInput(f"required deployed asset tree is missing: {source}")
            digest = tree_digest(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(source, destination)
            copied = tree_digest(destination)
            if copied != digest:
                raise GateFailure(f"deployed asset copy changed bytes: {relative}")
            results[relative.as_posix()] = digest
        return results

    def _build(self, arm: str, worktree: Path) -> dict[str, Any]:
        self.stop.check()
        self._assert_worktree_owned(arm)
        build_root = self.session_dir / "builds" / arm
        ensure_owned_directory(
            self.session_dir,
            build_root,
            label=f"{arm} build evidence directory",
        )
        build_log = build_root / "build.log"
        self._run(f"{arm}-privacy", ["python3", "scripts/check_local_privacy_setup.py"], worktree)
        self._run(f"{arm}-ci", ["./scripts/ci-test.sh"], worktree)
        self._run(f"{arm}-build", ["./build.sh", "--skip-web"], worktree)
        self._run(
            f"{arm}-buildfs",
            ["pio", "run", "-e", DEFAULT_ENV, "-t", "buildfs"],
            worktree,
        )
        self._run(
            f"{arm}-flash-package",
            [
                "python3",
                "scripts/report_flash_package_size.py",
                "--build-dir",
                f".pio/build/{DEFAULT_ENV}",
                "--partition-table",
                "partitions_v1.csv",
                "--max-firmware-bytes",
                str(MAX_FIRMWARE_BYTES),
                "--expect-littlefs-bytes",
                str(EXPECTED_LITTLEFS_BYTES),
            ],
            worktree,
        )
        memory_dir = build_root / "memory"
        ensure_owned_directory(
            self.session_dir,
            memory_dir,
            label=f"{arm} memory evidence directory",
        )
        self._run(
            f"{arm}-memory",
            [
                "python3",
                "scripts/check_memory_headroom.py",
                "--env",
                DEFAULT_ENV,
                "--no-build",
                "--report-dir",
                str(memory_dir),
                "--warn-diram-zero",
            ],
            worktree,
        )
        # Preserve the final build log used by build.sh and the owned binaries.
        command_log = self.session_dir / "logs" / f"{arm}-build.log"
        shutil.copy2(command_log, build_log)
        output_dir = build_root / "images"
        ensure_owned_directory(
            self.session_dir,
            output_dir,
            label=f"{arm} preserved-image directory",
        )
        files: dict[str, Any] = {}
        for name in ("bootloader.bin", "partitions.bin", "firmware.bin"):
            source = worktree / ".pio" / "build" / DEFAULT_ENV / name
            if not source.is_file() or source.stat().st_size < 1:
                raise GateFailure(f"{arm} build did not produce {name}")
            destination = output_dir / name
            shutil.copy2(source, destination)
            files[name] = {
                "path": str(destination),
                "size_bytes": destination.stat().st_size,
                "sha256": sha256_file(destination),
            }
        memory_path = memory_dir / f"{DEFAULT_ENV}.json"
        memory = read_json_object(memory_path, f"{arm} memory report")
        validate_memory_report(memory, arm)
        return {
            "build_log": str(build_log),
            "build_log_sha256": sha256_file(build_log),
            "memory_report": str(memory_path),
            "memory_report_sha256": sha256_file(memory_path),
            "memory": memory,
            "flash_package_log": str(self.session_dir / "logs" / f"{arm}-flash-package.log"),
            "flash_package_log_sha256": sha256_file(
                self.session_dir / "logs" / f"{arm}-flash-package.log"
            ),
            "images": files,
        }

    def prepare(self) -> dict[str, Any]:
        source_before = source_snapshot(ROOT)
        if source_before != self.plan["source_snapshot"]:
            raise InvalidInput("invoking worktree changed after the immutable plan was created")
        self._run("source-privacy", ["python3", "scripts/check_local_privacy_setup.py"], ROOT)
        patch = run_capture(
            [
                "git",
                "diff",
                "--binary",
                str(self.plan["base_sha"]),
                str(self.plan["candidate_sha"]),
                "--",
            ],
            cwd=ROOT,
        ).encode("utf-8")
        if not patch:
            raise InvalidInput("candidate patch is unexpectedly empty")
        patch_path = self.session_dir / "candidate.patch"
        write_exclusive(patch_path, patch)
        self.context = {
            "patch": {
                "path": str(patch_path),
                "size_bytes": patch_path.stat().st_size,
                "sha256": sha256_file(patch_path),
            }
        }
        self.store.update(context=self.context)
        assert_owned_path_chain(
            self.session_dir, self.base_worktree, label="base worktree creation path"
        )
        assert_owned_path_chain(
            self.session_dir, self.candidate_worktree, label="candidate worktree creation path"
        )
        ensure_owned_directory(
            self.session_dir,
            self.base_worktree.parent,
            label="controller worktree directory",
        )
        assert_owned_path_chain(
            self.session_dir, self.base_worktree, label="base worktree creation path"
        )
        assert_repository_hook_contract(ROOT)
        self._run(
            "worktree-base",
            [
                "git",
                "worktree",
                "add",
                "--detach",
                str(self.base_worktree),
                str(self.plan["base_sha"]),
            ],
            ROOT,
        )
        self.store.update(evaluation_cleanup_required=True)
        assert_owned_path_chain(
            self.session_dir, self.candidate_worktree, label="candidate worktree creation path"
        )
        assert_repository_hook_contract(ROOT)
        self._run(
            "worktree-candidate",
            [
                "git",
                "worktree",
                "add",
                "-b",
                str(self.plan["evaluation_branch"]),
                str(self.candidate_worktree),
                str(self.plan["candidate_sha"]),
            ],
            ROOT,
        )
        self._assert_worktree_owned("baseline")
        self._assert_worktree_owned("candidate")
        assets = {
            "baseline": self._seed_assets(self.base_worktree),
            "candidate": self._seed_assets(self.candidate_worktree),
        }
        if assets["baseline"] != assets["candidate"]:
            raise GateFailure("base and candidate deployed asset bytes differ")
        self.context["assets"] = assets["baseline"]
        self.store.update(context=self.context)
        identities = {
            "baseline": identity_for_worktree(self.base_worktree),
            "candidate": identity_for_worktree(self.candidate_worktree),
        }
        validate_identity_pair(identities["baseline"], identities["candidate"])
        self.context["identities"] = identities
        self.context["builds"] = {}
        self.store.update(context=self.context)
        builds: dict[str, Any] = {}
        for arm, worktree in (
            ("baseline", self.base_worktree),
            ("candidate", self.candidate_worktree),
        ):
            self.context["builds"][arm] = {
                "status": "STARTED",
                "log_root": str(self.session_dir / "logs"),
            }
            self.store.update(context=self.context)
            builds[arm] = self._build(arm, worktree)
            self.context["builds"][arm] = builds[arm]
            self.store.update(context=self.context)
        resource_deltas = compare_memory_reports(
            builds["baseline"]["memory"], builds["candidate"]["memory"]
        )
        self.context["resource_deltas"] = resource_deltas
        self.store.update(context=self.context)
        for name in ("bootloader.bin", "partitions.bin"):
            if builds["baseline"]["images"][name]["sha256"] != builds["candidate"]["images"][name][
                "sha256"
            ]:
                raise ResourceFailure(f"candidate unexpectedly changed {name}")
        if builds["baseline"]["images"]["firmware.bin"]["sha256"] == builds["candidate"]["images"][
            "firmware.bin"
        ]["sha256"]:
            raise NoChangeCandidate(
                "candidate source change produced the same firmware bytes as baseline"
            )
        return dict(self.context)

    def _worktree(self, arm: str) -> Path:
        if arm == "baseline":
            return self.base_worktree
        if arm == "candidate":
            return self.candidate_worktree
        raise InvalidInput(f"unknown arm: {arm}")

    def _assert_context_anchor(self) -> None:
        if not self.context:
            raise GateFailure("prepared build context is missing")
        expected = sha256_bytes(canonical_bytes(self.context))
        anchored = False
        if os.path.lexists(self.store.events_path):
            journal, _identity = read_owned_regular_bytes(
                self.store.events_path, label="improvement event journal"
            )
            for line in journal.decode("utf-8").splitlines():
                entry = json.loads(line)
                if entry.get("event") == "prepare_complete":
                    details = entry.get("details")
                    anchored = isinstance(details, dict) and details.get("context_sha256") == expected
        if not anchored:
            raise GateFailure("prepared build context is not anchored in the event journal")

    def _assert_worktree_owned(self, arm: str, *, allow_reverted: bool = False) -> str:
        worktree = self._worktree(arm)
        assert_owned_path_chain(self.session_dir, worktree, label=f"{arm} worktree")
        if not worktree.is_dir() or worktree.is_symlink():
            raise GateFailure(f"{arm} worktree ownership is unavailable")
        top = Path(
            run_capture(["git", "rev-parse", "--show-toplevel"], cwd=worktree)
        ).resolve()
        common = Path(
            run_capture(
                ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                cwd=worktree,
            )
        ).resolve()
        if top != worktree.resolve() or common != (ROOT / ".git").resolve():
            raise GateFailure(f"{arm} worktree no longer belongs to this repository/session")
        if run_capture(["git", "status", "--porcelain=v1", "-uall"], cwd=worktree):
            raise GateFailure(f"{arm} worktree is dirty")
        head = run_capture(["git", "rev-parse", "HEAD"], cwd=worktree)
        status, symbolic_ref, _ = run_capture_optional(
            ["git", "symbolic-ref", "-q", "HEAD"], cwd=worktree
        )
        if arm == "baseline":
            if status == 0 or head != self.plan["base_sha"]:
                raise GateFailure("baseline worktree is not detached at the pinned base")
        else:
            expected_ref = f"refs/heads/{self.plan['evaluation_branch']}"
            if status != 0 or symbolic_ref != expected_ref:
                raise GateFailure("candidate worktree is not on the controller evaluation branch")
            source_candidate = run_capture(
                ["git", "rev-parse", f"refs/heads/{self.plan['candidate_branch']}"], cwd=ROOT
            )
            if source_candidate != self.plan["candidate_sha"]:
                raise GateFailure("submitted candidate branch moved during the experiment")
            evaluation_head = run_capture(["git", "rev-parse", expected_ref], cwd=ROOT)
            if evaluation_head != head:
                raise GateFailure("evaluation branch ref and worktree HEAD disagree")
            if not allow_reverted and head != self.plan["candidate_sha"]:
                raise GateFailure("candidate evaluation worktree moved before flashing")
            if allow_reverted and head != self.plan["candidate_sha"]:
                tree_diff = run_capture(
                    ["git", "diff", "--name-only", self.plan["base_sha"], head, "--"],
                    cwd=worktree,
                )
                if tree_diff:
                    raise GateFailure("evaluation branch is neither candidate nor reverted base tree")
        verify_worktree_matches_commit(worktree, head)
        return head

    def _verified_images(self, arm: str) -> dict[str, Any]:
        self._assert_context_anchor()
        builds = self.context.get("builds")
        if not isinstance(builds, dict) or not isinstance(builds.get(arm), dict):
            raise GateFailure(f"{arm} build evidence is missing")
        records = builds[arm].get("images")
        if not isinstance(records, dict):
            raise GateFailure(f"{arm} image evidence is missing")
        verified: dict[str, Any] = {}
        for name in ("bootloader.bin", "partitions.bin", "firmware.bin"):
            record = records.get(name)
            if not isinstance(record, dict) or not valid_digest(record.get("sha256")):
                raise GateFailure(f"{arm} {name} evidence is invalid")
            preserved = Path(os.path.abspath(str(record.get("path") or "")))
            expected_preserved = self.session_dir / "builds" / arm / "images" / name
            actual = self._worktree(arm) / ".pio" / "build" / DEFAULT_ENV / name
            assert_owned_path_chain(
                self.session_dir, preserved, label=f"{arm} preserved {name}"
            )
            assert_owned_path_chain(
                self._worktree(arm), actual, label=f"{arm} build {name}"
            )
            if preserved != expected_preserved:
                raise GateFailure(f"{arm} {name} is missing or outside owned storage")
            for path in (preserved, actual):
                try:
                    metadata = path.lstat()
                except OSError as exc:
                    raise GateFailure(f"{arm} {name} is missing before upload") from exc
                if (
                    not stat.S_ISREG(metadata.st_mode)
                    or stat.S_ISLNK(metadata.st_mode)
                    or metadata.st_nlink != 1
                    or metadata.st_size != record.get("size_bytes")
                    or sha256_file(path) != record["sha256"]
                ):
                    raise GateFailure(f"{arm} {name} bytes changed before upload")
            verified[name] = {
                "sha256": record["sha256"],
                "size_bytes": record["size_bytes"],
                "preserved_path": str(preserved),
            }
        return verified

    def flash(self, arm: str, *, recovery: bool = False) -> None:
        self.campaign_lease.acquire()
        worktree = self._worktree(arm)
        self._assert_worktree_owned(arm)
        images = self._verified_images(arm)
        command = build_flash_command(str(self.plan["port"]))
        assert_firmware_only_command(command)
        attempts = list(self.store.state.get("flash_attempts") or [])
        attempt_number = len(attempts) + 1
        name = (
            f"flash-{arm}-{'recovery' if recovery else 'campaign'}"
            f"-attempt-{attempt_number:03d}"
        )
        log_path = self.session_dir / "logs" / f"{name}.log"
        attempt = {
            "attempt": attempt_number,
            "arm": arm,
            "recovery": recovery,
            "status": "STARTED",
            "command": command,
            "images": images,
            "log": str(log_path),
        }
        attempts.append(attempt)
        self.store.update(flash_attempts=attempts)
        self._run(
            name,
            command,
            worktree,
            recovery=recovery,
        )
        self._verified_images(arm)
        attempt["status"] = "COMPLETE"
        attempt["log_sha256"] = sha256_file(log_path)
        self.store.update(flash_attempts=attempts)
        if not recovery:
            deadline = time.monotonic() + int(self.plan["post_flash_settle_seconds"])
            while time.monotonic() < deadline:
                self.stop.check()
                time.sleep(min(0.25, deadline - time.monotonic()))

    def _new_run_dirs(self, before: set[Path], artifact_root: Path) -> list[Path]:
        after = self._current_run_dirs(artifact_root)
        return sorted(after - before)

    def _current_run_dirs(self, artifact_root: Path) -> set[Path]:
        runs_root = artifact_root / DEFAULT_BOARD_ID / "runs"
        assert_owned_path_chain(self.session_dir, runs_root, label="bench runs root")
        if not runs_root.is_dir():
            return set()
        output: set[Path] = set()
        for path in runs_root.iterdir():
            if path.is_symlink():
                raise GateFailure(f"bench runs root contains a symlink: {path}")
            if path.is_dir():
                lexical = Path(os.path.abspath(path))
                assert_owned_path_chain(self.session_dir, lexical, label="bench run directory")
                output.add(lexical)
        return output

    def _check_capture_space(self) -> dict[str, Any]:
        completed = list(self.store.state.get("runs") or [])
        total_runs = 2 * int(self.plan["runs_per_arm"])
        remaining = total_runs - len(completed)
        if remaining < 1:
            raise GateFailure("capture-space check ran after the planned campaign completed")
        observed_sizes: list[int] = []
        for evidence in completed:
            run_dir = evidence.get("run_dir") if isinstance(evidence, dict) else None
            if not isinstance(run_dir, str):
                continue
            path = Path(os.path.abspath(run_dir))
            assert_owned_path_chain(
                self.session_dir,
                path,
                label="completed bench run",
            )
            try:
                path.relative_to(self.session_dir / "bench")
            except ValueError as exc:
                raise GateFailure("completed run path escapes the Phase-B session") from exc
            observed_sizes.append(owned_tree_size(path))
        estimate_per_run = int(ESTIMATED_GIB_PER_FULL_RUN * GIB)
        if observed_sizes:
            estimate_per_run = max(estimate_per_run, int(max(observed_sizes) * 1.15))
        required = remaining * estimate_per_run + int(DISK_RESERVE_GIB * GIB)
        available = shutil.disk_usage(self.session_dir).free
        check = {
            "before_sequence": len(completed) + 1,
            "remaining_runs_including_next": remaining,
            "estimated_bytes_per_run": estimate_per_run,
            "required_free_bytes": required,
            "available_free_bytes": available,
            "observed_run_sizes": observed_sizes,
        }
        checks = list(self.store.state.get("disk_checks") or [])
        checks.append(check)
        self.store.update(disk_checks=checks)
        if available < required:
            raise GateFailure(
                f"insufficient space before capture: need {required / GIB:.1f} GiB free, "
                f"found {available / GIB:.1f} GiB"
            )
        return check

    def collect(self, arm: str, arm_index: int, sequence: int) -> dict[str, Any]:
        if self.campaign_lease.fd is None:
            raise GateFailure("campaign radio lease is not held before collection")
        self._check_capture_space()
        worktree = self._worktree(arm)
        artifact_root = self.session_dir / "bench" / arm
        ensure_owned_directory(
            self.session_dir,
            artifact_root,
            label=f"{arm} bench artifact root",
        )
        before = self._current_run_dirs(artifact_root)
        command = build_bench_command(str(self.plan["port"]), artifact_root)
        log_name = f"bench-{sequence:02d}-{arm}-{arm_index:02d}"
        bench_status: int | None = None
        invocation_error = ""
        interrupted = False
        try:
            bench_status = self._run(
                log_name,
                command,
                worktree,
                allowed_statuses=frozenset({0, 1, 2, 3}),
            )
        except ControllerInterrupted as exc:
            invocation_error = str(exc)
            interrupted = True
        except GateFailure as exc:
            invocation_error = str(exc)
        log_path = self.session_dir / "logs" / f"{log_name}.log"
        evidence: dict[str, Any] = {
            "arm": arm,
            "arm_index": arm_index,
            "sequence": sequence,
            "result": "COLLECTION_FAILED",
            "bench_exit_status": bench_status,
            "bench_log": str(log_path),
            "bench_log_sha256": sha256_file(log_path),
            "simulated": False,
        }
        if invocation_error:
            evidence["validation_error"] = invocation_error
            evidence["controller_interrupted"] = interrupted
        created = self._new_run_dirs(before, artifact_root)
        if len(created) != 1:
            creation_error = f"bench invocation created {len(created)} run directories, expected one"
            evidence["validation_error"] = (
                f"{invocation_error}; {creation_error}" if invocation_error else creation_error
            )
            evidence["created_run_dirs"] = [str(path) for path in created]
            return evidence
        run_dir = created[0]
        evidence["run_dir"] = str(run_dir)
        bench_result_path = run_dir / "bench_result.json"
        if not bench_result_path.is_file():
            evidence["validation_error"] = "bench result is missing"
            return evidence
        evidence["bench_result"] = str(bench_result_path)
        evidence["bench_result_sha256"] = sha256_file(bench_result_path)
        try:
            result = read_json_object(bench_result_path, "bench result")
        except GateFailure as exc:
            evidence["validation_error"] = str(exc)
            return evidence
        evidence["result"] = str(result.get("result") or "COLLECTION_FAILED")
        if invocation_error or bench_status != 0 or result.get("result") != "PASS":
            status_detail = f"bench exit={bench_status} canonical result={result.get('result')!r}"
            evidence["validation_error"] = (
                f"{invocation_error}; {status_detail}" if invocation_error else status_detail
            )
            return evidence
        identities = self.context["identities"][arm]
        try:
            validate_bench_result(
                result,
                arm=arm,
                arm_index=arm_index,
                plan=self.plan,
                identities=identities,
            )
        except GateFailure as exc:
            evidence["result"] = "EVIDENCE_INVALID"
            evidence["validation_error"] = str(exc)
            return evidence
        qualification_path = self.session_dir / "qualifications" / f"{sequence:02d}-{arm}-{arm_index:02d}.json"
        ensure_owned_directory(
            self.session_dir,
            qualification_path.parent,
            label="qualification evidence directory",
        )
        assert_owned_path_chain(
            self.session_dir, qualification_path, label="qualification evidence"
        )
        try:
            self._run(
                f"qualify-{sequence:02d}-{arm}-{arm_index:02d}",
                [
                    "python3",
                    "scripts/bench/bench_policy.py",
                    "record-full",
                    "--bench-result",
                    str(bench_result_path),
                    "--qualification",
                    str(qualification_path),
                    "--board-id",
                    DEFAULT_BOARD_ID,
                ],
                worktree,
            )
            qualification = read_json_object(qualification_path, "qualification record")
            try:
                validate_qualification_record(qualification)
                validate_qualification_evidence(qualification)
            except Exception as exc:
                raise GateFailure(f"qualification evidence is invalid: {exc}") from exc
            qualification_evidence = qualification.get("evidence")
            if (
                qualification.get("board_id") != DEFAULT_BOARD_ID
                or qualification.get("product_fingerprint") != identities["product_fingerprint"]
                or qualification.get("grader_fingerprint") != identities["grader_fingerprint"]
                or qualification.get("hardware_scoring_fingerprint")
                != identities["hardware_scoring_fingerprint"]
                or qualification.get("scenario_fingerprints")
                != identities["scenario_fingerprints"]
                or not isinstance(qualification_evidence, dict)
                or Path(str(qualification_evidence.get("bench_result") or "")).resolve()
                != bench_result_path.resolve()
                or qualification_evidence.get("bench_result_sha256")
                != evidence["bench_result_sha256"]
            ):
                raise GateFailure("qualification record does not exactly own this planned bench result")
            target_suite = str(self.plan["target"]["suite"])
            policy, catalog_hash = resolve_target_policy(
                worktree / "tools" / "hardware_metric_catalog.json",
                target_suite,
                str(self.plan["target"]["metric"]),
            )
            if policy_record(policy, target_suite, catalog_hash) != self.plan["target"]:
                raise GateFailure("target policy changed during the experiment")
            validated_suites = {
                suite: validate_suite_artifacts(
                    run_dir,
                    suite,
                    arm=arm,
                    plan=self.plan,
                    identities=identities,
                )
                for suite in ("core", "display", "replay")
            }
            target_manifest_path = validated_suites[target_suite]["paths"]["manifest.json"]
            target_value = extract_target_value(target_manifest_path, policy, target_suite)
            metric_manifest = validated_suites[target_suite]["manifest"]
            metric_run_id = metric_manifest.get("run_id")
            if not isinstance(metric_run_id, str) or not metric_run_id:
                raise GateFailure("target suite manifest has no run ID")
            suite_artifacts = {
                suite: {
                    name: {
                        "path": str(path),
                        "sha256": sha256_file(path),
                    }
                    for name, path in validated_suites[suite]["paths"].items()
                }
                for suite in ("core", "display", "replay")
            }
        except (GateFailure, OSError, InvalidInput) as exc:
            evidence["result"] = "EVIDENCE_INVALID"
            evidence["validation_error"] = str(exc)
            if qualification_path.is_file():
                evidence["qualification"] = str(qualification_path)
                evidence["qualification_sha256"] = sha256_file(qualification_path)
            return evidence
        evidence.update(
            {
                "result": "PASS",
                "target_value": target_value,
                "target_unit": policy.unit,
                "metric_run_id": metric_run_id,
                "qualification": str(qualification_path),
                "qualification_sha256": sha256_file(qualification_path),
                "qualification_id": qualification["qualification_id"],
                "suite_artifacts": suite_artifacts,
            }
        )
        return evidence

    def _verified_suite_inputs(
        self, run: Mapping[str, Any], suite: str
    ) -> dict[str, dict[str, Any]]:
        arm = str(run.get("arm") or "")
        if arm not in {"baseline", "candidate"}:
            raise GateFailure("cross-arm run has an invalid arm")
        run_dir_text = run.get("run_dir")
        if not isinstance(run_dir_text, str):
            raise GateFailure("cross-arm run has no owned run directory")
        run_dir = Path(os.path.abspath(run_dir_text))
        assert_owned_path_chain(self.session_dir, run_dir, label="cross-arm run directory")
        validated = validate_suite_artifacts(
            run_dir,
            suite,
            arm=arm,
            plan=self.plan,
            identities=self.context["identities"][arm],
        )
        stored_suites = run.get("suite_artifacts")
        if not isinstance(stored_suites, dict) or not isinstance(stored_suites.get(suite), dict):
            raise GateFailure(f"{suite} stored artifact citations are missing")
        stored = stored_suites[suite]
        citations: dict[str, dict[str, Any]] = {}
        for name, path in validated["paths"].items():
            citation = stored.get(name)
            if not isinstance(citation, dict):
                raise GateFailure(f"{suite}/{name} stored artifact citation is invalid")
            verified = verify_owned_citation(
                self.session_dir,
                citation.get("path"),
                citation.get("sha256"),
                label=f"{arm} run {run.get('arm_index')} {suite}/{name}",
                expected_path=path,
            )
            citations[name] = {
                "path": str(verified),
                "sha256": str(citation["sha256"]),
            }
        return citations

    def validate_regressions(self, runs: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
        baseline_runs = [run for run in runs if run.get("arm") == "baseline"]
        candidate_runs = [run for run in runs if run.get("arm") == "candidate"]
        expected = int(self.plan["runs_per_arm"])
        if len(baseline_runs) != expected or len(candidate_runs) != expected:
            raise GateFailure("cross-arm scoring does not own exactly N runs per arm")
        output_dir = self.session_dir / "cross_arm_scoring"
        ensure_owned_directory(
            self.session_dir,
            output_dir,
            label="cross-arm scoring directory",
        )
        records: list[dict[str, Any]] = []
        catalog = self.candidate_worktree / "tools" / "hardware_metric_catalog.json"
        planned_catalog_hash = self.plan["target"]["catalog_sha256"]
        for candidate in candidate_runs:
            for suite in ("core", "display", "replay"):
                self.stop.check()
                verified_catalog = verify_owned_citation(
                    self.session_dir,
                    str(catalog),
                    planned_catalog_hash,
                    label="cross-arm hardware metric catalog",
                )
                catalog_input = {
                    "path": str(verified_catalog),
                    "sha256": planned_catalog_hash,
                }
                candidate_inputs = self._verified_suite_inputs(candidate, suite)
                baseline_inputs = [
                    self._verified_suite_inputs(run, suite) for run in baseline_runs
                ]
                candidate_manifest = Path(candidate_inputs["manifest.json"]["path"])
                baselines = [Path(inputs["manifest.json"]["path"]) for inputs in baseline_inputs]
                name = f"candidate-{int(candidate['arm_index']):02d}-{suite}.json"
                path = output_dir / name
                try:
                    scoring = score_run(candidate_manifest, catalog, baselines)
                except Exception as exc:
                    scoring = {
                        "schema_version": SCORING_SCHEMA_VERSION,
                        "result": "ERROR",
                        "summary": {"reason": str(exc)},
                    }
                self.stop.check()
                candidate_after = self._verified_suite_inputs(candidate, suite)
                baselines_after = [
                    self._verified_suite_inputs(run, suite) for run in baseline_runs
                ]
                if candidate_after != candidate_inputs or baselines_after != baseline_inputs:
                    raise GateFailure("cross-arm input citations changed while scoring")
                verified_catalog_after = verify_owned_citation(
                    self.session_dir,
                    str(catalog),
                    planned_catalog_hash,
                    label="cross-arm hardware metric catalog",
                )
                if verified_catalog_after != verified_catalog:
                    raise GateFailure("hardware metric catalog changed while scoring")
                write_exclusive_json(path, scoring)
                baseline_window = scoring.get("baseline_window")
                candidate_count = (
                    baseline_window.get("candidate_count")
                    if isinstance(baseline_window, dict)
                    else None
                )
                summary = scoring.get("summary") if isinstance(scoring.get("summary"), dict) else {}
                result = str(scoring.get("result") or "ERROR")
                if (
                    candidate_count != expected
                    or summary.get("hard_failures") != 0
                    or summary.get("advisory_failures") != 0
                    or result != "PASS"
                ):
                    result = "FAIL"
                records.append(
                    {
                        "candidate_arm_index": candidate["arm_index"],
                        "suite": suite,
                        "result": result,
                        "baseline_count": candidate_count,
                        "scoring": str(path),
                        "scoring_sha256": sha256_file(path),
                        "hard_failures": summary.get("hard_failures"),
                        "advisory_failures": summary.get("advisory_failures"),
                        "inputs": {
                            "catalog": catalog_input,
                            "candidate": candidate_inputs,
                            "baselines": [
                                {
                                    "arm_index": run["arm_index"],
                                    "artifacts": inputs,
                                }
                                for run, inputs in zip(baseline_runs, baseline_inputs)
                            ],
                        },
                    }
                )
        return records

    def _assert_staged_base_tree(self, worktree: Path, base: str) -> None:
        """Require the index and worktree to be exactly the pinned base tree."""
        status = run_capture(["git", "status", "--porcelain=v1", "-uall"], cwd=worktree)
        if any(line.startswith("??") for line in status.splitlines()):
            raise GateFailure("evaluation worktree has untracked files before the revert commit")
        unstaged_status, _, _ = run_capture_optional(
            ["git", "diff", "--quiet", "--"], cwd=worktree
        )
        if unstaged_status != 0:
            raise GateFailure("evaluation worktree has unstaged changes before the revert commit")
        index_matches_base, _, _ = run_capture_optional(
            ["git", "diff", "--cached", "--quiet", base, "--"], cwd=worktree
        )
        if index_matches_base != 0:
            raise GateFailure("staged evaluation tree does not equal the pinned base tree")

    def finalize_evaluation(self) -> dict[str, Any]:
        if not self.candidate_worktree.is_dir():
            status, ref_sha, _ = run_capture_optional(
                ["git", "show-ref", "--verify", f"refs/heads/{self.plan['evaluation_branch']}"],
                cwd=ROOT,
            )
            if status == 0:
                raise GateFailure(
                    "evaluation branch exists but its owned worktree is unavailable"
                )
            return {"message": "evaluation worktree was not created"}

        worktree = self.candidate_worktree
        assert_owned_path_chain(self.session_dir, worktree, label="candidate worktree")
        top = Path(run_capture(["git", "rev-parse", "--show-toplevel"], cwd=worktree)).resolve()
        common = Path(
            run_capture(
                ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                cwd=worktree,
            )
        ).resolve()
        expected_ref = f"refs/heads/{self.plan['evaluation_branch']}"
        status, symbolic_ref, _ = run_capture_optional(
            ["git", "symbolic-ref", "-q", "HEAD"], cwd=worktree
        )
        head = run_capture(["git", "rev-parse", "HEAD"], cwd=worktree)
        evaluation_head = run_capture(["git", "rev-parse", expected_ref], cwd=ROOT)
        source_candidate = run_capture(
            ["git", "rev-parse", f"refs/heads/{self.plan['candidate_branch']}"], cwd=ROOT
        )
        if (
            top != worktree.resolve()
            or common != (ROOT / ".git").resolve()
            or status != 0
            or symbolic_ref != expected_ref
            or evaluation_head != head
            or source_candidate != self.plan["candidate_sha"]
        ):
            raise GateFailure("evaluation ownership changed before finalization")

        candidate = str(self.plan["candidate_sha"])
        base = str(self.plan["base_sha"])
        worktree_status = run_capture(
            ["git", "status", "--porcelain=v1", "-uall"], cwd=worktree
        )
        if head == candidate and worktree_status:
            unstaged_status, _, _ = run_capture_optional(
                ["git", "diff", "--quiet", "--"], cwd=worktree
            )
            index_matches_base, _, _ = run_capture_optional(
                ["git", "diff", "--cached", "--quiet", base, "--"], cwd=worktree
            )
            has_untracked = any(line.startswith("??") for line in worktree_status.splitlines())
            exact_staged_base = unstaged_status == 0 and index_matches_base == 0
            if not exact_staged_base and not has_untracked:
                index_matches_candidate, _, _ = run_capture_optional(
                    ["git", "diff", "--cached", "--quiet", candidate, "--"], cwd=worktree
                )
                worktree_matches_base, _, _ = run_capture_optional(
                    ["git", "diff", "--quiet", base, "--"], cwd=worktree
                )
                if index_matches_candidate == 0 and worktree_matches_base == 0:
                    assert_repository_hook_contract(worktree)
                    stage_name = self._next_command_attempt_name(
                        "stage-evaluation-base-tree"
                    )
                    self._run(
                        stage_name,
                        ["git", "add", "-u", "--"],
                        worktree,
                        recovery=True,
                    )
                    unstaged_status, _, _ = run_capture_optional(
                        ["git", "diff", "--quiet", "--"], cwd=worktree
                    )
                    index_matches_base, _, _ = run_capture_optional(
                        ["git", "diff", "--cached", "--quiet", base, "--"],
                        cwd=worktree,
                    )
                    exact_staged_base = (
                        unstaged_status == 0 and index_matches_base == 0
                    )
            if not exact_staged_base or has_untracked:
                raise GateFailure(
                    "dirty evaluation worktree is not the exact staged pinned-base tree"
                )
            revert_status, _, _ = run_capture_optional(
                ["git", "rev-parse", "--verify", "-q", "REVERT_HEAD"], cwd=worktree
            )
            assert_repository_hook_contract(worktree)
            command_name = self._next_command_attempt_name("revert-evaluation-branch")
            if revert_status == 0:
                command = ["git", "-c", "commit.gpgSign=false", "revert", "--continue"]
            else:
                subject = run_capture(["git", "log", "-1", "--format=%s", candidate], cwd=worktree)
                command = [
                    "git",
                    "-c",
                    "commit.gpgSign=false",
                    "commit",
                    "--no-gpg-sign",
                    "-m",
                    f'Revert "{subject}"',
                ]
            self._run(command_name, command, worktree, recovery=True)
        elif head == candidate:
            # Materialize the pinned base tree in the controller-owned evaluation
            # worktree, then commit it with an ordinary `git commit`.
            #
            # `git revert` cannot be used here. On Git versions that stage a merge
            # result it points the `AUTO_MERGE` pseudo-ref at a *tree* object, and
            # the fail-closed privacy reference hook correctly refuses every ref
            # target that is not a commit or tag. That abort is predictable rather
            # than transient, so clean finalization must never depend on it.
            #
            # `read-tree` writes only the index and the worktree, so the single
            # reference update in this path is the commit itself — a commit
            # object, which the hook evaluates normally.
            assert_repository_hook_contract(worktree)
            self._run(
                self._next_command_attempt_name("stage-evaluation-base-tree"),
                ["git", "read-tree", "-u", "--reset", base],
                worktree,
                recovery=True,
            )
            self._assert_staged_base_tree(worktree, base)
            subject = run_capture(["git", "log", "-1", "--format=%s", candidate], cwd=worktree)
            assert_repository_hook_contract(worktree)
            self._run(
                self._next_command_attempt_name("revert-evaluation-branch"),
                [
                    "git",
                    "-c",
                    "commit.gpgSign=false",
                    "commit",
                    "--no-gpg-sign",
                    "-m",
                    f'Revert "{subject}"',
                ],
                worktree,
                recovery=True,
            )

        head = self._assert_worktree_owned("candidate", allow_reverted=True)
        parents = run_capture(
            ["git", "rev-list", "--parents", "-n", "1", head], cwd=worktree
        ).split()
        tree_diff = run_capture(
            ["git", "diff", "--name-only", base, head, "--"], cwd=worktree
        )
        if head in {candidate, base} or parents != [head, candidate] or tree_diff:
            raise GateFailure("evaluation branch is not a clean one-commit revert to the pinned base")
        return {
            "message": "controller evaluation branch reverted; source candidate branch preserved",
            "evaluation_branch": self.plan["evaluation_branch"],
            "revert_commit": head,
        }


def load_plan(session_dir: Path) -> dict[str, Any]:
    if session_dir.exists() and session_dir.is_symlink():
        raise InvalidInput("session path must not be a symlink")
    session_dir = session_dir.resolve()
    plan = read_json_object(session_dir / "plan.json", "improvement plan")
    validate_live_plan(plan, session_dir, require_port=False)
    dry_run_path = session_dir / "dry_run_report.json"
    try:
        dry_run_hash = sha256_owned_regular_file(
            dry_run_path, label="session dry-run proof"
        )
    except GateFailure as exc:
        raise InvalidInput("session dry-run proof is missing or changed") from exc
    if dry_run_hash != plan["dry_run_report_sha256"]:
        raise InvalidInput("session dry-run proof is missing or changed")
    return plan


def verify_source_unchanged(plan: Mapping[str, Any]) -> None:
    if source_snapshot(ROOT) != plan.get("source_snapshot"):
        raise GateFailure("invoking worktree changed during the experiment")
    head = plan.get("source_snapshot", {}).get("head")
    if not isinstance(head, str) or not HEX40.fullmatch(head):
        raise GateFailure("invoking worktree snapshot has no pinned commit")
    verify_worktree_matches_commit(ROOT, head)


def run_live(args: argparse.Namespace) -> int:
    validate_git_environment()
    registry = ActiveSessionRegistry()
    with GlobalLease():
        unresolved = registry.unresolved()
        if unresolved is not None:
            raise InvalidInput(
                f"unfinished Phase-B session requires `resume --session {unresolved}` before a new start"
            )
        dry_run_report = build_dry_run_report()
        if dry_run_report.get("result") != "PASS":
            raise GateFailure("mandatory Phase-B dry-run self-test failed")
        dry_run_bytes = (
            json.dumps(dry_run_report, indent=2, sort_keys=True).encode("utf-8") + b"\n"
        )
        dry_run_hash = sha256_bytes(dry_run_bytes)
        plan = make_live_plan(args, dry_run_report_sha256=dry_run_hash)
        session_dir = Path(str(plan["session_dir"]))
        store = FileEvidenceStore(session_dir)
        write_exclusive_json(session_dir / "plan.json", plan)
        write_exclusive(session_dir / "dry_run_report.json", dry_run_bytes)
        reserve = create_recovery_reserve(session_dir)
        store.update(recovery_reserve=reserve, recovery_reserve_released=False)
        store.event(
            "plan_published",
            {
                "plan_sha256": sha256_owned_regular_file(
                    session_dir / "plan.json",
                    label="immutable improvement plan",
                ),
                "dry_run_report_sha256": sha256_owned_regular_file(
                    session_dir / "dry_run_report.json",
                    label="immutable dry-run proof",
                ),
            },
        )
        registry.register(session_dir)
        print(f"Phase-B session: {session_dir}")
        with StopController() as stop:
            runner = CommandRunner(stop)
            adapter = LiveAdapter(plan, store, runner, stop)
            try:
                decision = execute_experiment(plan, adapter, store)
            finally:
                adapter.close()
        if decision.get("result") in TERMINAL_STATES:
            if registry.unresolved() is not None:
                raise GateFailure("terminal decision did not safely close the active session")
    try:
        verify_source_unchanged(plan)
    except GateFailure as exc:
        print(f"[improve] WARNING: {exc}", file=sys.stderr)
        return 2
    print(f"Phase-B decision: {decision['result']}")
    print(f"Evidence: {decision_evidence_path(store, decision)}")
    if decision["result"] == "ACCEPTED":
        return 0
    if decision["result"] == "REJECTED_NO_IMPROVEMENT":
        return 1
    return 2


def decision_evidence_path(store: FileEvidenceStore, decision: Mapping[str, Any]) -> Path:
    if decision.get("result") in UNRESOLVED_STATES:
        failures = sorted((store.session_dir / "cleanup_failures").glob("attempt-*.json"))
        if failures:
            return failures[-1]
    return store.decision_path


def resume_live(args: argparse.Namespace) -> int:
    requested = Path(args.session).expanduser()
    if os.path.lexists(requested) and requested.is_symlink():
        raise InvalidInput("session path must not be a symlink")
    session_dir = requested.resolve()
    registry = ActiveSessionRegistry()
    with GlobalLease():
        unresolved = registry.unresolved()
        if unresolved is not None and unresolved != session_dir:
            raise InvalidInput(f"another unfinished Phase-B session owns recovery: {unresolved}")
        plan = load_plan(session_dir)
        if plan.get("simulated") is True:
            raise InvalidInput("simulated sessions do not require live recovery")
        if args.port and args.port != plan.get("port"):
            raise InvalidInput("recovery port does not match the immutable session plan")
        validate_serial_port(plan.get("port"), require_exists=True)
        registry.register(session_dir)
        reserve_released = release_recovery_reserve(session_dir)
        store = FileEvidenceStore.open(session_dir)
        if reserve_released:
            store.update(recovery_reserve_released=True)
        with StopController() as stop:
            adapter = LiveAdapter(plan, store, CommandRunner(stop), stop)
            try:
                decision = recover_experiment(plan, adapter, store)
            finally:
                adapter.close()
        if decision.get("result") in TERMINAL_STATES:
            if registry.unresolved() is not None:
                raise GateFailure("recovery decision did not safely close the active session")
    print(f"Phase-B recovery: {decision['result']}")
    print(f"Evidence: {decision_evidence_path(store, decision)}")
    return 0 if decision["result"] in TERMINAL_STATES else 2


def render_status(session_dir: Path) -> dict[str, Any]:
    plan = load_plan(session_dir)
    state = read_json_object(session_dir / "state.json", "improvement state")
    decision_path = session_dir / "decision.json"
    decision = read_json_object(decision_path, "improvement decision") if decision_path.is_file() else None
    return {
        "session": str(session_dir.resolve()),
        "target": plan.get("target"),
        "runs_per_arm": plan.get("runs_per_arm"),
        "status": state.get("status"),
        "completed_runs": len(state.get("runs") or []),
        "candidate_may_be_installed": state.get("candidate_may_be_installed"),
        "decision": decision,
    }


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    dry = subparsers.add_parser("dry-run", help="exercise the full controller with fake hardware")
    dry.add_argument("--json-out", help="optional exclusive path for the deterministic report")

    start = subparsers.add_parser("start", help="start a fresh live candidate experiment")
    start.add_argument("--candidate-branch", required=True)
    start.add_argument("--target-suite", choices=tuple(sorted(SUPPORTED_TARGET_SUITES)), required=True)
    start.add_argument("--target-metric", required=True)
    start.add_argument("--runs", type=int, default=DEFAULT_RUNS)
    start.add_argument("--port", required=True)
    start.add_argument("--base-ref", default="HEAD")
    start.add_argument("--artifact-root", default=str(ROOT / ".artifacts" / "improve"))

    resume = subparsers.add_parser(
        "resume", help="restore base firmware and close an interrupted session; never resume measurements"
    )
    resume.add_argument("--session", required=True)
    resume.add_argument("--port", help="must match the immutable planned port when supplied")

    status = subparsers.add_parser("status", help="show an existing session without changing it")
    status.add_argument("--session", required=True)
    status.add_argument("--json", action="store_true")
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.command == "dry-run":
            report = build_dry_run_report()
            encoded = json.dumps(report, indent=2, sort_keys=True).encode("utf-8") + b"\n"
            if args.json_out:
                output = Path(args.json_out).expanduser().resolve()
                write_exclusive(output, encoded)
                print(f"deterministic Phase-B dry-run PASS: {output}")
            else:
                sys.stdout.buffer.write(encoded)
            return 0 if report["result"] == "PASS" else 2
        if args.command == "start":
            return run_live(args)
        if args.command == "resume":
            return resume_live(args)
        status = render_status(Path(args.session).expanduser().resolve())
        if args.json:
            print(json.dumps(status, indent=2, sort_keys=True))
        else:
            print(f"Phase-B status: {status['status']}")
            print(f"Completed runs: {status['completed_runs']}/{2 * int(status['runs_per_arm'])}")
            print(f"Candidate may be installed: {status['candidate_may_be_installed']}")
        return 0
    except (ImproveError, OSError, ValueError) as exc:
        print(f"[improve] {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
