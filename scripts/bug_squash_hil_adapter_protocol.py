#!/usr/bin/env python3
"""Strict request, response, and raw-artifact boundary for HIL rig adapters."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
from typing import Mapping, Sequence

import bug_squash_hil_rig_adapters as rig_adapters


SCHEMA_VERSION = 1
REQUEST_DOMAIN = "v1simple.hil.rig-adapter-request.v1"
MANIFEST_DOMAIN = "v1simple.hil.raw-artifact-manifest.v1"
FULL_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
OPAQUE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,127}$")
ALIAS_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
READ_CHUNK_BYTES = 1024 * 1024


class AdapterProtocolError(ValueError):
    """A public, value-free protocol failure safe for runner output."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def canonical_json_bytes(payload: object) -> bytes:
    try:
        return json.dumps(
            payload,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise AdapterProtocolError("protocol_invalid", "protocol payload is not canonical JSON") from exc


def canonical_commitment(domain: str, payload: object) -> str:
    return hashlib.sha256(domain.encode("ascii") + b"\0" + canonical_json_bytes(payload)).hexdigest()


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate JSON key")
        value[key] = item
    return value


def _exact_object(value: object, keys: set[str], label: str) -> dict[str, object]:
    if not isinstance(value, dict) or set(value) != keys:
        raise AdapterProtocolError("protocol_invalid", f"{label} fields are not exact")
    return value


def _sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        raise AdapterProtocolError("protocol_invalid", f"{label} must be a lowercase SHA-256")
    return value


def _full_sha(value: object) -> str:
    if not isinstance(value, str) or FULL_SHA_RE.fullmatch(value) is None:
        raise AdapterProtocolError("protocol_invalid", "target SHA must be a full lowercase Git SHA")
    return value


def _opaque_id(value: object, label: str) -> str:
    if not isinstance(value, str) or OPAQUE_ID_RE.fullmatch(value) is None:
        raise AdapterProtocolError("protocol_invalid", f"{label} is not a safe opaque identity")
    return value


def _alias(value: object, label: str) -> str:
    if not isinstance(value, str) or ALIAS_RE.fullmatch(value) is None:
        raise AdapterProtocolError("protocol_invalid", f"{label} is not a safe opaque alias")
    return value


def _role_for(
    adapter: rig_adapters.RigAdapter, role_id: object
) -> rig_adapters.AdapterRoleContract:
    if not isinstance(role_id, str):
        raise AdapterProtocolError("protocol_invalid", "adapter role is invalid")
    matches = [role for role in adapter.roles if role.role_id == role_id]
    if len(matches) != 1:
        raise AdapterProtocolError("protocol_invalid", "adapter role is not tracked")
    return matches[0]


def _raw_contract_payload(
    artifacts: Sequence[rig_adapters.RawArtifactContract],
) -> list[dict[str, object]]:
    return [
        {
            "filename": artifact.filename,
            "maximum_bytes": artifact.maximum_bytes,
            "role": artifact.role,
        }
        for artifact in artifacts
    ]


def build_adapter_request(
    *,
    adapter: rig_adapters.RigAdapter,
    target_sha: str,
    profile_id: str,
    profile_version: int,
    profile_sha256: str,
    adapter_source_sha256: str,
    role_id: str,
    run_index: int,
    session_id: str,
    attempt_id: str,
    nonce: str,
    dut_alias: str,
    dut_capabilities: Sequence[str],
    rig_alias: str,
    rig_capabilities: Sequence[str],
    firmware_binary_sha256: str,
) -> dict[str, object]:
    """Create a complete runner-owned request for one adapter invocation."""

    try:
        rig_adapters.validate_adapter_descriptor(adapter)
    except rig_adapters.RigAdapterContractError as exc:
        raise AdapterProtocolError("adapter_contract_invalid", "rig-adapter descriptor is invalid") from exc
    if not adapter.implemented:
        raise AdapterProtocolError("adapter_unavailable", "rig adapter is not implemented")
    role = _role_for(adapter, role_id)
    request: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "protocol_version": rig_adapters.ADAPTER_PROTOCOL_VERSION,
        "target_sha": target_sha,
        "profile": {
            "id": profile_id,
            "version": profile_version,
            "sha256": profile_sha256,
        },
        "case": {
            "id": adapter.case_id,
            "adapter_id": adapter.adapter_id,
            "adapter_source_sha256": adapter_source_sha256,
        },
        "role_id": role.role_id,
        "run_index": run_index,
        "session_id": session_id,
        "attempt_id": attempt_id,
        "nonce": nonce,
        "dut": {"alias": dut_alias, "capabilities": list(dut_capabilities)},
        "rig": {"alias": rig_alias, "capabilities": list(rig_capabilities)},
        "firmware": {
            "environment": role.firmware_environment,
            "binary_sha256": firmware_binary_sha256,
        },
        "raw_artifacts": _raw_contract_payload(role.raw_artifacts),
    }
    request["request_commitment_sha256"] = canonical_commitment(REQUEST_DOMAIN, request)
    validate_adapter_request(request, adapter=adapter)
    return request


