#!/usr/bin/env python3
"""Validate the pinned bug-squash HIL qualification evidence pack."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from functools import lru_cache
import hashlib
import ipaddress
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any

import authenticate_platformio_packages as build_root
import resolve_hil_board as hil_resolver

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "tools" / "bug_squash_hil_qualification_profile_v1.json"
BUILD_EVIDENCE_GENERATOR = ROOT / "scripts" / "generate_bug_squash_build_evidence.py"
BOARD_INVENTORY_TRUST_ROOT = hil_resolver.DEFAULT_BOARD_TRUST_ROOT
PINNED_PROFILE_SHA256 = "337d8bac5f5e67f1eed92dd2571a34b0b2a86b9b7ea32a727ee4b4d605e4fb0d"
MINIMUM_READY_PROFILE_VERSION = 3
PINNED_PROFILE_VERSION = 8
INTEGRITY_PROVENANCE_VERIFIER_VERSION = 1
AUTHENTICATED_PROVENANCE_VERIFIER_VERSION = 1
AUTHORITATIVE_GIT = Path("/usr/bin/git")
COMMITMENT_ALGORITHM = "sha256-domain-separated-canonical-json-v1"
BOARD_COMMITMENT_ALGORITHM = "sha256-salted-inventory-canonical-json-v1"
BOARD_AUTHENTICATION_ALGORITHM = hil_resolver.INVENTORY_AUTHENTICATION_ALGORITHM
EXPECTED_CASE_IDS = tuple(
    [f"BSC-{number:02d}" for number in range(2, 15)] + ["BSC-16"]
)
FULL_GIT_SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
LOWER_SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
SAFE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
SAFE_PATH_SEGMENT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")
UTC_TIMESTAMP_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$"
)
MAC_RE = re.compile(r"(?i)(?:^|[^0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?:$|[^0-9a-f])")
IPV4_SHAPE_RE = re.compile(r"(?:^|[^0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?:$|[^0-9])")
LONG_SERIAL_RE = re.compile(r"(?i)^[0-9a-f]{12,63}$")
SERIAL_MARKER_RE = re.compile(r"(?i)(?:^|[._-])(?:serial|usb|tty|cu|device|dev)(?:$|[._-])")


class DuplicateJsonKeyError(ValueError):
    """Raised when a JSON object repeats a key."""


@dataclass(frozen=True)
class RepositoryState:
    head_sha: str
    target_commit_utc: datetime
    firmware_version: str
    worktree_clean: bool


def reject_duplicate_json_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKeyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifact",
        required=True,
        help="Path to the local qualification_result.json.",
    )
    parser.add_argument(
        "--expected-git-sha",
        required=True,
        help="Full repository HEAD expected in the evidence pack.",
    )
    return parser.parse_args()


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json_bytes(payload: Any) -> bytes:
    return json.dumps(
        payload,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def canonical_commitment(domain: str, payload: Any) -> str:
    if not isinstance(domain, str) or not domain:
        raise ValueError("commitment domain must be non-empty")
    return sha256_bytes(domain.encode("ascii") + b"\0" + canonical_json_bytes(payload))


def sanitized_git_environment() -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_")
    }
    environment.update(
        {
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_COUNT": "0",
        }
    )
    return environment


def run_authoritative_git(arguments: list[str], *, timeout: int = 10) -> subprocess.CompletedProcess[str]:
    if not AUTHORITATIVE_GIT.is_file():
        raise OSError("authoritative Git executable is unavailable")
    return subprocess.run(
        [str(AUTHORITATIVE_GIT), *arguments],
        cwd=ROOT,
        env=sanitized_git_environment(),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
        timeout=timeout,
    )


def git_blob_sha256(target_sha: str, relative_path: str) -> str:
    paths_errors: list[str] = []
    parsed = parse_tracked_paths([relative_path], "tracked_path", paths_errors)
    if not parsed or paths_errors:
        raise ValueError("tracked provenance path is unsafe")
    if not AUTHORITATIVE_GIT.is_file():
        raise ValueError("authoritative Git executable is unavailable")
    result = subprocess.run(
        [str(AUTHORITATIVE_GIT), "show", f"{target_sha}:{relative_path}"],
        cwd=ROOT,
        env=sanitized_git_environment(),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
        timeout=10,
    )
    if result.returncode != 0:
        raise ValueError("tracked provenance source is missing from target commit")
    return sha256_bytes(result.stdout)


def git_tree_sha256(target_sha: str) -> str:
    result = run_authoritative_git(
        ["ls-tree", "-r", "-z", "--full-tree", target_sha],
        timeout=30,
    )
    if result.returncode != 0:
        raise ValueError("target Git tree could not be enumerated")
    records: list[dict[str, str]] = []
    for raw_record in result.stdout.split("\0"):
        if not raw_record:
            continue
        metadata, separator, path = raw_record.partition("\t")
        fields = metadata.split()
        if not separator or len(fields) != 3:
            raise ValueError("target Git tree record is malformed")
        mode, object_type, object_id = fields
        records.append(
            {"mode": mode, "type": object_type, "object_id": object_id, "path": path}
        )
    if not records:
        raise ValueError("target Git tree is empty")
    return canonical_commitment("v1simple.hil.git-tree.v1", records)


def authoritative_tool_environment(pio_executable: Path) -> dict[str, str]:
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
    environment = {key: value for key, value in os.environ.items() if key in allowed}
    environment["PATH"] = os.pathsep.join(
        (str(pio_executable.parent), "/usr/sbin", "/usr/bin", "/sbin", "/bin")
    )
    return environment


def hash_python_package(
    module_name: str,
    python_command: Path,
    environment: dict[str, str],
) -> str:
    result = subprocess.run(
        [
            str(python_command),
            "-I",
            "-c",
            (
                "import importlib.util; "
                f"spec=importlib.util.find_spec({module_name!r}); "
                "print(spec.origin if spec is not None else '')"
            ),
        ],
        cwd=ROOT,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
        timeout=30,
    )
    origin_raw = result.stdout.strip()
    if result.returncode != 0 or not origin_raw:
        raise ValueError(f"required Python package is unavailable: {module_name}")
    origin = Path(origin_raw).resolve()
    if not origin.is_file():
        raise ValueError(f"required Python package has no importable origin: {module_name}")
    root = origin.parent
    records: list[dict[str, str]] = []
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if "__pycache__" in relative.parts or path.suffix in {".pyc", ".pyo"}:
            continue
        if path.is_symlink():
            raise ValueError(f"required Python package has unsafe files: {module_name}")
        if not path.is_file():
            continue
        records.append(
            {"path": relative.as_posix(), "sha256": file_sha256(path)}
        )
    if not records:
        raise ValueError(f"required Python package is empty: {module_name}")
    return canonical_commitment(f"v1simple.hil.python-package.{module_name}.v1", records)


def _tool_version(
    command: list[str],
    pattern: str,
    environment: dict[str, str],
) -> str:
    result = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=30,
    )
    match = re.search(pattern, result.stdout)
    if result.returncode != 0 or match is None:
        raise ValueError("required build tool identity could not be established")
    return match.group(0)


@lru_cache(maxsize=1)
def current_build_tool_identity() -> dict[str, Any]:
    pio_raw = shutil.which("pio")
    if pio_raw is None:
        raise ValueError("PlatformIO executable is unavailable")
    pio = Path(pio_raw).resolve()
    git = AUTHORITATIVE_GIT.resolve()
    if not pio.is_file() or not git.is_file():
        raise ValueError("required build tool executable is unavailable")
    python_candidates = (
        pio.parent / "python",
        Path(sys.executable),
        Path.home() / ".platformio" / "penv" / "bin" / "python",
    )
    python_command: Path | None = None
    python_package_sha256 = ""
    esptool_package_sha256 = ""
    python_version = ""
    esptool_version = ""
    observed: set[Path] = set()
    environment = authoritative_tool_environment(pio)
    for candidate in python_candidates:
        if candidate in observed or not candidate.is_file():
            continue
        observed.add(candidate)
        try:
            candidate_platformio_sha256 = hash_python_package(
                "platformio",
                candidate,
                environment,
            )
            candidate_esptool_sha256 = hash_python_package(
                "esptool",
                candidate,
                environment,
            )
            candidate_python_version = _tool_version(
                [str(candidate), "--version"],
                r"Python \d+\.\d+\.\d+",
                environment,
            ).removeprefix("Python ")
            candidate_esptool_version = _tool_version(
                [str(candidate), "-m", "esptool", "version"],
                r"esptool v\d+\.\d+\.\d+",
                environment,
            )
        except (OSError, subprocess.SubprocessError, ValueError):
            continue
        python_command = candidate
        python_package_sha256 = candidate_platformio_sha256
        esptool_package_sha256 = candidate_esptool_sha256
        python_version = candidate_python_version
        esptool_version = candidate_esptool_version
        break
    if python_command is None:
        raise ValueError("PlatformIO Python environment is unavailable")
    python = python_command.resolve()
    build_root.authenticate_platformio_launcher(pio, python)
    platformio_root = build_root.authenticate_platformio_core(
        python_package_sha256=python_package_sha256,
    )
    build_root.authenticate_python_package(
        "tool-esptoolpy",
        esptool_package_sha256,
    )
    platformio_packages = build_root.authenticate_platformio_packages(ROOT)
    tools = {
        "schema_version": 2,
        "platformio": {
            "sha256": file_sha256(pio),
            "package_sha256": python_package_sha256,
            "root": platformio_root,
            "version": _tool_version(
                [str(pio), "--version"],
                r"PlatformIO Core, version \d+\.\d+\.\d+",
                environment,
            ),
        },
        "python": {
            "sha256": file_sha256(python),
            "version": python_version,
        },
        "git": {
            "sha256": file_sha256(git),
            "version": _tool_version(
                [str(git), "--version"],
                r"git version \d+\.\d+(?:\.\d+)?",
                environment,
            ),
        },
        "esptool": {
            "sha256": esptool_package_sha256,
            "version": esptool_version,
        },
        "platformio_packages": platformio_packages,
    }
    tools["identity_sha256"] = canonical_commitment(
        "v1simple.hil.build-tools.v1",
        tools,
    )
    return tools


def load_json_bytes(content: bytes, label: str) -> Any:
    try:
        text = content.decode("utf-8")
        return json.loads(text, object_pairs_hook=reject_duplicate_json_keys)
    except UnicodeDecodeError as exc:
        raise ValueError(f"{label} is not UTF-8 JSON") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(
            f"{label} is not valid JSON at {exc.lineno}:{exc.colno}: {exc.msg}"
        ) from exc
    except DuplicateJsonKeyError as exc:
        raise ValueError(f"{label} is not valid JSON: {exc}") from exc


def load_json(path: Path, label: str) -> Any:
    try:
        return load_json_bytes(path.read_bytes(), label)
    except FileNotFoundError as exc:
        raise ValueError(f"{label} not found: {path}") from exc
    except (OSError, ValueError) as exc:
        if isinstance(exc, ValueError):
            raise
        raise ValueError(f"could not read {label} {path}: {exc}") from exc


def reject_unknown_keys(
    value: dict[str, Any],
    expected: set[str],
    prefix: str,
    errors: list[str],
) -> None:
    for field in sorted(expected - value.keys()):
        errors.append(f"{prefix}.{field} is required")
    for field in sorted(value.keys() - expected):
        errors.append(f"{prefix}.{field} is not allowed")


def parse_utc_timestamp(raw: Any, field: str) -> tuple[datetime | None, str | None]:
    if not isinstance(raw, str) or not UTC_TIMESTAMP_RE.fullmatch(raw):
        return None, f"{field} must be an RFC3339 UTC timestamp ending in Z"
    try:
        parsed = datetime.fromisoformat(raw[:-1] + "+00:00")
    except ValueError:
        return None, f"{field} must be a valid RFC3339 UTC timestamp"
    if parsed.tzinfo is None or parsed.utcoffset() != timezone.utc.utcoffset(parsed):
        return None, f"{field} must be UTC"
    return parsed, None


def append_time_errors(
    raw: Any,
    field: str,
    errors: list[str],
    *,
    earliest: datetime | None = None,
    latest: datetime | None = None,
    now: datetime | None = None,
) -> datetime | None:
    parsed, error = parse_utc_timestamp(raw, field)
    if error is not None:
        errors.append(error)
        return None
    assert parsed is not None
    if earliest is not None and parsed < earliest:
        errors.append(f"{field} must not precede the qualification start")
    if latest is not None and parsed > latest:
        errors.append(f"{field} must not follow the qualification completion")
    if now is not None and parsed > now:
        errors.append(f"{field} must not be in the future")
    return parsed


def valid_sha256(raw: Any, field: str, errors: list[str]) -> str | None:
    if not isinstance(raw, str) or not SHA256_RE.fullmatch(raw):
        errors.append(f"{field} must be a 64-character hexadecimal SHA-256")
        return None
    normalized = raw.lower()
    if normalized == "0" * 64:
        errors.append(f"{field} must not be the all-zero SHA-256")
        return None
    return normalized


def is_sensitive_shape(value: str) -> bool:
    if (
        "\x00" in value
        or MAC_RE.search(value) is not None
        or IPV4_SHAPE_RE.search(value) is not None
    ):
        return True
    try:
        ipaddress.ip_address(value.strip("[]"))
        return True
    except ValueError:
        pass
    serial_token = any(
        LONG_SERIAL_RE.fullmatch(token) is not None
        for token in re.split(r"[._-]", value)
    )
    return bool(serial_token or SERIAL_MARKER_RE.search(value))


def parse_slug_list(
    raw: Any,
    field: str,
    errors: list[str],
    *,
    allow_empty: bool = False,
) -> tuple[str, ...]:
    if not isinstance(raw, list) or (not raw and not allow_empty):
        qualifier = "an array" if allow_empty else "a non-empty array"
        errors.append(f"{field} must be {qualifier}")
        return ()
    result: list[str] = []
    seen: set[str] = set()
    for index, value in enumerate(raw):
        if not isinstance(value, str) or LOWER_SLUG_RE.fullmatch(value) is None:
            errors.append(f"{field}[{index}] must be a lowercase slug")
            continue
        if value in seen:
            errors.append(f"{field} has duplicate value: {value}")
            continue
        seen.add(value)
        result.append(value)
    return tuple(result)


def parse_safe_id(raw: Any, field: str, errors: list[str]) -> str | None:
    if (
        not isinstance(raw, str)
        or SAFE_ID_RE.fullmatch(raw) is None
        or is_sensitive_shape(raw)
    ):
        errors.append(f"{field} must be a safe identifier")
        return None
    return raw


def parse_tracked_paths(raw: Any, field: str, errors: list[str]) -> tuple[str, ...]:
    if not isinstance(raw, list):
        errors.append(f"{field} must be an array")
        return ()
    result: list[str] = []
    for index, value in enumerate(raw):
        prefix = f"{field}[{index}]"
        if not isinstance(value, str) or not value or value != PurePosixPath(value).as_posix():
            errors.append(f"{prefix} must be a canonical tracked path")
            continue
        parts = PurePosixPath(value).parts
        if (
            PurePosixPath(value).is_absolute()
            or any(part in {".", ".."} for part in parts)
            or any(SAFE_PATH_SEGMENT_RE.fullmatch(part) is None for part in parts)
        ):
            errors.append(f"{prefix} must be a safe relative tracked path")
            continue
        if value in result:
            errors.append(f"{field} has duplicate path: {value}")
            continue
        result.append(value)
    if result != sorted(result):
        errors.append(f"{field} must be sorted")
    return tuple(result)


def validate_provenance_profile_contracts(
    profile: dict[str, Any],
    blocker_codes: set[str],
    errors: list[str],
) -> None:
    build = profile.get("build_provenance_contract")
    build_status: str | None = None
    if not isinstance(build, dict):
        errors.append("pinned profile build_provenance_contract must be an object")
    else:
        reject_unknown_keys(
            build,
            {
                "schema_version",
                "status",
                "commitment_algorithm",
                "source_tree_scope",
                "required_tool_identities",
            },
            "pinned profile.build_provenance_contract",
            errors,
        )
        build_status = build.get("status")
        if build.get("schema_version") != 1 or build_status != "authenticated":
            errors.append(
                "pinned profile build provenance contract must be authenticated schema 1"
            )
        if build.get("commitment_algorithm") != COMMITMENT_ALGORITHM:
            errors.append("pinned profile build provenance commitment algorithm mismatch")
        if build.get("source_tree_scope") != "full-git-tree":
            errors.append("pinned profile build provenance must bind the full Git tree")
        if build.get("required_tool_identities") != [
            "esptool",
            "git",
            "platformio",
            "platformio-package-set",
            "python",
        ]:
            errors.append("pinned profile build provenance tool identities mismatch")

    board = profile.get("board_provenance_contract")
    board_status: str | None = None
    if not isinstance(board, dict):
        errors.append("pinned profile board_provenance_contract must be an object")
    else:
        reject_unknown_keys(
            board,
            {
                "schema_version",
                "status",
                "commitment_algorithm",
                "salt_bytes",
                "authentication_algorithm",
                "signature_namespace",
                "signer_principal",
                "trust_root_path",
                "trust_root_sha256",
            },
            "pinned profile.board_provenance_contract",
            errors,
        )
        board_status = board.get("status") if isinstance(board.get("status"), str) else None
        if board.get("schema_version") != 2 or board_status != "authenticated":
            errors.append(
                "pinned profile board provenance contract must be authenticated schema 2"
            )
        if board.get("commitment_algorithm") != BOARD_COMMITMENT_ALGORITHM:
            errors.append("pinned profile board commitment algorithm mismatch")
        if board.get("salt_bytes") != 32:
            errors.append("pinned profile board commitment salt must be 32 bytes")
        if board.get("authentication_algorithm") != BOARD_AUTHENTICATION_ALGORITHM:
            errors.append("pinned profile board authentication algorithm mismatch")
        if board.get("signature_namespace") != hil_resolver.INVENTORY_SIGNATURE_NAMESPACE:
            errors.append("pinned profile board signature namespace mismatch")
        if board.get("signer_principal") != hil_resolver.INVENTORY_SIGNER_PRINCIPAL:
            errors.append("pinned profile board signer principal mismatch")
        if board.get("trust_root_path") != (
            BOARD_INVENTORY_TRUST_ROOT.relative_to(ROOT).as_posix()
        ):
            errors.append("pinned profile board trust root path mismatch")
        try:
            trust_root_sha256 = sha256_bytes(BOARD_INVENTORY_TRUST_ROOT.read_bytes())
        except OSError:
            trust_root_sha256 = None
            errors.append("pinned board inventory trust root is unavailable")
        if board.get("trust_root_sha256") != trust_root_sha256:
            errors.append("pinned profile board trust root digest mismatch")

    driver = profile.get("case_driver_contract")
    driver_status: str | None = None
    if not isinstance(driver, dict):
        errors.append("pinned profile case_driver_contract must be an object")
    else:
        reject_unknown_keys(
            driver,
            {"schema_version", "status", "runner_path", "driver_source_paths"},
            "pinned profile.case_driver_contract",
            errors,
        )
        driver_status = driver.get("status") if isinstance(driver.get("status"), str) else None
        if driver.get("schema_version") != 1 or driver_status not in {"active", "blocked"}:
            errors.append("pinned profile case driver contract is invalid")
        runner_paths = parse_tracked_paths(
            [driver.get("runner_path")],
            "pinned profile.case_driver_contract.runner_path",
            errors,
        )
        source_paths = parse_tracked_paths(
            driver.get("driver_source_paths"),
            "pinned profile.case_driver_contract.driver_source_paths",
            errors,
        )
        if not runner_paths:
            errors.append("pinned profile case driver runner path is invalid")
        if driver_status == "active" and not source_paths:
            errors.append("active case driver contract needs tracked driver sources")
        if driver_status == "blocked" and source_paths:
            errors.append("blocked case driver contract must not claim driver sources")

    fault = profile.get("fault_control_contract")
    fault_status: str | None = None
    if not isinstance(fault, dict):
        errors.append("pinned profile fault_control_contract must be an object")
    else:
        reject_unknown_keys(
            fault,
            {
                "schema_version",
                "status",
                "build_kind",
                "implementation_source_paths",
                "test_source_paths",
            },
            "pinned profile.fault_control_contract",
            errors,
        )
        fault_status = fault.get("status") if isinstance(fault.get("status"), str) else None
        if fault.get("schema_version") != 1 or fault_status not in {"active", "blocked"}:
            errors.append("pinned profile fault control contract is invalid")
        if fault.get("build_kind") != "hil-fault":
            errors.append("pinned profile fault control build kind mismatch")
        implementation_paths = parse_tracked_paths(
            fault.get("implementation_source_paths"),
            "pinned profile.fault_control_contract.implementation_source_paths",
            errors,
        )
        test_paths = parse_tracked_paths(
            fault.get("test_source_paths"),
            "pinned profile.fault_control_contract.test_source_paths",
            errors,
        )
        if fault_status == "active" and (not implementation_paths or not test_paths):
            errors.append("active fault control contract needs implementation and tests")
        if fault_status == "blocked" and (implementation_paths or test_paths):
            errors.append("blocked fault control contract must not claim tracked controls")

    activation = profile.get("activation_contract")
    requirements = activation.get("requirements", []) if isinstance(activation, dict) else []
    requirement_status = {
        item.get("id"): item.get("status")
        for item in requirements
        if isinstance(item, dict)
    }
    board_requirement_status = (
        "active" if board_status == "authenticated" else "blocked"
    )
    expected_status = {
        "authenticated-build-generator-provenance": (
            "active" if build_status == "authenticated" else "blocked"
        ),
        "authenticated-board-inventory-resolver-provenance": board_requirement_status,
        "authenticated-case-driver-source-provenance": driver_status,
        "bounded-hil-fault-control": fault_status,
    }
    if requirement_status != expected_status:
        errors.append("pinned profile activation requirements do not match provenance contracts")
    expected_blockers = {
        code
        for code, status in (
            (
                "build-generator-provenance-not-authenticated",
                "active" if build_status == "authenticated" else "blocked",
            ),
            (
                "board-resolution-provenance-not-authenticated",
                board_requirement_status,
            ),
            ("case-source-provenance-not-authenticated", driver_status),
            ("hil-fault-control-not-implemented", fault_status),
        )
        if status == "blocked"
    }
    if blocker_codes != expected_blockers:
        errors.append("pinned profile blockers do not exactly match inactive contracts")


def validate_profile_bytes(content: bytes) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    actual_digest = sha256_bytes(content)
    if actual_digest != PINNED_PROFILE_SHA256:
        errors.append(
            "pinned qualification profile digest mismatch; custom or modified "
            "profiles are forbidden"
        )
        return None, errors
    try:
        profile = load_json_bytes(content, "pinned profile")
    except ValueError as exc:
        return None, [str(exc)]
    if not isinstance(profile, dict):
        return None, ["pinned profile root must be an object"]
    reject_unknown_keys(
        profile,
        {
            "schema_version",
            "profile_id",
            "profile_version",
            "qualification_status",
            "blockers",
            "activation_contract",
            "build_provenance_contract",
            "board_provenance_contract",
            "case_driver_contract",
            "fault_control_contract",
            "allowed_instrumentation_modes",
            "build_contracts",
            "required_cases",
        },
        "pinned profile",
        errors,
    )
    if profile.get("schema_version") != 1:
        errors.append("pinned profile schema_version must be 1")
    if profile.get("profile_id") != "bug-squash-hil-v1":
        errors.append("pinned profile_id mismatch")
    if profile.get("profile_version") != PINNED_PROFILE_VERSION:
        errors.append(f"pinned profile_version must be {PINNED_PROFILE_VERSION}")
    if profile.get("qualification_status") not in {"ready", "blocked"}:
        errors.append("pinned profile qualification_status must be ready or blocked")
    blockers = profile.get("blockers")
    blocker_codes: set[str] = set()
    if not isinstance(blockers, list):
        errors.append("pinned profile blockers must be an array")
    else:
        for index, blocker in enumerate(blockers):
            prefix = f"pinned profile.blockers[{index}]"
            if not isinstance(blocker, dict):
                errors.append(f"{prefix} must be an object")
                continue
            reject_unknown_keys(
                blocker,
                {"code", "affected_component"},
                prefix,
                errors,
            )
            code = blocker.get("code")
            if not isinstance(code, str) or LOWER_SLUG_RE.fullmatch(code) is None:
                errors.append(f"{prefix}.code must be a lowercase slug")
            elif code in blocker_codes:
                errors.append(f"{prefix}.code must be unique")
            else:
                blocker_codes.add(code)
            component = blocker.get("affected_component")
            if not isinstance(component, str) or LOWER_SLUG_RE.fullmatch(component) is None:
                errors.append(f"{prefix}.affected_component must be a lowercase slug")
    activation = profile.get("activation_contract")
    if not isinstance(activation, dict):
        errors.append("pinned profile activation_contract must be an object")
    else:
        reject_unknown_keys(
            activation,
            {
                "minimum_ready_profile_version",
                "required_validator_provenance_version",
                "status",
                "requirements",
            },
            "pinned profile.activation_contract",
            errors,
        )
        if activation.get("minimum_ready_profile_version") != MINIMUM_READY_PROFILE_VERSION:
            errors.append(
                "pinned profile activation_contract minimum ready version mismatch"
            )
        if activation.get("required_validator_provenance_version") != 1:
            errors.append(
                "pinned profile activation_contract provenance verifier version mismatch"
            )
        if activation.get("status") not in {"blocked", "active"}:
            errors.append("pinned profile activation_contract status is invalid")
        requirements = activation.get("requirements")
        if not isinstance(requirements, list) or not requirements:
            errors.append("pinned profile activation requirements must be non-empty")
        else:
            seen_requirements: set[str] = set()
            for index, requirement in enumerate(requirements):
                prefix = f"pinned profile.activation_contract.requirements[{index}]"
                if not isinstance(requirement, dict):
                    errors.append(f"{prefix} must be an object")
                    continue
                reject_unknown_keys(requirement, {"id", "status"}, prefix, errors)
                requirement_id = requirement.get("id")
                if (
                    not isinstance(requirement_id, str)
                    or LOWER_SLUG_RE.fullmatch(requirement_id) is None
                ):
                    errors.append(f"{prefix}.id must be a lowercase slug")
                elif requirement_id in seen_requirements:
                    errors.append(f"{prefix}.id must be unique")
                else:
                    seen_requirements.add(requirement_id)
                if requirement.get("status") not in {"blocked", "active"}:
                    errors.append(f"{prefix}.status must be blocked or active")
    cases = profile.get("required_cases")
    if not isinstance(cases, list):
        errors.append("pinned profile required_cases must be an array")
    else:
        ids = [case.get("id") for case in cases if isinstance(case, dict)]
        if tuple(ids) != EXPECTED_CASE_IDS or len(ids) != len(EXPECTED_CASE_IDS):
            errors.append("pinned profile must contain the exact full BSC case set in order")
    build_contracts = profile.get("build_contracts")
    if not isinstance(build_contracts, list):
        errors.append("pinned profile build_contracts must be an array")
    else:
        seen_build_kinds: set[str] = set()
        for index, contract in enumerate(build_contracts):
            prefix = f"pinned profile.build_contracts[{index}]"
            if not isinstance(contract, dict):
                errors.append(f"{prefix} must be an object")
                continue
            reject_unknown_keys(
                contract,
                {
                    "kind",
                    "implementation_status",
                    "blocker_code",
                    "environment",
                    "build_command",
                    "binary_role",
                },
                prefix,
                errors,
            )
            kind = contract.get("kind")
            if not isinstance(kind, str) or LOWER_SLUG_RE.fullmatch(kind) is None:
                errors.append(f"{prefix}.kind must be a lowercase slug")
            elif kind in seen_build_kinds:
                errors.append(f"{prefix}.kind must be unique")
            else:
                seen_build_kinds.add(kind)
            status = contract.get("implementation_status")
            if status == "active":
                environment = contract.get("environment")
                if not isinstance(environment, str) or LOWER_SLUG_RE.fullmatch(environment) is None:
                    errors.append(f"{prefix}.environment must identify an active environment")
                if contract.get("build_command") != ["pio", "run", "-e", environment]:
                    errors.append(f"{prefix}.build_command must select its exact environment")
                if contract.get("blocker_code") is not None:
                    errors.append(f"{prefix}.blocker_code must be null for an active build")
            elif status == "blocked":
                if contract.get("environment") is not None or contract.get("build_command") != []:
                    errors.append(f"{prefix} must not claim an executable blocked environment")
                if contract.get("blocker_code") not in blocker_codes:
                    errors.append(f"{prefix}.blocker_code must reference a declared blocker")
            else:
                errors.append(f"{prefix}.implementation_status must be active or blocked")
            if not isinstance(contract.get("binary_role"), str) or LOWER_SLUG_RE.fullmatch(
                contract["binary_role"]
            ) is None:
                errors.append(f"{prefix}.binary_role must be a lowercase slug")
        if profile.get("qualification_status") == "blocked" and not blocker_codes:
            errors.append("blocked pinned profile must declare at least one blocker")
        if profile.get("qualification_status") == "ready" and blocker_codes:
            errors.append("ready pinned profile must not declare blockers")
    validate_provenance_profile_contracts(profile, blocker_codes, errors)
    return (profile if not errors else None), errors


def load_pinned_profile() -> tuple[dict[str, Any] | None, list[str]]:
    try:
        content = DEFAULT_PROFILE.read_bytes()
    except OSError as exc:
        return None, [f"could not read pinned qualification profile: {exc}"]
    return validate_profile_bytes(content)


def read_repository_state(
    candidate: Any,
    expected_git_sha: Any,
) -> tuple[RepositoryState | None, list[str]]:
    errors: list[str] = []
    if not isinstance(candidate, str) or FULL_GIT_SHA_RE.fullmatch(candidate) is None:
        errors.append("target_git_sha must be a full 40-character hexadecimal git SHA")
        return None, errors
    candidate = candidate.lower()
    if candidate == "0" * 40:
        errors.append("target_git_sha must not be the all-zero git SHA")
    if (
        not isinstance(expected_git_sha, str)
        or FULL_GIT_SHA_RE.fullmatch(expected_git_sha) is None
        or expected_git_sha.lower() == "0" * 40
    ):
        errors.append(
            "expected_git_sha must be a nonzero full 40-character hexadecimal git SHA"
        )
        return None, errors

    try:
        command = run_authoritative_git(
            [
                "show",
                "-s",
                "--format=%H%n%cI",
                candidate,
            ],
            timeout=5,
        )
        head = run_authoritative_git(["rev-parse", "HEAD"], timeout=5)
        config = run_authoritative_git(
            ["show", f"{candidate}:include/config.h"], timeout=5
        )
        status = run_authoritative_git(
            ["status", "--porcelain=v1", "--untracked-files=all"], timeout=5
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        errors.append(f"could not verify target_git_sha against the repository: {exc}")
        return None, errors

    lines = command.stdout.splitlines()
    if command.returncode != 0 or len(lines) != 2:
        errors.append("target_git_sha is not an existing repository commit")
        return None, errors
    resolved_sha = lines[0].strip().lower()
    if resolved_sha != candidate:
        errors.append("target_git_sha did not resolve to the exact requested commit")
    try:
        commit_time = datetime.fromisoformat(lines[1].strip())
    except ValueError:
        errors.append("could not resolve target commit timestamp")
        return None, errors
    commit_time = commit_time.astimezone(timezone.utc)
    if head.returncode != 0 or FULL_GIT_SHA_RE.fullmatch(head.stdout.strip()) is None:
        errors.append("could not resolve repository HEAD")
        return None, errors
    head_sha = head.stdout.strip().lower()
    if candidate != expected_git_sha.lower():
        errors.append("target_git_sha does not match expected_git_sha")
    if candidate != head_sha:
        errors.append("target_git_sha must match repository HEAD")
    if config.returncode != 0:
        errors.append("could not read firmware version from target Git tree")
        firmware_version = ""
    else:
        match = re.search(
            r'^#define\s+FIRMWARE_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"$',
            config.stdout,
            flags=re.MULTILINE,
        )
        if match is None:
            errors.append("target Git tree has no valid FIRMWARE_VERSION")
            firmware_version = ""
        else:
            firmware_version = match.group(1)
    if status.returncode != 0:
        errors.append("could not derive live repository cleanliness")
        clean = False
    else:
        clean = not status.stdout.strip()
        if not clean:
            errors.append("repository worktree must be clean during qualification validation")
    if errors:
        return None, errors
    return RepositoryState(head_sha, commit_time, firmware_version, clean), []


def validate_artifact_path(
    qualification_path: Path,
    raw: Any,
    field: str,
) -> tuple[Path | None, str | None]:
    if not isinstance(raw, str) or not raw or raw != raw.strip() or "\x00" in raw:
        return None, f"{field} must be a nonempty canonical relative POSIX path"
    try:
        posix = PurePosixPath(raw)
        windows = PureWindowsPath(raw)
    except (OSError, ValueError) as exc:
        return None, f"{field} is invalid: {exc}"
    if (
        "\\" in raw
        or posix.is_absolute()
        or windows.is_absolute()
        or windows.drive
        or raw != posix.as_posix()
    ):
        return None, f"{field} must be a canonical relative POSIX path"
    if not posix.parts or len(posix.parts) > 8:
        return None, f"{field} must contain between one and eight safe path segments"
    for segment in posix.parts:
        if (
            segment in {".", ".."}
            or SAFE_PATH_SEGMENT_RE.fullmatch(segment) is None
            or is_sensitive_shape(segment)
        ):
            return None, f"{field} contains an unsafe or operationally identifying path segment"

    try:
        base = qualification_path.parent.resolve(strict=True)
    except (OSError, ValueError) as exc:
        return None, f"could not resolve evidence-pack directory for {field}: {exc}"

    candidate = base
    for index, segment in enumerate(posix.parts):
        try:
            candidate = candidate / segment
            metadata = os.lstat(candidate)
        except (OSError, ValueError) as exc:
            return None, f"could not lstat {field} path component: {exc}"
        if stat.S_ISLNK(metadata.st_mode):
            return None, f"{field} must not contain symlink path components"
        final = index == len(posix.parts) - 1
        if not final and not stat.S_ISDIR(metadata.st_mode):
            return None, f"{field} has a non-directory intermediate component"
        if final and not stat.S_ISREG(metadata.st_mode):
            return None, f"{field} must identify a regular file"
        if final and metadata.st_size <= 0:
            return None, f"{field} must identify a nonempty file"
    return candidate, None


def validate_local_pack_storage(artifact_path: Path, errors: list[str]) -> None:
    """Prevent raw resolver endpoints from living in trackable repo storage."""
    try:
        resolved = artifact_path.resolve(strict=True)
        repository = ROOT.resolve(strict=True)
    except (OSError, ValueError) as exc:
        errors.append(f"could not resolve qualification pack storage: {exc}")
        return
    if repository not in resolved.parents:
        return
    try:
        ignored = subprocess.run(
            ["git", "check-ignore", "--quiet", "--no-index", str(resolved)],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        errors.append(f"could not verify qualification pack ignore status: {exc}")
        return
    if ignored.returncode != 0:
        errors.append(
            "qualification pack inside the repository must be stored below a git-ignored path"
        )


def parse_identity(
    raw: Any,
    field: str,
    prefix: str,
    errors: list[str],
    *,
    additional_keys: set[str] | None = None,
) -> tuple[str | None, tuple[str, ...]]:
    if not isinstance(raw, dict):
        errors.append(f"{field} must be an object")
        return None, ()
    expected_keys = {"alias", "capabilities"}
    if additional_keys is not None:
        expected_keys.update(additional_keys)
    reject_unknown_keys(raw, expected_keys, field, errors)
    alias = raw.get("alias")
    if (
        not isinstance(alias, str)
        or LOWER_SLUG_RE.fullmatch(alias) is None
        or not alias.startswith(prefix)
        or is_sensitive_shape(alias)
    ):
        errors.append(f"{field}.alias must be a sanitized {prefix}* alias")
        alias = None
    capabilities = parse_slug_list(raw.get("capabilities"), f"{field}.capabilities", errors)
    if capabilities and capabilities != tuple(sorted(capabilities)):
        errors.append(f"{field}.capabilities must be sorted")
    return alias, capabilities


def require_pass(raw: Any, field: str, errors: list[str]) -> None:
    if raw == "ACCEPTED_RISK":
        errors.append(f"{field} accepted-risk result cannot close a qualification")
    elif raw != "PASS":
        errors.append(f"{field} must be PASS")


def profile_builds(profile: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        entry["kind"]: entry
        for entry in profile["build_contracts"]
        if entry["implementation_status"] == "active"
    }


def profile_activation_errors(profile: dict[str, Any]) -> list[str]:
    """Return fail-closed readiness blockers for the current profile schema."""
    errors: list[str] = []
    version = profile.get("profile_version")
    if type(version) is not int or version < MINIMUM_READY_PROFILE_VERSION:
        errors.append(
            f"profile version must be at least {MINIMUM_READY_PROFILE_VERSION} before activation"
        )
    if profile.get("qualification_status") != "ready":
        errors.append("qualification_status is not ready")
    blockers = profile.get("blockers")
    if not isinstance(blockers, list):
        errors.append("profile blockers are invalid")
    elif blockers:
        errors.append("profile declares open blockers")
    activation = profile.get("activation_contract")
    if not isinstance(activation, dict):
        errors.append("activation_contract is not active")
        return errors
    if activation.get("status") != "active":
        errors.append("activation_contract is not active")
    required_provenance_version = activation.get(
        "required_validator_provenance_version"
    )
    if AUTHENTICATED_PROVENANCE_VERIFIER_VERSION is None:
        errors.append("authenticated provenance verifier is not implemented")
    elif required_provenance_version != AUTHENTICATED_PROVENANCE_VERIFIER_VERSION:
        errors.append("authenticated provenance verifier version does not match")
    requirements = activation.get("requirements")
    if not isinstance(requirements, list) or not requirements:
        errors.append("activation_contract requirements are missing")
        return errors
    for requirement in requirements:
        if not isinstance(requirement, dict) or requirement.get("status") != "active":
            requirement_id = (
                requirement.get("id", "unknown")
                if isinstance(requirement, dict)
                else "unknown"
            )
            errors.append(f"activation requirement is not active: {requirement_id}")
    return errors


def build_contracts_sha256(profile: dict[str, Any]) -> str:
    return canonical_commitment(
        "v1simple.hil.build-contracts.v1",
        profile["build_contracts"],
    )


def case_definitions_sha256(profile: dict[str, Any]) -> str:
    return canonical_commitment(
        "v1simple.hil.case-definitions.v1",
        profile["required_cases"],
    )


def expected_execution_provenance(
    profile: dict[str, Any],
    target_sha: str,
) -> dict[str, Any]:
    driver = profile["case_driver_contract"]
    fault = profile["fault_control_contract"]
    tracked_paths = sorted(
        {
            driver["runner_path"],
            *driver["driver_source_paths"],
            *fault["implementation_source_paths"],
            *fault["test_source_paths"],
        }
    )
    tracked_sources = [
        {"path": path, "sha256": git_blob_sha256(target_sha, path)}
        for path in tracked_paths
    ]
    payload: dict[str, Any] = {
        "schema_version": 1,
        "target_git_sha": target_sha,
        "profile_sha256": PINNED_PROFILE_SHA256,
        "case_definitions_sha256": case_definitions_sha256(profile),
        "case_driver_contract_sha256": canonical_commitment(
            "v1simple.hil.case-driver-contract.v1",
            driver,
        ),
        "fault_control_contract_sha256": canonical_commitment(
            "v1simple.hil.fault-control-contract.v1",
            fault,
        ),
        "tracked_sources": tracked_sources,
    }
    payload["provenance_sha256"] = canonical_commitment(
        "v1simple.hil.execution-provenance.v1",
        payload,
    )
    return payload


def validate_execution_provenance(
    payload: Any,
    field: str,
    profile: dict[str, Any],
    target_sha: str | None,
    errors: list[str],
) -> str | None:
    if not isinstance(payload, dict):
        errors.append(f"{field} must be an object")
        return None
    reject_unknown_keys(
        payload,
        {
            "schema_version",
            "target_git_sha",
            "profile_sha256",
            "case_definitions_sha256",
            "case_driver_contract_sha256",
            "fault_control_contract_sha256",
            "tracked_sources",
            "provenance_sha256",
        },
        field,
        errors,
    )
    if target_sha is None:
        errors.append(f"{field} cannot be verified without a valid target commit")
        return None
    try:
        expected = expected_execution_provenance(profile, target_sha)
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        errors.append(f"{field} tracked provenance could not be recomputed: {exc}")
        return None
    if payload != expected:
        errors.append(f"{field} must exactly match target-tracked execution provenance")
        return None
    return expected["provenance_sha256"]


def profile_cases(profile: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {entry["id"]: entry for entry in profile["required_cases"]}


def required_case_roles(case: dict[str, Any]) -> dict[str, dict[str, Any]]:
    roles = {case["scenario"]["role_id"]: case["scenario"]}
    replay = case.get("production_replay")
    if replay is not None:
        roles[replay["role_id"]] = replay
    return roles


def artifact_matches(
    entry: dict[str, Any] | None,
    *,
    scope: str,
    role: str,
    artifact_format: str,
    field: str,
    errors: list[str],
) -> bool:
    if entry is None:
        errors.append(f"{field} does not reference an evidence artifact")
        return False
    valid = True
    for key, expected in (
        ("scope", scope),
        ("role", role),
        ("format", artifact_format),
    ):
        if entry.get(key) != expected:
            errors.append(f"{field} must reference {scope}/{role}/{artifact_format}")
            valid = False
    return valid


def build_board_inventory_attestation(
    binding: Any,
    *,
    observed_at_utc: str | None = None,
    board_trust_root_path: Path | None = None,
) -> dict[str, Any]:
    if not isinstance(binding, dict) or set(binding) != {
        "schema_version",
        "commitment_salt_hex",
        "inventory_authentication",
        "inventory_record",
        "resolution",
    }:
        raise ValueError("board binding must use the exact provenance schema")
    if binding.get("schema_version") != 2:
        raise ValueError("board binding schema_version must be 2")
    try:
        authenticated_inventory = hil_resolver.authenticate_inventory_binding(
            binding.get("inventory_authentication"),
            trust_root_path=board_trust_root_path or BOARD_INVENTORY_TRUST_ROOT,
        )
    except hil_resolver.ResolverError as exc:
        raise ValueError(f"board inventory authentication failed: {exc.message}") from exc
    salt = binding.get("commitment_salt_hex")
    if not isinstance(salt, str) or re.fullmatch(r"[0-9a-f]{64}", salt) is None:
        raise ValueError("board binding commitment salt must be 32 lowercase hex bytes")
    inventory_record = binding.get("inventory_record")
    if not isinstance(inventory_record, dict) or set(inventory_record) != {
        "alias",
        "capabilities",
        "connection",
    }:
        raise ValueError("board binding inventory record is invalid")
    alias = inventory_record.get("alias")
    capabilities = inventory_record.get("capabilities")
    connection = inventory_record.get("connection")
    if (
        not isinstance(alias, str)
        or LOWER_SLUG_RE.fullmatch(alias) is None
        or not isinstance(capabilities, list)
        or not capabilities
        or capabilities != sorted(capabilities)
        or len(capabilities) != len(set(capabilities))
        or any(
            not isinstance(capability, str)
            or LOWER_SLUG_RE.fullmatch(capability) is None
            for capability in capabilities
        )
        or not isinstance(connection, dict)
        or set(connection) != {"lan_base_url", "usb_serial"}
        or any(
            value is not None and (not isinstance(value, str) or not value)
            for value in connection.values()
        )
    ):
        raise ValueError("board binding inventory record fields are invalid")
    authenticated_board = authenticated_inventory.boards.get(alias)
    if authenticated_board is None:
        raise ValueError("board binding alias is absent from authenticated inventory")
    authenticated_record = {
        "alias": authenticated_board.alias,
        "capabilities": list(authenticated_board.capabilities),
        "connection": {
            "lan_base_url": authenticated_board.lan_base_url,
            "usb_serial": authenticated_board.usb_serial,
        },
    }
    if inventory_record != authenticated_record:
        raise ValueError("board binding record does not match authenticated inventory")
    resolution = binding.get("resolution")
    base_attestation = hil_resolver.build_resolution_attestation(
        resolution,
        observed_at_utc=observed_at_utc,
    )
    if base_attestation["alias"] != alias or not set(
        base_attestation["capabilities"]
    ).issubset(capabilities):
        raise ValueError("board binding resolution does not match inventory record")
    inventory_commitment = canonical_commitment(
        "v1simple.hil.board-inventory.v2",
        {
            "commitment_salt_hex": salt,
            "inventory_record": inventory_record,
            "inventory_sha256": authenticated_inventory.authentication.inventory_sha256,
            "trust_root_sha256": authenticated_inventory.authentication.trust_root_sha256,
        },
    )
    return {
        "schema_version": 3,
        "resolver_schema_version": base_attestation["resolver_schema_version"],
        "board_provenance_schema_version": 2,
        "commitment_algorithm": BOARD_COMMITMENT_ALGORITHM,
        "authentication_algorithm": BOARD_AUTHENTICATION_ALGORITHM,
        "trust_root_sha256": authenticated_inventory.authentication.trust_root_sha256,
        "inventory_sha256": authenticated_inventory.authentication.inventory_sha256,
        "alias": base_attestation["alias"],
        "capabilities": base_attestation["capabilities"],
        "resolution_sha256": base_attestation["resolution_sha256"],
        "inventory_commitment_sha256": inventory_commitment,
        "observed_at_utc": base_attestation["observed_at_utc"],
    }


def validate_resolver_attestation(
    payload: Any,
    field: str,
    artifact_path: Path,
    local_resolution_path: Any,
    dut_alias: str,
    dut_capabilities: tuple[str, ...],
    qualification_start: datetime | None,
    qualification_end: datetime | None,
    now: datetime,
    errors: list[str],
    *,
    board_trust_root_path: Path | None = None,
) -> None:
    if not isinstance(payload, dict):
        errors.append(f"{field} must be an object")
        return
    reject_unknown_keys(
        payload,
        {
            "schema_version",
            "resolver_schema_version",
            "board_provenance_schema_version",
            "commitment_algorithm",
            "authentication_algorithm",
            "trust_root_sha256",
            "inventory_sha256",
            "alias",
            "capabilities",
            "resolution_sha256",
            "inventory_commitment_sha256",
            "observed_at_utc",
        },
        field,
        errors,
    )
    if payload.get("schema_version") != 3:
        errors.append(f"{field}.schema_version must be 3")
    if payload.get("resolver_schema_version") != 1:
        errors.append(f"{field}.resolver_schema_version must be 1")
    if payload.get("board_provenance_schema_version") != 2:
        errors.append(f"{field}.board_provenance_schema_version must be 2")
    if payload.get("commitment_algorithm") != BOARD_COMMITMENT_ALGORITHM:
        errors.append(f"{field}.commitment_algorithm must match the pinned board contract")
    if payload.get("authentication_algorithm") != BOARD_AUTHENTICATION_ALGORITHM:
        errors.append(
            f"{field}.authentication_algorithm must match the pinned board contract"
        )
    valid_sha256(payload.get("trust_root_sha256"), f"{field}.trust_root_sha256", errors)
    valid_sha256(payload.get("inventory_sha256"), f"{field}.inventory_sha256", errors)
    if payload.get("alias") != dut_alias:
        errors.append(f"{field}.alias must match the approved board alias")
    capabilities = parse_slug_list(
        payload.get("capabilities"),
        f"{field}.capabilities",
        errors,
    )
    if capabilities != tuple(sorted(dut_capabilities)):
        errors.append(f"{field}.capabilities must exactly match approved board capabilities")
    valid_sha256(payload.get("resolution_sha256"), f"{field}.resolution_sha256", errors)
    valid_sha256(
        payload.get("inventory_commitment_sha256"),
        f"{field}.inventory_commitment_sha256",
        errors,
    )
    append_time_errors(
        payload.get("observed_at_utc"),
        f"{field}.observed_at_utc",
        errors,
        earliest=qualification_start,
        latest=qualification_end,
        now=now,
    )
    local_path, path_error = validate_artifact_path(
        artifact_path,
        local_resolution_path,
        f"{field}.local_resolution_path",
    )
    if path_error is not None:
        errors.append(path_error)
        return
    assert local_path is not None
    try:
        raw_binding = load_json(local_path, f"{field}.local_resolution_path")
        expected = build_board_inventory_attestation(
            raw_binding,
            observed_at_utc=payload.get("observed_at_utc"),
            board_trust_root_path=board_trust_root_path,
        )
    except (ValueError, hil_resolver.ResolverError) as exc:
        errors.append(f"{field} local resolution could not be attested: {exc}")
        return
    if payload != expected:
        errors.append(f"{field} does not match the recomputed local resolution attestation")


def validate_firmware_image(
    path: Path,
    field: str,
    errors: list[str],
    expected_firmware_version: str | None = None,
) -> None:
    """Require esptool to recognize a complete ESP32-S3 application image."""
    try:
        result = subprocess.run(
            [sys.executable, "-m", "esptool", "image-info", str(path)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        errors.append(f"{field}.binary could not be validated by esptool: {exc}")
        return
    output = result.stdout
    required_markers = (
        "Detected image type: ESP32-S3",
        "Chip ID: 9 (ESP32-S3)",
        "Application Information",
    )
    image_invalid = (
        result.returncode != 0
        or any(marker not in output for marker in required_markers)
        or re.search(r"^Checksum: .+ \(valid\)$", output, flags=re.MULTILINE) is None
        or re.search(r"^Validation hash: .+ \(valid\)$", output, flags=re.MULTILINE)
        is None
    )
    if image_invalid:
        errors.append(
            f"{field}.binary must be a complete esptool-valid ESP32-S3 application image"
        )
    if expected_firmware_version is not None:
        try:
            image = path.read_bytes()
        except OSError as exc:
            errors.append(f"{field}.binary firmware identity could not be read: {exc}")
            return
        version_markers = (
            f"v{expected_firmware_version}".encode("ascii"),
            f"Firmware: {expected_firmware_version}".encode("ascii"),
        )
        if any(marker not in image for marker in version_markers):
            errors.append(
                f"{field}.binary must embed the target Git tree firmware version identity"
            )


def build_input_commitment(
    *,
    target_git_sha: str,
    target_tree_sha256: str,
    firmware_version: str,
    contract: dict[str, Any],
    tools: dict[str, Any],
) -> str:
    return canonical_commitment(
        "v1simple.hil.build-input.v1",
        {
            "target_git_sha": target_git_sha,
            "target_tree_sha256": target_tree_sha256,
            "firmware_version": firmware_version,
            "build_contract": contract,
            "tool_identity_sha256": tools.get("identity_sha256"),
        },
    )


def build_output_commitment(
    *,
    binary_sha256: str,
    log_sha256: str,
    started_at_utc: str,
    completed_at_utc: str,
) -> str:
    return canonical_commitment(
        "v1simple.hil.build-output.v1",
        {
            "binary_sha256": binary_sha256,
            "log_sha256": log_sha256,
            "started_at_utc": started_at_utc,
            "completed_at_utc": completed_at_utc,
        },
    )


def with_provenance_commitment(domain: str, payload: dict[str, Any]) -> dict[str, Any]:
    result = dict(payload)
    result["provenance_sha256"] = canonical_commitment(domain, payload)
    return result


def expected_build_log_header(
    *,
    target_git_sha: str,
    target_tree_sha256: str,
    build_contract_sha256: str,
    tool_identity_sha256: str,
    kind: str,
    environment: str,
    started_at_utc: str,
    completed_at_utc: str,
) -> list[str]:
    return [
        "provenance_schema_version=1",
        f"target_git_sha={target_git_sha}",
        f"target_tree_sha256={target_tree_sha256}",
        f"build_contract_sha256={build_contract_sha256}",
        f"tool_identity_sha256={tool_identity_sha256}",
        f"build_kind={kind}",
        f"environment={environment}",
        f"started_at_utc={started_at_utc}",
        f"completed_at_utc={completed_at_utc}",
        "exit_code=0",
    ]


def validate_build_manifest(
    payload: Any,
    field: str,
    target_sha: str | None,
    target_firmware_version: str | None,
    profile: dict[str, Any],
    artifacts: dict[str, dict[str, Any]],
    qualification_start: datetime | None,
    qualification_end: datetime | None,
    now: datetime,
    references: dict[str, int],
    errors: list[str],
) -> None:
    if not isinstance(payload, dict):
        errors.append(f"{field} must be an object")
        return
    reject_unknown_keys(
        payload,
        {
            "schema_version",
            "target_git_sha",
            "target_tree_sha256",
            "build_contracts_sha256",
            "observed_at_utc",
            "generator",
            "tools",
            "builds",
            "provenance_sha256",
        },
        field,
        errors,
    )
    if payload.get("schema_version") != 3:
        errors.append(f"{field}.schema_version must be 3")
    if target_sha is not None and payload.get("target_git_sha") != target_sha:
        errors.append(f"{field}.target_git_sha must match the qualification target")
    observed_at = append_time_errors(
        payload.get("observed_at_utc"),
        f"{field}.observed_at_utc",
        errors,
        earliest=qualification_start,
        latest=qualification_end,
        now=now,
    )
    expected_tree_sha: str | None = None
    if target_sha is not None:
        try:
            expected_tree_sha = git_tree_sha256(target_sha)
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            errors.append(f"{field}.target_tree_sha256 could not be recomputed: {exc}")
    target_tree_sha = valid_sha256(
        payload.get("target_tree_sha256"),
        f"{field}.target_tree_sha256",
        errors,
    )
    if (
        expected_tree_sha is not None
        and target_tree_sha is not None
        and target_tree_sha != expected_tree_sha
    ):
        errors.append(f"{field}.target_tree_sha256 must match the exact target Git tree")
    expected_contracts_sha = build_contracts_sha256(profile)
    if payload.get("build_contracts_sha256") != expected_contracts_sha:
        errors.append(f"{field}.build_contracts_sha256 must match the pinned contracts")
    generator = payload.get("generator")
    if not isinstance(generator, dict):
        errors.append(f"{field}.generator must be an object")
    else:
        reject_unknown_keys(generator, {"path", "sha256"}, f"{field}.generator", errors)
        if generator.get("path") != "scripts/generate_bug_squash_build_evidence.py":
            errors.append(f"{field}.generator.path must identify the tracked generator")
        if target_sha is not None:
            try:
                expected_generator_sha = git_blob_sha256(target_sha, generator.get("path", ""))
            except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
                errors.append(f"{field}.generator tracked bytes could not be verified: {exc}")
            else:
                if generator.get("sha256") != expected_generator_sha:
                    errors.append(f"{field}.generator.sha256 must match target-tracked bytes")
    tools = payload.get("tools")
    if not isinstance(tools, dict):
        errors.append(f"{field}.tools must be an object")
        tools = {}
    else:
        try:
            expected_tools = current_build_tool_identity()
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            errors.append(f"{field}.tools could not be independently established: {exc}")
        else:
            if tools != expected_tools:
                errors.append(f"{field}.tools must match the independently hashed tool identity")
    raw_builds = payload.get("builds")
    if not isinstance(raw_builds, list):
        errors.append(f"{field}.builds must be an array")
        return
    contracts = profile_builds(profile)
    seen: set[str] = set()
    binary_digests: set[str] = set()
    for index, build in enumerate(raw_builds):
        prefix = f"{field}.builds[{index}]"
        if not isinstance(build, dict):
            errors.append(f"{prefix} must be an object")
            continue
        reject_unknown_keys(
            build,
            {
                "kind",
                "firmware_version",
                "environment",
                "commit_sha",
                "build_command",
                "build_contract_sha256",
                "binary_artifact_id",
                "binary_sha256",
                "log_artifact_id",
                "log_sha256",
                "source_worktree_clean",
                "started_at_utc",
                "completed_at_utc",
                "input_commitment_sha256",
                "output_commitment_sha256",
                "provenance_sha256",
            },
            prefix,
            errors,
        )
        kind = build.get("kind")
        if not isinstance(kind, str) or kind not in contracts:
            errors.append(f"{prefix}.kind is not required by the pinned profile")
            continue
        if kind in seen:
            errors.append(f"{field}.builds has duplicate kind: {kind}")
        seen.add(kind)
        contract = contracts[kind]
        contract_sha = canonical_commitment(
            "v1simple.hil.build-contract.v1",
            contract,
        )
        if build.get("build_contract_sha256") != contract_sha:
            errors.append(f"{prefix}.build_contract_sha256 must match the pinned contract")
        if not isinstance(build.get("firmware_version"), str) or SEMVER_RE.fullmatch(
            build["firmware_version"]
        ) is None:
            errors.append(f"{prefix}.firmware_version must be X.Y.Z")
        elif (
            target_firmware_version is not None
            and build["firmware_version"] != target_firmware_version
        ):
            errors.append(
                f"{prefix}.firmware_version must match target Git tree FIRMWARE_VERSION"
            )
        if build.get("environment") != contract["environment"]:
            errors.append(f"{prefix}.environment must match the pinned build contract")
        if build.get("build_command") != contract["build_command"]:
            errors.append(f"{prefix}.build_command must exactly match the pinned build contract")
        if target_sha is not None and build.get("commit_sha") != target_sha:
            errors.append(f"{prefix}.commit_sha must match the qualification target")
        if build.get("source_worktree_clean") is not True:
            errors.append(f"{prefix}.source_worktree_clean must be boolean true")
        build_started = append_time_errors(
            build.get("started_at_utc"),
            f"{prefix}.started_at_utc",
            errors,
            earliest=qualification_start,
            latest=qualification_end,
            now=now,
        )
        build_completed = append_time_errors(
            build.get("completed_at_utc"),
            f"{prefix}.completed_at_utc",
            errors,
            earliest=qualification_start,
            latest=qualification_end,
            now=now,
        )
        if (
            build_started is not None
            and build_completed is not None
            and build_completed < build_started
        ):
            errors.append(f"{prefix}.completed_at_utc must not precede started_at_utc")
        if (
            build_completed is not None
            and observed_at is not None
            and observed_at < build_completed
        ):
            errors.append(f"{field}.observed_at_utc must not precede a completed build")
        log_artifact_id = parse_safe_id(
            build.get("log_artifact_id"),
            f"{prefix}.log_artifact_id",
            errors,
        )
        if log_artifact_id is not None:
            references[log_artifact_id] = references.get(log_artifact_id, 0) + 1
            log_artifact = artifacts.get(log_artifact_id)
            artifact_matches(
                log_artifact,
                scope="qualification",
                role="build-log",
                artifact_format="text",
                field=f"{prefix}.log_artifact_id",
                errors=errors,
            )
            log_sha = valid_sha256(
                build.get("log_sha256"),
                f"{prefix}.log_sha256",
                errors,
            )
            if (
                log_artifact is not None
                and log_sha is not None
                and log_artifact.get("sha256") != log_sha
            ):
                errors.append(f"{prefix}.log_sha256 must match the retained build log")
            log_path = log_artifact.get("_resolved_path") if log_artifact else None
            if isinstance(log_path, Path):
                try:
                    log_lines = log_path.read_text(encoding="utf-8").splitlines()
                except (OSError, UnicodeError) as exc:
                    errors.append(f"{prefix}.log could not be parsed: {exc}")
                else:
                    expected_header = expected_build_log_header(
                        target_git_sha=str(build.get("commit_sha")),
                        target_tree_sha256=str(payload.get("target_tree_sha256")),
                        build_contract_sha256=contract_sha,
                        tool_identity_sha256=str(tools.get("identity_sha256")),
                        kind=kind,
                        environment=str(build.get("environment")),
                        started_at_utc=str(build.get("started_at_utc")),
                        completed_at_utc=str(build.get("completed_at_utc")),
                    )
                    if log_lines[: len(expected_header)] != expected_header:
                        errors.append(f"{prefix}.log provenance header is forged or inconsistent")
                    if len(log_lines) <= len(expected_header):
                        errors.append(f"{prefix}.log must retain nonempty build process output")
        binary_sha = valid_sha256(build.get("binary_sha256"), f"{prefix}.binary_sha256", errors)
        if binary_sha is not None:
            if binary_sha in binary_digests:
                errors.append(f"{field}.builds must identify distinct firmware binaries")
            binary_digests.add(binary_sha)
        artifact_id = parse_safe_id(
            build.get("binary_artifact_id"),
            f"{prefix}.binary_artifact_id",
            errors,
        )
        if artifact_id is None:
            continue
        references[artifact_id] = references.get(artifact_id, 0) + 1
        artifact = artifacts.get(artifact_id)
        artifact_matches(
            artifact,
            scope="qualification",
            role=contract["binary_role"],
            artifact_format="binary",
            field=f"{prefix}.binary_artifact_id",
            errors=errors,
        )
        if (
            artifact is not None
            and binary_sha is not None
            and artifact.get("sha256") != binary_sha
        ):
            errors.append(f"{prefix}.binary_sha256 must match the retained binary artifact")
        if artifact is not None:
            resolved_path = artifact.get("_resolved_path")
            if isinstance(resolved_path, Path):
                validate_firmware_image(
                    resolved_path,
                    prefix,
                    errors,
                    build.get("firmware_version")
                    if isinstance(build.get("firmware_version"), str)
                    else None,
                )
        firmware_version = build.get("firmware_version")
        binary_sha_value = build.get("binary_sha256")
        log_sha_value = build.get("log_sha256")
        started_value = build.get("started_at_utc")
        completed_value = build.get("completed_at_utc")
        if all(
            isinstance(value, str)
            for value in (
                target_sha,
                target_tree_sha,
                firmware_version,
                binary_sha_value,
                log_sha_value,
                started_value,
                completed_value,
            )
        ) and tools:
            expected_input = build_input_commitment(
                target_git_sha=target_sha,
                target_tree_sha256=target_tree_sha,
                firmware_version=firmware_version,
                contract=contract,
                tools=tools,
            )
            expected_output = build_output_commitment(
                binary_sha256=binary_sha_value,
                log_sha256=log_sha_value,
                started_at_utc=started_value,
                completed_at_utc=completed_value,
            )
            if build.get("input_commitment_sha256") != expected_input:
                errors.append(f"{prefix}.input_commitment_sha256 is not input-bound")
            if build.get("output_commitment_sha256") != expected_output:
                errors.append(f"{prefix}.output_commitment_sha256 is not output-bound")
            build_without_provenance = {
                key: value for key, value in build.items() if key != "provenance_sha256"
            }
            expected_provenance = canonical_commitment(
                "v1simple.hil.build-provenance.v1",
                build_without_provenance,
            )
            if build.get("provenance_sha256") != expected_provenance:
                errors.append(f"{prefix}.provenance_sha256 does not bind the build record")
    for missing in contracts:
        if missing not in seen:
            errors.append(f"{field}.builds is missing required kind: {missing}")
    manifest_without_provenance = {
        key: value for key, value in payload.items() if key != "provenance_sha256"
    }
    expected_manifest_provenance = canonical_commitment(
        "v1simple.hil.build-manifest.v1",
        manifest_without_provenance,
    )
    if payload.get("provenance_sha256") != expected_manifest_provenance:
        errors.append(f"{field}.provenance_sha256 does not bind the complete manifest")


def validate_event_ids(
    raw: Any,
    field: str,
    expected_ids: list[str],
    errors: list[str],
    *,
    event_kind: str,
    qualification_start: datetime | None,
    qualification_end: datetime | None,
    now: datetime,
) -> None:
    if not isinstance(raw, list):
        errors.append(f"{field} must be an array")
        return
    seen: set[str] = set()
    for index, event in enumerate(raw):
        prefix = f"{field}[{index}]"
        if not isinstance(event, dict):
            errors.append(f"{prefix} must be an object")
            continue
        event_id = event.get("id")
        if event_kind == "stimulus":
            keys = {"id", "observed", "at_utc"}
            reject_unknown_keys(event, keys, prefix, errors)
            if event.get("observed") is not True:
                errors.append(f"{prefix}.observed must be boolean true")
            append_time_errors(
                event.get("at_utc"),
                f"{prefix}.at_utc",
                errors,
                earliest=qualification_start,
                latest=qualification_end,
                now=now,
            )
        elif event_kind == "fault":
            keys = {"id", "armed_at_utc", "triggered_at_utc", "cleared_at_utc"}
            reject_unknown_keys(event, keys, prefix, errors)
            armed = append_time_errors(
                event.get("armed_at_utc"),
                f"{prefix}.armed_at_utc",
                errors,
                earliest=qualification_start,
                latest=qualification_end,
                now=now,
            )
            triggered = append_time_errors(
                event.get("triggered_at_utc"),
                f"{prefix}.triggered_at_utc",
                errors,
                earliest=qualification_start,
                latest=qualification_end,
                now=now,
            )
            cleared = append_time_errors(
                event.get("cleared_at_utc"),
                f"{prefix}.cleared_at_utc",
                errors,
                earliest=qualification_start,
                latest=qualification_end,
                now=now,
            )
            if armed is not None and triggered is not None and triggered < armed:
                errors.append(f"{prefix}.triggered_at_utc must not precede armed_at_utc")
            if triggered is not None and cleared is not None and cleared < triggered:
                errors.append(f"{prefix}.cleared_at_utc must not precede triggered_at_utc")
        else:
            keys = {"id", "ready_at_utc", "released_at_utc", "timed_out"}
            reject_unknown_keys(event, keys, prefix, errors)
            ready = append_time_errors(
                event.get("ready_at_utc"),
                f"{prefix}.ready_at_utc",
                errors,
                earliest=qualification_start,
                latest=qualification_end,
                now=now,
            )
            released = append_time_errors(
                event.get("released_at_utc"),
                f"{prefix}.released_at_utc",
                errors,
                earliest=qualification_start,
                latest=qualification_end,
                now=now,
            )
            if event.get("timed_out") is not False:
                errors.append(f"{prefix}.timed_out must be boolean false")
            if ready is not None and released is not None and released < ready:
                errors.append(f"{prefix}.released_at_utc must not precede ready_at_utc")
        if not isinstance(event_id, str) or event_id not in expected_ids:
            errors.append(f"{prefix}.id is not required by the pinned case role")
            continue
        if event_id in seen:
            errors.append(f"{field} has duplicate id: {event_id}")
        seen.add(event_id)
    for event_id in expected_ids:
        if event_id not in seen:
            errors.append(f"{field} is missing required id: {event_id}")
    if len(raw) != len(expected_ids):
        errors.append(f"{field} must contain exactly the pinned event set")


def validate_facts(
    raw: Any,
    field: str,
    contracts: list[dict[str, Any]],
    errors: list[str],
) -> None:
    if not isinstance(raw, dict):
        errors.append(f"{field} must be an object")
        return
    expected = {contract["id"]: contract for contract in contracts}
    reject_unknown_keys(raw, set(expected), field, errors)
    for fact_id, contract in expected.items():
        if fact_id not in raw:
            continue
        value = raw[fact_id]
        if contract["type"] == "boolean":
            if type(value) is not bool or value is not contract["expected"]:
                errors.append(
                    f"{field}.{fact_id} must be boolean {str(contract['expected']).lower()}"
                )
        elif contract["type"] == "integer":
            if type(value) is not int:
                errors.append(f"{field}.{fact_id} must be an integer")
            elif value < contract["minimum"] or value > contract["maximum"]:
                errors.append(
                    f"{field}.{fact_id} must be between "
                    f"{contract['minimum']} and {contract['maximum']}"
                )
        else:
            errors.append(f"{field}.{fact_id} uses an unsupported pinned fact type")


def validate_case_source(
    source: Any,
    field: str,
    observation: dict[str, Any],
    role: dict[str, Any],
    case_contract: dict[str, Any],
    driver_contract: dict[str, Any],
    execution_provenance_sha256: str | None,
    errors: list[str],
) -> None:
    if not isinstance(source, dict):
        errors.append(f"{field} must be an object")
        return
    reject_unknown_keys(
        source,
        {
            "schema_version",
            "case_id",
            "role_id",
            "run_id",
            "case_definition_sha256",
            "driver_contract_sha256",
            "execution_provenance_sha256",
            "records",
            "source_commitment_sha256",
        },
        field,
        errors,
    )
    if source.get("schema_version") != 1:
        errors.append(f"{field}.schema_version must be 1")
    for key in ("case_id", "role_id", "run_id"):
        if source.get(key) != observation.get(key):
            errors.append(f"{field}.{key} must match its case observation")
    expected_case_sha = canonical_commitment(
        "v1simple.hil.case-definition.v1",
        case_contract,
    )
    expected_driver_sha = canonical_commitment(
        "v1simple.hil.case-driver-contract.v1",
        driver_contract,
    )
    if source.get("case_definition_sha256") != expected_case_sha:
        errors.append(f"{field}.case_definition_sha256 must bind the pinned case")
    if source.get("driver_contract_sha256") != expected_driver_sha:
        errors.append(f"{field}.driver_contract_sha256 must bind tracked driver definitions")
    if (
        execution_provenance_sha256 is None
        or source.get("execution_provenance_sha256") != execution_provenance_sha256
    ):
        errors.append(
            f"{field}.execution_provenance_sha256 must bind authenticated "
            "target-tracked execution provenance"
        )
    if driver_contract.get("status") != "active":
        errors.append(f"{field} cannot authenticate without an active tracked case driver")

    expected: dict[tuple[str, str], tuple[Any, Any]] = {}
    for event in observation.get("stimuli", []):
        if isinstance(event, dict):
            expected[("stimulus", str(event.get("id")))] = (
                event.get("at_utc"),
                True,
            )
    for event in observation.get("faults", []):
        if isinstance(event, dict):
            event_id = str(event.get("id"))
            for phase in ("armed", "triggered", "cleared"):
                expected[(f"fault-{phase}", event_id)] = (
                    event.get(f"{phase}_at_utc"),
                    True,
                )
    for event in observation.get("barriers", []):
        if isinstance(event, dict):
            event_id = str(event.get("id"))
            expected[("barrier-ready", event_id)] = (
                event.get("ready_at_utc"),
                True,
            )
            expected[("barrier-released", event_id)] = (
                event.get("released_at_utc"),
                True,
            )
            expected[("barrier-timeout", event_id)] = (
                event.get("released_at_utc"),
                False,
            )
    completed_at = observation.get("completed_at_utc")
    expected[("isolation", "vbus-isolated")] = (
        completed_at,
        observation.get("vbus_isolated"),
    )
    resets = observation.get("resets")
    if isinstance(resets, dict):
        for key in ("planned", "observed", "unexpected"):
            expected[("reset", key)] = (completed_at, resets.get(key))
    facts = observation.get("facts")
    if isinstance(facts, dict):
        for fact in role["facts"]:
            fact_id = fact["id"]
            expected[("fact", fact_id)] = (completed_at, facts.get(fact_id))

    records = source.get("records")
    if not isinstance(records, list):
        errors.append(f"{field}.records must be an array")
        return
    seen: set[tuple[str, str]] = set()
    previous_time: datetime | None = None
    for index, record in enumerate(records):
        prefix = f"{field}.records[{index}]"
        if not isinstance(record, dict):
            errors.append(f"{prefix} must be an object")
            continue
        reject_unknown_keys(record, {"kind", "id", "at_utc", "value"}, prefix, errors)
        key = (str(record.get("kind")), str(record.get("id")))
        if key not in expected:
            errors.append(f"{prefix} is not required by the case observation")
            continue
        if key in seen:
            errors.append(f"{field}.records has duplicate source record: {key[0]}/{key[1]}")
        seen.add(key)
        expected_time, expected_value = expected[key]
        if record.get("at_utc") != expected_time:
            errors.append(f"{prefix}.at_utc must match the observed event timestamp")
        if type(record.get("value")) is not type(expected_value) or record.get(
            "value"
        ) != expected_value:
            errors.append(f"{prefix}.value must match the observed source value")
        parsed, time_error = parse_utc_timestamp(record.get("at_utc"), f"{prefix}.at_utc")
        if time_error is not None:
            errors.append(time_error)
        elif previous_time is not None and parsed is not None and parsed < previous_time:
            errors.append(f"{field}.records must be causally ordered by timestamp")
        if parsed is not None:
            previous_time = parsed
    for key in expected:
        if key not in seen:
            errors.append(f"{field}.records is missing source record: {key[0]}/{key[1]}")
    if len(records) != len(expected):
        errors.append(f"{field}.records must contain exactly the derived source set")
    source_without_commitment = {
        key: value for key, value in source.items() if key != "source_commitment_sha256"
    }
    expected_source_commitment = canonical_commitment(
        "v1simple.hil.case-source.v1",
        source_without_commitment,
    )
    if source.get("source_commitment_sha256") != expected_source_commitment:
        errors.append(f"{field}.source_commitment_sha256 does not bind the source record")


def validate_case_observation(
    payload: Any,
    field: str,
    case_id: str,
    role: dict[str, Any],
    case_contract: dict[str, Any],
    driver_contract: dict[str, Any],
    execution_provenance_sha256: str | None,
    run_index: int,
    run_id: str,
    dut_alias: str,
    rig_alias: str,
    instrumentation_modes: set[str],
    active_build_kinds: set[str],
    qualification_start: datetime | None,
    qualification_end: datetime | None,
    now: datetime,
    artifacts: dict[str, dict[str, Any]],
    artifact_content: dict[str, Any],
    references: dict[str, int],
    errors: list[str],
) -> None:
    if not isinstance(payload, dict):
        errors.append(f"{field} must be an object")
        return
    reject_unknown_keys(
        payload,
        {
            "schema_version",
            "case_id",
            "role_id",
            "run_index",
            "run_id",
            "dut_alias",
            "rig_alias",
            "build_kind",
            "instrumentation_mode",
            "source_artifact_id",
            "started_at_utc",
            "completed_at_utc",
            "stimuli",
            "faults",
            "barriers",
            "vbus_isolated",
            "resets",
            "facts",
        },
        field,
        errors,
    )
    if payload.get("schema_version") != 1:
        errors.append(f"{field}.schema_version must be 1")
    for key, expected in (
        ("case_id", case_id),
        ("role_id", role["role_id"]),
        ("run_index", run_index),
        ("run_id", run_id),
        ("dut_alias", dut_alias),
        ("rig_alias", rig_alias),
        ("build_kind", role["build_kind"]),
    ):
        if payload.get(key) != expected:
            errors.append(f"{field}.{key} must match the case-run contract")
    if role["build_kind"] not in active_build_kinds:
        errors.append(
            f"{field}.build_kind requires a blocked or unimplemented build contract"
        )
    mode = payload.get("instrumentation_mode")
    role_modes = (
        {"automated", "hybrid"}
        if role["fault_ids"] or role["barrier_ids"]
        else instrumentation_modes
    )
    if not isinstance(mode, str) or mode not in role_modes:
        errors.append(f"{field}.instrumentation_mode is not allowed by the profile")
    started = append_time_errors(
        payload.get("started_at_utc"),
        f"{field}.started_at_utc",
        errors,
        earliest=qualification_start,
        latest=qualification_end,
        now=now,
    )
    completed = append_time_errors(
        payload.get("completed_at_utc"),
        f"{field}.completed_at_utc",
        errors,
        earliest=qualification_start,
        latest=qualification_end,
        now=now,
    )
    if started is not None and completed is not None and completed < started:
        errors.append(f"{field}.completed_at_utc must not precede started_at_utc")

    validate_event_ids(
        payload.get("stimuli"),
        f"{field}.stimuli",
        role["stimulus_ids"],
        errors,
        event_kind="stimulus",
        qualification_start=started,
        qualification_end=completed,
        now=now,
    )
    validate_event_ids(
        payload.get("faults"),
        f"{field}.faults",
        role["fault_ids"],
        errors,
        event_kind="fault",
        qualification_start=started,
        qualification_end=completed,
        now=now,
    )
    validate_event_ids(
        payload.get("barriers"),
        f"{field}.barriers",
        role["barrier_ids"],
        errors,
        event_kind="barrier",
        qualification_start=started,
        qualification_end=completed,
        now=now,
    )
    if type(payload.get("vbus_isolated")) is not bool:
        errors.append(f"{field}.vbus_isolated must be a boolean")
    elif role["vbus_isolation_required"] and payload["vbus_isolated"] is not True:
        errors.append(f"{field}.vbus_isolated must be true for this case role")

    resets = payload.get("resets")
    reset_contract = role["reset_contract"]
    if not isinstance(resets, dict):
        errors.append(f"{field}.resets must be an object")
    else:
        reject_unknown_keys(
            resets,
            {"expected_kind", "planned", "observed", "unexpected"},
            f"{field}.resets",
            errors,
        )
        if resets.get("expected_kind") != reset_contract["expected_kind"]:
            errors.append(f"{field}.resets.expected_kind must match the pinned contract")
        for key in ("planned", "observed"):
            value = resets.get(key)
            if type(value) is not int or value != reset_contract["expected_count"]:
                errors.append(
                    f"{field}.resets.{key} must be integer "
                    f"{reset_contract['expected_count']}"
                )
        unexpected = resets.get("unexpected")
        if type(unexpected) is not int or unexpected != 0:
            errors.append(f"{field}.resets.unexpected must be integer 0")
    validate_facts(payload.get("facts"), f"{field}.facts", role["facts"], errors)
    source_artifact_id = parse_safe_id(
        payload.get("source_artifact_id"),
        f"{field}.source_artifact_id",
        errors,
    )
    if source_artifact_id is None:
        return
    references[source_artifact_id] = references.get(source_artifact_id, 0) + 1
    source_artifact = artifacts.get(source_artifact_id)
    if artifact_matches(
        source_artifact,
        scope=case_id,
        role=f"{role['role_id']}-source",
        artifact_format="json",
        field=f"{field}.source_artifact_id",
        errors=errors,
    ):
        validate_case_source(
            artifact_content.get(source_artifact_id),
            f"{field}.source",
            payload,
            role,
            case_contract,
            driver_contract,
            execution_provenance_sha256,
            errors,
        )


def validate_artifact(
    payload: Any,
    artifact_path: Path,
    expected_git_sha: Any,
    *,
    now: datetime | None = None,
    board_trust_root_path: Path | None = None,
) -> list[str]:
    profile, errors = load_pinned_profile()
    if profile is None:
        return errors
    if now is None:
        now = datetime.now(timezone.utc)
    if not isinstance(payload, dict):
        return errors + ["artifact root must be an object"]
    validate_local_pack_storage(artifact_path, errors)

    reject_unknown_keys(
        payload,
        {
            "schema_version",
            "profile_id",
            "profile_version",
            "profile_sha256",
            "result",
            "target_git_sha",
            "started_at_utc",
            "completed_at_utc",
            "safety",
            "duts",
            "rigs",
            "build_manifest_artifact_id",
            "execution_provenance_artifact_id",
            "evidence_artifacts",
            "cases",
        },
        "artifact",
        errors,
    )
    if payload.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    if payload.get("profile_id") != profile["profile_id"]:
        errors.append(f"profile_id must be {profile['profile_id']}")
    if payload.get("profile_version") != profile["profile_version"]:
        errors.append(f"profile_version must be {profile['profile_version']}")
    if payload.get("profile_sha256") != PINNED_PROFILE_SHA256:
        errors.append("profile_sha256 must match the pinned tracked profile digest")
    require_pass(payload.get("result"), "result", errors)
    activation_errors = profile_activation_errors(profile)
    if activation_errors:
        blocker_codes = ", ".join(
            blocker["code"]
            for blocker in profile["blockers"]
            if isinstance(blocker, dict) and isinstance(blocker.get("code"), str)
        )
        errors.append(
            "pinned qualification profile is blocked and cannot accept PASS evidence"
            + (f": {blocker_codes}" if blocker_codes else "")
        )
        errors.extend(
            f"pinned qualification activation blocked: {error}"
            for error in activation_errors
        )
    repository_state, target_errors = read_repository_state(
        payload.get("target_git_sha"),
        expected_git_sha,
    )
    errors.extend(target_errors)
    target_sha = repository_state.head_sha if repository_state is not None else None
    target_commit_time = (
        repository_state.target_commit_utc
        if repository_state is not None
        else None
    )

    qualification_start = append_time_errors(
        payload.get("started_at_utc"),
        "started_at_utc",
        errors,
        earliest=target_commit_time,
        now=now,
    )
    qualification_end = append_time_errors(
        payload.get("completed_at_utc"),
        "completed_at_utc",
        errors,
        earliest=target_commit_time,
        now=now,
    )
    if (
        qualification_start is not None
        and qualification_end is not None
        and qualification_end < qualification_start
    ):
        errors.append("completed_at_utc must not precede started_at_utc")

    safety = payload.get("safety")
    if not isinstance(safety, dict):
        errors.append("safety must be an object")
    else:
        reject_unknown_keys(
            safety,
            {"unexpected_panics", "unexpected_watchdog_resets"},
            "safety",
            errors,
        )
        for field_name in ("unexpected_panics", "unexpected_watchdog_resets"):
            value = safety.get(field_name)
            if type(value) is not int or value != 0:
                errors.append(f"safety.{field_name} must be integer 0")

    artifacts: dict[str, dict[str, Any]] = {}
    artifact_content: dict[str, Any] = {}
    artifact_paths: set[str] = set()
    raw_artifacts = payload.get("evidence_artifacts")
    if not isinstance(raw_artifacts, list) or not raw_artifacts:
        errors.append("evidence_artifacts must be a non-empty array")
    else:
        for index, entry in enumerate(raw_artifacts):
            prefix = f"evidence_artifacts[{index}]"
            if not isinstance(entry, dict):
                errors.append(f"{prefix} must be an object")
                continue
            reject_unknown_keys(
                entry,
                {"id", "scope", "role", "format", "path", "sha256"},
                prefix,
                errors,
            )
            artifact_id = parse_safe_id(entry.get("id"), f"{prefix}.id", errors)
            scope = entry.get("scope")
            role_name = entry.get("role")
            artifact_format = entry.get("format")
            if not isinstance(scope, str) or (
                scope != "qualification" and scope not in EXPECTED_CASE_IDS
            ):
                errors.append(f"{prefix}.scope is not allowed")
            if not isinstance(role_name, str) or LOWER_SLUG_RE.fullmatch(role_name) is None:
                errors.append(f"{prefix}.role must be a lowercase slug")
            if artifact_format not in {"json", "binary", "text"}:
                errors.append(f"{prefix}.format must be json, binary, or text")
            path_value = entry.get("path")
            if isinstance(path_value, str):
                if path_value in artifact_paths:
                    errors.append(f"duplicate evidence artifact path: {path_value}")
                artifact_paths.add(path_value)
            declared_sha = valid_sha256(entry.get("sha256"), f"{prefix}.sha256", errors)
            file_path, path_error = validate_artifact_path(
                artifact_path,
                path_value,
                f"{prefix}.path",
            )
            if path_error is not None:
                errors.append(path_error)
            actual_sha: str | None = None
            parsed_content: Any = None
            if file_path is not None:
                try:
                    actual_sha = file_sha256(file_path)
                except (OSError, ValueError) as exc:
                    errors.append(f"could not hash {prefix}.path: {exc}")
                if declared_sha is not None and actual_sha != declared_sha:
                    errors.append(f"{prefix}.sha256 does not match file content")
                if artifact_format == "json":
                    try:
                        parsed_content = load_json(
                            file_path,
                            f"{prefix}.path",
                        )
                    except ValueError as exc:
                        errors.append(str(exc))
            if artifact_id is None:
                continue
            if artifact_id in artifacts:
                errors.append(f"duplicate evidence artifact id: {artifact_id}")
                continue
            normalized = dict(entry)
            normalized["sha256"] = declared_sha
            normalized["_resolved_path"] = file_path
            artifacts[artifact_id] = normalized
            if artifact_format == "json" and parsed_content is not None:
                artifact_content[artifact_id] = parsed_content

    references: dict[str, int] = {}
    case_contracts = profile_cases(profile)
    allowed_dut_capabilities = {
        capability
        for case in case_contracts.values()
        for capability in case["required_dut_capabilities"]
    }
    allowed_rig_capabilities = {
        capability
        for case in case_contracts.values()
        for capability in case["required_rig_capabilities"]
    }
    duts: dict[str, tuple[str, ...]] = {}
    local_resolution_paths: set[str] = set()
    raw_duts = payload.get("duts")
    if not isinstance(raw_duts, list) or not raw_duts:
        errors.append("duts must be a non-empty array")
    else:
        for index, dut in enumerate(raw_duts):
            prefix = f"duts[{index}]"
            if not isinstance(dut, dict):
                errors.append(f"{prefix} must be an object")
                continue
            reject_unknown_keys(
                dut,
                {
                    "alias",
                    "capabilities",
                    "resolver_attestation_artifact_id",
                    "local_resolution_path",
                },
                prefix,
                errors,
            )
            alias, capabilities = parse_identity(
                dut,
                prefix,
                "dut-",
                errors,
                additional_keys={
                    "resolver_attestation_artifact_id",
                    "local_resolution_path",
                },
            )
            if alias is None:
                continue
            for capability in capabilities:
                if capability not in allowed_dut_capabilities:
                    errors.append(f"{prefix}.capabilities contains an unapproved capability")
            if alias in duts:
                errors.append(f"duplicate DUT alias: {alias}")
            duts[alias] = capabilities
            attestation_id = parse_safe_id(
                dut.get("resolver_attestation_artifact_id"),
                f"{prefix}.resolver_attestation_artifact_id",
                errors,
            )
            if attestation_id is None:
                continue
            local_resolution_path = dut.get("local_resolution_path")
            if isinstance(local_resolution_path, str):
                if local_resolution_path in local_resolution_paths:
                    errors.append(f"{prefix}.local_resolution_path must be unique")
                local_resolution_paths.add(local_resolution_path)
            references[attestation_id] = references.get(attestation_id, 0) + 1
            attestation_artifact = artifacts.get(attestation_id)
            if artifact_matches(
                attestation_artifact,
                scope="qualification",
                role="resolver-attestation",
                artifact_format="json",
                field=f"{prefix}.resolver_attestation_artifact_id",
                errors=errors,
            ):
                validate_resolver_attestation(
                    artifact_content.get(attestation_id),
                    f"{prefix}.resolver_attestation",
                    artifact_path,
                    local_resolution_path,
                    alias,
                    capabilities,
                    qualification_start,
                    qualification_end,
                    now,
                    errors,
                    board_trust_root_path=board_trust_root_path,
                )

    rigs: dict[str, tuple[str, ...]] = {}
    raw_rigs = payload.get("rigs")
    if not isinstance(raw_rigs, list) or not raw_rigs:
        errors.append("rigs must be a non-empty array")
    else:
        for index, rig in enumerate(raw_rigs):
            prefix = f"rigs[{index}]"
            alias, capabilities = parse_identity(
                rig,
                prefix,
                "rig-",
                errors,
                additional_keys={
                    "resolver_attestation_artifact_id",
                    "local_resolution_path",
                },
            )
            if alias is None:
                continue
            for capability in capabilities:
                if capability not in allowed_rig_capabilities:
                    errors.append(f"{prefix}.capabilities contains an unapproved capability")
            if alias in rigs:
                errors.append(f"duplicate rig alias: {alias}")
            rigs[alias] = capabilities
            attestation_id = parse_safe_id(
                rig.get("resolver_attestation_artifact_id"),
                f"{prefix}.resolver_attestation_artifact_id",
                errors,
            )
            local_resolution_path = rig.get("local_resolution_path")
            if isinstance(local_resolution_path, str):
                if local_resolution_path in local_resolution_paths:
                    errors.append(f"{prefix}.local_resolution_path must be unique")
                local_resolution_paths.add(local_resolution_path)
            if attestation_id is None:
                continue
            references[attestation_id] = references.get(attestation_id, 0) + 1
            attestation_artifact = artifacts.get(attestation_id)
            if artifact_matches(
                attestation_artifact,
                scope="qualification",
                role="resolver-attestation",
                artifact_format="json",
                field=f"{prefix}.resolver_attestation_artifact_id",
                errors=errors,
            ):
                validate_resolver_attestation(
                    artifact_content.get(attestation_id),
                    f"{prefix}.resolver_attestation",
                    artifact_path,
                    local_resolution_path,
                    alias,
                    capabilities,
                    qualification_start,
                    qualification_end,
                    now,
                    errors,
                    board_trust_root_path=board_trust_root_path,
                )

    build_manifest_id = parse_safe_id(
        payload.get("build_manifest_artifact_id"),
        "build_manifest_artifact_id",
        errors,
    )
    if build_manifest_id is not None:
        references[build_manifest_id] = references.get(build_manifest_id, 0) + 1
        build_artifact = artifacts.get(build_manifest_id)
        if artifact_matches(
            build_artifact,
            scope="qualification",
            role="build-manifest",
            artifact_format="json",
            field="build_manifest_artifact_id",
            errors=errors,
        ):
            validate_build_manifest(
                artifact_content.get(build_manifest_id),
                "build_manifest",
                target_sha,
                (
                    repository_state.firmware_version
                    if repository_state is not None
                    else None
                ),
                profile,
                artifacts,
                qualification_start,
                qualification_end,
                now,
                references,
                errors,
            )

    authenticated_execution_provenance_sha256: str | None = None
    execution_provenance_id = parse_safe_id(
        payload.get("execution_provenance_artifact_id"),
        "execution_provenance_artifact_id",
        errors,
    )
    if execution_provenance_id is not None:
        references[execution_provenance_id] = references.get(execution_provenance_id, 0) + 1
        provenance_artifact = artifacts.get(execution_provenance_id)
        if artifact_matches(
            provenance_artifact,
            scope="qualification",
            role="execution-provenance",
            artifact_format="json",
            field="execution_provenance_artifact_id",
            errors=errors,
        ):
            authenticated_execution_provenance_sha256 = validate_execution_provenance(
                artifact_content.get(execution_provenance_id),
                "execution_provenance",
                profile,
                target_sha,
                errors,
            )

    cases_by_id = case_contracts
    raw_cases = payload.get("cases")
    seen_cases: set[str] = set()
    run_ids: set[str] = set()
    if not isinstance(raw_cases, list):
        errors.append("cases must be an array")
    else:
        for case_index, case_result in enumerate(raw_cases):
            case_prefix = f"cases[{case_index}]"
            if not isinstance(case_result, dict):
                errors.append(f"{case_prefix} must be an object")
                continue
            reject_unknown_keys(case_result, {"id", "result", "runs"}, case_prefix, errors)
            case_id = case_result.get("id")
            if not isinstance(case_id, str) or case_id not in cases_by_id:
                errors.append(f"{case_prefix}.id is not required by the pinned profile")
                continue
            if case_id in seen_cases:
                errors.append(f"duplicate case id: {case_id}")
            seen_cases.add(case_id)
            require_pass(case_result.get("result"), f"{case_prefix}.result", errors)
            case_contract = cases_by_id[case_id]
            required_roles = required_case_roles(case_contract)
            raw_runs = case_result.get("runs")
            if (
                not isinstance(raw_runs, list)
                or len(raw_runs) < case_contract["minimum_runs"]
            ):
                errors.append(
                    f"{case_prefix}.runs must contain at least "
                    f"{case_contract['minimum_runs']} distinct runs"
                )
                continue
            for run_offset, run in enumerate(raw_runs):
                run_prefix = f"{case_prefix}.runs[{run_offset}]"
                if not isinstance(run, dict):
                    errors.append(f"{run_prefix} must be an object")
                    continue
                reject_unknown_keys(
                    run,
                    {"run_index", "run_id", "dut_alias", "rig_alias", "roles"},
                    run_prefix,
                    errors,
                )
                run_index = run.get("run_index")
                if type(run_index) is not int or run_index != run_offset + 1:
                    errors.append(f"{run_prefix}.run_index must be the one-based array index")
                    run_index = run_offset + 1
                run_id = parse_safe_id(run.get("run_id"), f"{run_prefix}.run_id", errors)
                if run_id is None or not run_id.startswith("run-") or is_sensitive_shape(run_id):
                    errors.append(f"{run_prefix}.run_id must be a sanitized run-* identifier")
                    run_id = f"invalid-run-{run_offset}"
                if run_id in run_ids:
                    errors.append(f"duplicate qualification run_id: {run_id}")
                run_ids.add(run_id)
                dut_alias = run.get("dut_alias")
                rig_alias = run.get("rig_alias")
                if dut_alias not in duts:
                    errors.append(f"{run_prefix}.dut_alias is not approved")
                    dut_capabilities: tuple[str, ...] = ()
                else:
                    dut_capabilities = duts[dut_alias]
                if rig_alias not in rigs:
                    errors.append(f"{run_prefix}.rig_alias is not approved")
                    rig_capabilities: tuple[str, ...] = ()
                else:
                    rig_capabilities = rigs[rig_alias]
                for required in case_contract["required_dut_capabilities"]:
                    if required not in dut_capabilities:
                        errors.append(
                            f"{run_prefix}.dut_alias lacks required capability: {required}"
                        )
                for required in case_contract["required_rig_capabilities"]:
                    if required not in rig_capabilities:
                        errors.append(
                            f"{run_prefix}.rig_alias lacks required capability: {required}"
                        )

                raw_roles = run.get("roles")
                seen_roles: set[str] = set()
                if not isinstance(raw_roles, list):
                    errors.append(f"{run_prefix}.roles must be an array")
                    continue
                for role_index, role_ref in enumerate(raw_roles):
                    role_prefix = f"{run_prefix}.roles[{role_index}]"
                    if not isinstance(role_ref, dict):
                        errors.append(f"{role_prefix} must be an object")
                        continue
                    reject_unknown_keys(
                        role_ref,
                        {"role_id", "artifact_id"},
                        role_prefix,
                        errors,
                    )
                    role_id = role_ref.get("role_id")
                    role_contract = required_roles.get(role_id)
                    if role_contract is None:
                        errors.append(f"{role_prefix}.role_id is not required for {case_id}")
                        continue
                    if role_id in seen_roles:
                        errors.append(f"{run_prefix}.roles has duplicate role_id: {role_id}")
                    seen_roles.add(role_id)
                    artifact_id = parse_safe_id(
                        role_ref.get("artifact_id"),
                        f"{role_prefix}.artifact_id",
                        errors,
                    )
                    if artifact_id is None:
                        continue
                    references[artifact_id] = references.get(artifact_id, 0) + 1
                    evidence = artifacts.get(artifact_id)
                    if artifact_matches(
                        evidence,
                        scope=case_id,
                        role=role_id,
                        artifact_format="json",
                        field=f"{role_prefix}.artifact_id",
                        errors=errors,
                    ):
                        validate_case_observation(
                            artifact_content.get(artifact_id),
                            f"{role_prefix}.observation",
                            case_id,
                            role_contract,
                            case_contract,
                            profile["case_driver_contract"],
                            authenticated_execution_provenance_sha256,
                            run_index,
                            run_id,
                            dut_alias if isinstance(dut_alias, str) else "",
                            rig_alias if isinstance(rig_alias, str) else "",
                            set(profile["allowed_instrumentation_modes"]),
                            set(profile_builds(profile)),
                            qualification_start,
                            qualification_end,
                            now,
                            artifacts,
                            artifact_content,
                            references,
                            errors,
                        )
                for role_id in required_roles:
                    if role_id not in seen_roles:
                        errors.append(f"{run_prefix}.roles is missing required role_id: {role_id}")
                if len(raw_roles) != len(required_roles):
                    errors.append(f"{run_prefix}.roles must contain exactly the pinned role set")

    for case_id in EXPECTED_CASE_IDS:
        if case_id not in seen_cases:
            errors.append(f"missing required case id: {case_id}")
    for artifact_id in artifacts:
        count = references.get(artifact_id, 0)
        if count == 0:
            errors.append(f"unreferenced evidence artifact id: {artifact_id}")
        elif count > 1:
            errors.append(f"evidence artifact id must be referenced exactly once: {artifact_id}")
    return errors


def validate_artifact_file(
    artifact_path: Path,
    expected_git_sha: Any,
    *,
    now: datetime | None = None,
) -> list[str]:
    try:
        metadata = os.lstat(artifact_path)
    except (OSError, ValueError) as exc:
        return [f"could not lstat artifact file: {exc}"]
    if stat.S_ISLNK(metadata.st_mode):
        return ["artifact file must not be a symlink"]
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_size <= 0:
        return ["artifact file must be a nonempty regular file"]
    try:
        payload = load_json(artifact_path, "artifact")
    except ValueError as exc:
        return [str(exc)]
    return validate_artifact(
        payload,
        artifact_path,
        expected_git_sha,
        now=now,
    )


def main() -> int:
    args = parse_args()
    artifact_path = Path(args.artifact).expanduser()
    errors = validate_artifact_file(artifact_path, args.expected_git_sha)
    if errors:
        print("[bug-squash-hil-qualification] validation failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print(f"[bug-squash-hil-qualification] artifact valid: {artifact_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
