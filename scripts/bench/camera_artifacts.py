#!/usr/bin/env python3
"""Own the raw files produced by one admitted bench camera capture."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from pathlib import Path
from typing import Any, Mapping

from artifact_privacy import REDACTED_NAME, sanitize_artifact_value
from camera_contract import EXPECTED_CAMERA_NAME


CAPTURE_MANIFEST_SCHEMA_VERSION = 3
CAPTURE_MANIFEST_NAME = "capture_manifest.json"
PREFLIGHT_NAME = "camera_preflight.json"
HEX_DIGEST_LENGTH = 64
REQUIRED_CAPTURE_ARTIFACTS = (
    "video",
    "frame_timing",
    "video_timing_verification",
    "session_start_still",
    "bright_still",
    "dim_still",
    "preflight",
)


class CameraArtifactError(RuntimeError):
    """Base error for malformed or incompatible camera capture ownership."""


class CameraArtifactConflict(CameraArtifactError):
    """An immutable destination already contains different content."""


class CameraArtifactIncompatible(CameraArtifactError):
    """Raw camera evidence cannot be safely bound to this capture."""


def _canonical_bytes(value: Mapping[str, Any]) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=True,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _valid_digest(value: Any) -> bool:
    text = str(value or "")
    return len(text) == HEX_DIGEST_LENGTH and all(
        character in "0123456789abcdef" for character in text
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _resolve_named_file(directory: Path, raw_name: Any) -> Path | None:
    name = Path(str(raw_name or "")).name
    if not name or name in {".", ".."}:
        return None
    candidate = directory / name
    return candidate if candidate.is_file() else None


def _file_entry(path: Path, directory: Path, **extra: Any) -> dict[str, Any]:
    if not path.is_file() or path.stat().st_size <= 0:
        raise CameraArtifactIncompatible(
            f"camera input is missing or empty: {path.name}"
        )
    try:
        relative = path.resolve().relative_to(directory.resolve())
    except ValueError as exc:
        raise CameraArtifactIncompatible(
            f"camera input escapes its capture directory: {path.name}"
        ) from exc
    if len(relative.parts) != 1:
        raise CameraArtifactIncompatible(
            f"camera input is not a direct capture artifact: {relative}"
        )
    return {
        "path": relative.as_posix(),
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
        **extra,
    }


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CameraArtifactError(f"invalid camera artifact: {path.name}") from exc
    if not isinstance(value, dict):
        raise CameraArtifactError(f"camera artifact is not an object: {path.name}")
    return value


def build_capture_manifest(
    *,
    camera_dir: Path,
    camera_result: Mapping[str, Any],
    suite: str,
) -> dict[str, Any]:
    """Bind hashes to the playable video, raw sidecars, and admission stills."""
    if suite not in {"core", "display", "replay"}:
        raise CameraArtifactIncompatible(f"unsupported camera suite: {suite}")
    directory = camera_dir.resolve()
    safe_result = sanitize_artifact_value(dict(camera_result), run_dir=directory)
    if safe_result.get("result") != "CAPTURED":
        raise CameraArtifactIncompatible("camera capture did not complete")
    if safe_result.get("camera_name") != EXPECTED_CAMERA_NAME:
        safe_result["camera_name"] = REDACTED_NAME

    artifacts: dict[str, Any] = {}
    for field in (
        "video",
        "frame_timing",
        "video_timing_verification",
        "session_start_still",
        "bright_still",
        "dim_still",
    ):
        path = _resolve_named_file(directory, safe_result.get(field))
        if path is None:
            raise CameraArtifactIncompatible(f"camera {field} is missing")
        extra = (
            {"duration_seconds": safe_result.get("video_duration_seconds")}
            if field == "video"
            else {}
        )
        artifacts[field] = _file_entry(path, directory, **extra)

    optional_timing = _resolve_named_file(
        directory,
        safe_result.get("preflight_frame_timing"),
    )
    if optional_timing is not None:
        artifacts["preflight_frame_timing"] = _file_entry(
            optional_timing,
            directory,
        )

    preflight_path = directory / PREFLIGHT_NAME
    preflight = _read_json(preflight_path)
    if (
        preflight.get("kind") != "bench_camera_preflight"
        or preflight.get("result") != "PASS"
    ):
        raise CameraArtifactIncompatible(
            "camera capture has no successful preflight"
        )
    artifacts["preflight"] = _file_entry(preflight_path, directory)

    timing = safe_result.get("video_timing_verification_result")
    if not isinstance(timing, Mapping) or timing.get("status") != "verified":
        raise CameraArtifactIncompatible(
            "camera video timing verification is not verified"
        )

    identity = {
        "schema_version": 1,
        "suite": suite,
        "camera": {
            "name": safe_result.get("camera_name"),
            "device_index": safe_result.get("camera_device_index"),
            "profile": (
                safe_result.get("profile")
                if isinstance(safe_result.get("profile"), dict)
                else {}
            ),
        },
        "expected_duration_seconds": safe_result.get(
            "expected_duration_seconds"
        ),
        "artifacts": artifacts,
    }
    capture_id = hashlib.sha256(_canonical_bytes(identity)).hexdigest()
    return {
        "schema_version": CAPTURE_MANIFEST_SCHEMA_VERSION,
        "kind": "bench_camera_capture",
        "capture_id": capture_id,
        "result": "CAPTURED",
        "timestamp_utc": safe_result.get("timestamp_utc"),
        "identity": identity,
        "capture": {
            "result": "CAPTURED",
            "video": str(safe_result.get("video") or ""),
            "video_duration_seconds": safe_result.get("video_duration_seconds"),
            "video_probe": safe_result.get("video_probe") or {},
            "frame_timing": str(safe_result.get("frame_timing") or ""),
            "preflight_frame_timing": str(
                safe_result.get("preflight_frame_timing") or ""
            ),
            "video_timing_verification": str(
                safe_result.get("video_timing_verification") or ""
            ),
            "video_timing_verification_result": dict(timing),
            "session_start_still": str(
                safe_result.get("session_start_still") or ""
            ),
            "bright_still": str(safe_result.get("bright_still") or ""),
            "dim_still": str(safe_result.get("dim_still") or ""),
            "camera_name": safe_result.get("camera_name"),
            "camera_device_index": safe_result.get("camera_device_index"),
            "profile": identity["camera"]["profile"],
            "expected_duration_seconds": safe_result.get(
                "expected_duration_seconds"
            ),
            "profile_validation": safe_result.get("profile_validation") or {},
            "errors": list(safe_result.get("errors") or []),
        },
        "preflight": {
            "result": "PASS",
            "artifact": PREFLIGHT_NAME,
            "sha256": artifacts["preflight"]["sha256"],
            "registration": preflight.get("registration") or {},
        },
        "errors": list(safe_result.get("errors") or []),
    }


def validate_capture_manifest(manifest: Mapping[str, Any]) -> None:
    if (
        manifest.get("schema_version") != CAPTURE_MANIFEST_SCHEMA_VERSION
        or manifest.get("kind") != "bench_camera_capture"
        or manifest.get("result") != "CAPTURED"
    ):
        raise CameraArtifactError("invalid camera capture manifest schema")
    identity = manifest.get("identity")
    if not isinstance(identity, Mapping):
        raise CameraArtifactError("camera capture identity is missing")
    suite = identity.get("suite")
    if suite not in {"core", "display", "replay"}:
        raise CameraArtifactError("camera capture suite is invalid")
    artifacts = identity.get("artifacts")
    if not isinstance(artifacts, Mapping):
        raise CameraArtifactError("camera capture artifacts are missing")
    if any(name not in artifacts for name in REQUIRED_CAPTURE_ARTIFACTS):
        raise CameraArtifactError("camera capture is missing required raw artifacts")
    for name, entry in artifacts.items():
        if not isinstance(name, str) or not isinstance(entry, Mapping):
            raise CameraArtifactError("camera capture artifact entry is invalid")
        path = Path(str(entry.get("path") or ""))
        size = entry.get("size_bytes")
        if (
            path.name != str(entry.get("path") or "")
            or path.name in {"", ".", ".."}
            or not _valid_digest(entry.get("sha256"))
            or not isinstance(size, int)
            or isinstance(size, bool)
            or size <= 0
        ):
            raise CameraArtifactError(
                f"camera capture artifact ownership is invalid: {name}"
            )
    expected_capture_id = hashlib.sha256(
        _canonical_bytes(dict(identity))
    ).hexdigest()
    if manifest.get("capture_id") != expected_capture_id:
        raise CameraArtifactError("camera capture identity digest does not match")


def load_capture_manifest(path: Path) -> dict[str, Any]:
    manifest = _read_json(path)
    validate_capture_manifest(manifest)
    return manifest


def _publish_exclusive_json(
    path: Path,
    payload: Mapping[str, Any],
) -> bool:
    safe_payload = sanitize_artifact_value(dict(payload), run_dir=path.parent)
    encoded = json.dumps(safe_payload, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if path.read_text(encoding="utf-8") == encoded:
            return False
        raise CameraArtifactConflict(
            f"immutable camera artifact already differs: {path.name}"
        )
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.",
        dir=path.parent,
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write(encoded)
            handle.flush()
            os.fsync(handle.fileno())
        try:
            os.link(temporary, path)
        except FileExistsError:
            if path.read_text(encoding="utf-8") == encoded:
                return False
            raise CameraArtifactConflict(
                f"immutable camera artifact already differs: {path.name}"
            )
        return True
    finally:
        temporary.unlink(missing_ok=True)


def publish_capture_manifest(
    camera_dir: Path,
    manifest: Mapping[str, Any],
) -> tuple[Path, bool]:
    validate_capture_manifest(manifest)
    path = camera_dir / CAPTURE_MANIFEST_NAME
    return path, _publish_exclusive_json(path, manifest)


def camera_result_view(manifest: Mapping[str, Any]) -> dict[str, Any]:
    validate_capture_manifest(manifest)
    capture = manifest.get("capture")
    if not isinstance(capture, Mapping):
        raise CameraArtifactError("camera capture result is missing")
    return dict(capture)


def resolve_manifest_artifact(
    camera_dir: Path,
    manifest: Mapping[str, Any],
    name: str,
) -> Path | None:
    validate_capture_manifest(manifest)
    identity = manifest["identity"]
    artifacts = identity["artifacts"]
    entry = artifacts.get(name)
    if not isinstance(entry, Mapping):
        return None
    path = camera_dir / str(entry.get("path") or "")
    return path if path.is_file() else None


def verify_capture_files(
    camera_dir: Path,
    manifest: Mapping[str, Any],
) -> None:
    validate_capture_manifest(manifest)
    artifacts = manifest["identity"]["artifacts"]
    for name, entry in artifacts.items():
        path = camera_dir / str(entry["path"])
        if not path.is_file() or path.stat().st_size != entry["size_bytes"]:
            raise CameraArtifactError(
                f"camera capture artifact changed or disappeared: {name}"
            )
        if sha256_file(path) != entry["sha256"]:
            raise CameraArtifactError(
                f"camera capture artifact hash changed: {name}"
            )