def validate_adapter_request(
    payload: object,
    *,
    adapter: rig_adapters.RigAdapter,
    expected: Mapping[str, object] | None = None,
) -> dict[str, object]:
    """Validate an exact request, optionally against the runner-owned original."""

    request = _exact_object(
        payload,
        {
            "schema_version",
            "protocol_version",
            "target_sha",
            "profile",
            "case",
            "role_id",
            "run_index",
            "session_id",
            "attempt_id",
            "nonce",
            "dut",
            "rig",
            "firmware",
            "raw_artifacts",
            "request_commitment_sha256",
        },
        "adapter request",
    )
    try:
        rig_adapters.validate_adapter_descriptor(adapter)
    except rig_adapters.RigAdapterContractError as exc:
        raise AdapterProtocolError("adapter_contract_invalid", "rig-adapter descriptor is invalid") from exc
    if not adapter.implemented:
        raise AdapterProtocolError("adapter_unavailable", "rig adapter is not implemented")
    if (
        type(request.get("schema_version")) is not int
        or request.get("schema_version") != SCHEMA_VERSION
        or type(request.get("protocol_version")) is not int
        or request.get("protocol_version") != adapter.protocol_version
    ):
        raise AdapterProtocolError("protocol_invalid", "adapter request version is invalid")
    _full_sha(request.get("target_sha"))
    profile = _exact_object(request.get("profile"), {"id", "version", "sha256"}, "profile")
    _opaque_id(profile.get("id"), "profile ID")
    if type(profile.get("version")) is not int or profile["version"] <= 0:
        raise AdapterProtocolError("protocol_invalid", "profile version is invalid")
    _sha256(profile.get("sha256"), "profile digest")
    case = _exact_object(
        request.get("case"),
        {"id", "adapter_id", "adapter_source_sha256"},
        "case",
    )
    if case.get("id") != adapter.case_id or case.get("adapter_id") != adapter.adapter_id:
        raise AdapterProtocolError("protocol_invalid", "case adapter identity is substituted")
    _sha256(case.get("adapter_source_sha256"), "adapter source digest")
    role = _role_for(adapter, request.get("role_id"))
    if (
        type(request.get("run_index")) is not int
        or not 1 <= request["run_index"] <= adapter.minimum_runs
    ):
        raise AdapterProtocolError("protocol_invalid", "adapter run index is invalid")
    _opaque_id(request.get("session_id"), "session ID")
    _opaque_id(request.get("attempt_id"), "attempt ID")
    _opaque_id(request.get("nonce"), "request nonce")
    dut = _exact_object(request.get("dut"), {"alias", "capabilities"}, "DUT")
    rig = _exact_object(request.get("rig"), {"alias", "capabilities"}, "rig")
    _alias(dut.get("alias"), "DUT alias")
    _alias(rig.get("alias"), "rig alias")
    if dut.get("alias") == rig.get("alias"):
        raise AdapterProtocolError("protocol_invalid", "DUT and rig aliases must be distinct")
    if dut.get("capabilities") != list(adapter.required_dut_capabilities):
        raise AdapterProtocolError("protocol_invalid", "DUT capabilities do not match the registry")
    if rig.get("capabilities") != list(adapter.required_rig_capabilities):
        raise AdapterProtocolError("protocol_invalid", "rig capabilities do not match the registry")
    firmware = _exact_object(
        request.get("firmware"), {"environment", "binary_sha256"}, "firmware"
    )
    if firmware.get("environment") != role.firmware_environment:
        raise AdapterProtocolError("protocol_invalid", "firmware environment does not match the role")
    _sha256(firmware.get("binary_sha256"), "firmware binary digest")
    if request.get("raw_artifacts") != _raw_contract_payload(role.raw_artifacts):
        raise AdapterProtocolError("protocol_invalid", "raw-artifact roles do not match the registry")
    commitment = _sha256(request.get("request_commitment_sha256"), "request commitment")
    uncommitted = dict(request)
    uncommitted.pop("request_commitment_sha256")
    if not secrets.compare_digest(commitment, canonical_commitment(REQUEST_DOMAIN, uncommitted)):
        raise AdapterProtocolError("protocol_invalid", "adapter request commitment is stale")
    if expected is not None and request != dict(expected):
        raise AdapterProtocolError("protocol_substituted", "adapter request differs from runner-owned request")
    return request


def parse_adapter_request(
    raw: bytes,
    *,
    adapter: rig_adapters.RigAdapter,
    expected: Mapping[str, object],
) -> dict[str, object]:
    try:
        payload = json.loads(raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise AdapterProtocolError("protocol_invalid", "adapter request is not strict JSON") from exc
    return validate_adapter_request(payload, adapter=adapter, expected=expected)


def validate_adapter_response(
    payload: object, *, request: Mapping[str, object]
) -> dict[str, object]:
    """Validate the minimal response; adapter-declared artifact hashes are rejected."""

    response = _exact_object(
        payload,
        {
            "schema_version",
            "protocol_version",
            "status",
            "request_commitment_sha256",
            "nonce",
        },
        "adapter response",
    )
    if (
        type(response.get("schema_version")) is not int
        or response.get("schema_version") != SCHEMA_VERSION
        or type(response.get("protocol_version")) is not int
        or response.get("protocol_version") != rig_adapters.ADAPTER_PROTOCOL_VERSION
        or response.get("status") != "complete"
        or response.get("request_commitment_sha256")
        != request.get("request_commitment_sha256")
        or response.get("nonce") != request.get("nonce")
    ):
        raise AdapterProtocolError("protocol_invalid", "adapter response does not bind its request")
    _sha256(response.get("request_commitment_sha256"), "response request commitment")
    _opaque_id(response.get("nonce"), "response nonce")
    return response


def parse_adapter_response(raw: bytes, *, request: Mapping[str, object]) -> dict[str, object]:
    try:
        payload = json.loads(raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise AdapterProtocolError("protocol_invalid", "adapter response is not strict JSON") from exc
    return validate_adapter_response(payload, request=request)


def _identity(metadata: os.stat_result) -> tuple[int, int, int, int, int]:
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_nlink,
        metadata.st_size,
    )


def _hash_raw_file(
    directory_fd: int,
    filename: str,
    contract: rig_adapters.RawArtifactContract,
) -> tuple[str, int]:
    try:
        before_path = os.stat(filename, dir_fd=directory_fd, follow_symlinks=False)
    except OSError as exc:
        raise AdapterProtocolError("raw_artifact_invalid", "raw artifact could not be inspected") from exc
    if (
        not stat.S_ISREG(before_path.st_mode)
        or before_path.st_nlink != 1
        or not 1 <= before_path.st_size <= contract.maximum_bytes
    ):
        raise AdapterProtocolError("raw_artifact_invalid", "raw artifact type or size is invalid")
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise AdapterProtocolError("raw_artifact_invalid", "no-follow file opening is unavailable")
    flags = os.O_RDONLY | nofollow
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    descriptor = -1
    try:
        descriptor = os.open(filename, flags, dir_fd=directory_fd)
        before_fd = os.fstat(descriptor)
        if _identity(before_fd) != _identity(before_path):
            raise AdapterProtocolError("raw_artifact_changed", "raw artifact changed before hashing")
        digest = hashlib.sha256()
        total = 0
        while True:
            chunk = os.read(descriptor, READ_CHUNK_BYTES)
            if not chunk:
                break
            total += len(chunk)
            if total > contract.maximum_bytes:
                raise AdapterProtocolError("raw_artifact_invalid", "raw artifact exceeded its size limit")
            digest.update(chunk)
        after_fd = os.fstat(descriptor)
        if _identity(after_fd) != _identity(before_fd) or total != after_fd.st_size:
            raise AdapterProtocolError("raw_artifact_changed", "raw artifact changed while hashing")
    except AdapterProtocolError:
        raise
    except OSError as exc:
        raise AdapterProtocolError("raw_artifact_invalid", "raw artifact could not be hashed") from exc
    finally:
        if descriptor >= 0:
            try:
                os.close(descriptor)
            except OSError:
                pass
    try:
        after_path = os.stat(filename, dir_fd=directory_fd, follow_symlinks=False)
    except OSError as exc:
        raise AdapterProtocolError("raw_artifact_changed", "raw artifact changed after hashing") from exc
    if _identity(after_path) != _identity(before_path):
        raise AdapterProtocolError("raw_artifact_changed", "raw artifact path changed while hashing")
    return digest.hexdigest(), total


def _read_raw_file(
    directory_fd: int,
    filename: str,
    contract: rig_adapters.RawArtifactContract,
    *,
    expected_sha256: str,
    expected_size: int,
) -> bytes:
    """Read one bounded artifact from a stable directory entry and reverify its manifest."""

    try:
        before_path = os.stat(filename, dir_fd=directory_fd, follow_symlinks=False)
    except OSError as exc:
        raise AdapterProtocolError("raw_artifact_changed", "raw artifact is unavailable for parsing") from exc
    if (
        not stat.S_ISREG(before_path.st_mode)
        or before_path.st_nlink != 1
        or before_path.st_size != expected_size
        or not 1 <= expected_size <= contract.maximum_bytes
    ):
        raise AdapterProtocolError("raw_artifact_changed", "raw artifact identity changed before parsing")
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise AdapterProtocolError("raw_artifact_invalid", "no-follow file opening is unavailable")
    flags = os.O_RDONLY | nofollow
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    descriptor = -1
    try:
        descriptor = os.open(filename, flags, dir_fd=directory_fd)
        before_fd = os.fstat(descriptor)
        if _identity(before_fd) != _identity(before_path):
            raise AdapterProtocolError("raw_artifact_changed", "raw artifact changed before parsing")
        content = bytearray()
        digest = hashlib.sha256()
        while True:
            chunk = os.read(descriptor, READ_CHUNK_BYTES)
            if not chunk:
                break
            content.extend(chunk)
            if len(content) > contract.maximum_bytes:
                raise AdapterProtocolError("raw_artifact_invalid", "raw artifact exceeded its size limit")
            digest.update(chunk)
        after_fd = os.fstat(descriptor)
        if (
            _identity(after_fd) != _identity(before_fd)
            or len(content) != expected_size
            or not secrets.compare_digest(digest.hexdigest(), expected_sha256)
        ):
            raise AdapterProtocolError("raw_artifact_changed", "raw artifact differs from its manifest")
    except AdapterProtocolError:
        raise
    except OSError as exc:
        raise AdapterProtocolError("raw_artifact_invalid", "raw artifact could not be parsed") from exc
    finally:
        if descriptor >= 0:
            try:
                os.close(descriptor)
            except OSError:
                pass
    try:
        after_path = os.stat(filename, dir_fd=directory_fd, follow_symlinks=False)
    except OSError as exc:
        raise AdapterProtocolError("raw_artifact_changed", "raw artifact changed after parsing") from exc
    if _identity(after_path) != _identity(before_path):
        raise AdapterProtocolError("raw_artifact_changed", "raw artifact path changed while parsing")
    return bytes(content)


def _write_manifest(path: Path, payload: Mapping[str, object]) -> None:
    absolute = Path(os.path.abspath(path))
    parent = absolute.parent
    try:
        parent_metadata = os.stat(parent, follow_symlinks=False)
    except OSError as exc:
        raise AdapterProtocolError("manifest_invalid", "manifest parent is unavailable") from exc
    if not stat.S_ISDIR(parent_metadata.st_mode):
        raise AdapterProtocolError("manifest_invalid", "manifest parent is not a directory")
    try:
        os.stat(absolute, follow_symlinks=False)
    except FileNotFoundError:
        pass
    except OSError as exc:
        raise AdapterProtocolError("manifest_invalid", "manifest destination is unsafe") from exc
    else:
        raise AdapterProtocolError("manifest_invalid", "manifest destination already exists")
    data = canonical_json_bytes(payload) + b"\n"
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise AdapterProtocolError("manifest_invalid", "no-follow manifest creation is unavailable")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | nofollow
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    descriptor = -1
    complete = False
    try:
        descriptor = os.open(absolute, flags, 0o600)
        written = 0
        while written < len(data):
            count = os.write(descriptor, data[written:])
            if count <= 0:
                raise OSError("short manifest write")
            written += count
        os.fsync(descriptor)
        metadata = os.fstat(descriptor)
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_nlink != 1
            or metadata.st_size != len(data)
        ):
            raise AdapterProtocolError("manifest_invalid", "manifest identity is invalid")
        complete = True
    except AdapterProtocolError:
        raise
    except OSError as exc:
        raise AdapterProtocolError("manifest_invalid", "manifest could not be written") from exc
    finally:
        if descriptor >= 0:
            try:
                os.close(descriptor)
            except OSError:
                pass
        if not complete:
            try:
                os.unlink(absolute)
            except OSError:
                pass


def collect_raw_artifacts(
    *,
    raw_directory: Path,
    role: rig_adapters.AdapterRoleContract,
    request_commitment_sha256: str,
    manifest_path: Path,
) -> dict[str, object]:
    """Enumerate and hash the exact runner-declared raw artifact set."""

    request_commitment = _sha256(request_commitment_sha256, "request commitment")
    raw_absolute = Path(os.path.abspath(raw_directory))
    manifest_absolute = Path(os.path.abspath(manifest_path))
    try:
        manifest_absolute.relative_to(raw_absolute)
    except ValueError:
        pass
    else:
        raise AdapterProtocolError("manifest_invalid", "manifest must be outside the raw directory")
    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory_flag = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory_flag is None:
        raise AdapterProtocolError("raw_artifact_invalid", "safe directory opening is unavailable")
    flags = os.O_RDONLY | nofollow | directory_flag
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    directory_fd = -1
    try:
        directory_fd = os.open(raw_absolute, flags)
        before_directory = os.fstat(directory_fd)
        if not stat.S_ISDIR(before_directory.st_mode):
            raise AdapterProtocolError(
                "raw_artifact_invalid", "raw artifact path is not a real directory"
            )
        names = os.listdir(directory_fd)
        if len(names) != len(set(names)):
            raise AdapterProtocolError(
                "raw_artifact_set_invalid", "raw artifact names are not unique"
            )
        expected_names = {artifact.filename for artifact in role.raw_artifacts}
        if set(names) != expected_names:
            raise AdapterProtocolError(
                "raw_artifact_set_invalid", "raw artifact set is missing or contains extras"
            )
        artifacts: list[dict[str, object]] = []
        for contract in role.raw_artifacts:
            if rig_adapters.SAFE_FILENAME_RE.fullmatch(contract.filename) is None:
                raise AdapterProtocolError(
                    "adapter_contract_invalid", "raw artifact filename is unsafe"
                )
            digest, size = _hash_raw_file(directory_fd, contract.filename, contract)
            artifacts.append(
                {
                    "filename": contract.filename,
                    "role": contract.role,
                    "sha256": digest,
                    "size_bytes": size,
                }
            )
        after_directory = os.fstat(directory_fd)
        try:
            after_path = os.stat(raw_absolute, follow_symlinks=False)
        except OSError as exc:
            raise AdapterProtocolError(
                "raw_artifact_changed", "raw artifact directory path changed while hashing"
            ) from exc
        if (
            _identity(after_directory) != _identity(before_directory)
            or _identity(after_path) != _identity(before_directory)
        ):
            raise AdapterProtocolError(
                "raw_artifact_changed", "raw artifact directory changed while hashing"
            )
    except AdapterProtocolError:
        raise
    except OSError as exc:
        raise AdapterProtocolError(
            "raw_artifact_invalid", "raw artifact directory could not be processed"
        ) from exc
    finally:
        if directory_fd >= 0:
            try:
                os.close(directory_fd)
            except OSError:
                pass
    manifest: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "protocol_version": rig_adapters.ADAPTER_PROTOCOL_VERSION,
        "request_commitment_sha256": request_commitment,
        "artifacts": artifacts,
    }
    manifest["manifest_commitment_sha256"] = canonical_commitment(MANIFEST_DOMAIN, manifest)
    _write_manifest(manifest_absolute, manifest)
    return manifest


def read_collected_raw_artifacts(
    *,
    raw_directory: Path,
    role: rig_adapters.AdapterRoleContract,
    manifest: object,
) -> dict[str, bytes]:
    """Return the exact bytes bound by a runner-owned manifest.

    Files are reopened relative to a no-follow directory descriptor and their
    identity, size, and digest are rechecked while reading. Callers can parse
    the returned bytes without reopening an attacker-replaceable path.
    """

    payload = _exact_object(
        manifest,
        {
            "schema_version",
            "protocol_version",
            "request_commitment_sha256",
            "artifacts",
            "manifest_commitment_sha256",
        },
        "raw artifact manifest",
    )
    rows = payload.get("artifacts")
    if (
        type(payload.get("schema_version")) is not int
        or payload.get("schema_version") != SCHEMA_VERSION
        or type(payload.get("protocol_version")) is not int
        or payload.get("protocol_version") != rig_adapters.ADAPTER_PROTOCOL_VERSION
        or not isinstance(rows, list)
        or len(rows) != len(role.raw_artifacts)
    ):
        raise AdapterProtocolError("manifest_invalid", "raw artifact manifest identity is invalid")
    _sha256(payload.get("request_commitment_sha256"), "manifest request commitment")
    commitment = _sha256(payload.get("manifest_commitment_sha256"), "manifest commitment")
    uncommitted = dict(payload)
    uncommitted.pop("manifest_commitment_sha256")
    if not secrets.compare_digest(commitment, canonical_commitment(MANIFEST_DOMAIN, uncommitted)):
        raise AdapterProtocolError("manifest_invalid", "raw artifact manifest commitment is stale")

    validated_rows: list[tuple[rig_adapters.RawArtifactContract, str, int]] = []
    for raw, contract in zip(rows, role.raw_artifacts, strict=True):
        row = _exact_object(raw, {"filename", "role", "sha256", "size_bytes"}, "raw artifact row")
        digest = _sha256(row.get("sha256"), "raw artifact digest")
        size = row.get("size_bytes")
        if (
            row.get("filename") != contract.filename
            or row.get("role") != contract.role
            or type(size) is not int
            or not 1 <= size <= contract.maximum_bytes
        ):
            raise AdapterProtocolError("manifest_invalid", "raw artifact manifest contract drifted")
        validated_rows.append((contract, digest, size))

    raw_absolute = Path(os.path.abspath(raw_directory))
    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory_flag = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory_flag is None:
        raise AdapterProtocolError("raw_artifact_invalid", "safe directory opening is unavailable")
    flags = os.O_RDONLY | nofollow | directory_flag
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    directory_fd = -1
    try:
        directory_fd = os.open(raw_absolute, flags)
        before_directory = os.fstat(directory_fd)
        if not stat.S_ISDIR(before_directory.st_mode):
            raise AdapterProtocolError("raw_artifact_invalid", "raw artifact path is not a real directory")
        names = os.listdir(directory_fd)
        expected_names = {contract.filename for contract in role.raw_artifacts}
        if len(names) != len(set(names)) or set(names) != expected_names:
            raise AdapterProtocolError("raw_artifact_set_invalid", "raw artifact set changed before parsing")
        content_by_role = {
            contract.role: _read_raw_file(
                directory_fd,
                contract.filename,
                contract,
                expected_sha256=digest,
                expected_size=size,
            )
            for contract, digest, size in validated_rows
        }
        after_directory = os.fstat(directory_fd)
        try:
            after_path = os.stat(raw_absolute, follow_symlinks=False)
        except OSError as exc:
            raise AdapterProtocolError(
                "raw_artifact_changed", "raw artifact directory path changed while parsing"
            ) from exc
        if (
            _identity(after_directory) != _identity(before_directory)
            or _identity(after_path) != _identity(before_directory)
        ):
            raise AdapterProtocolError(
                "raw_artifact_changed", "raw artifact directory changed while parsing"
            )
    except AdapterProtocolError:
        raise
    except OSError as exc:
        raise AdapterProtocolError(
            "raw_artifact_invalid", "raw artifact directory could not be parsed"
        ) from exc
    finally:
        if directory_fd >= 0:
            try:
                os.close(directory_fd)
            except OSError:
                pass
    return content_by_role
