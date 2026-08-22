#!/usr/bin/env python3
"""Immutable ownership helpers for bench camera captures and grades."""

from __future__ import annotations

import hashlib
import json
import math
import os
import tempfile
from pathlib import Path
from typing import Any, Mapping

from artifact_privacy import REDACTED_NAME, sanitize_artifact_value
from bench_identity import canonical_bytes
from camera_contract import EXPECTED_CAMERA_NAME, camera_evidence_contract


CAPTURE_MANIFEST_SCHEMA_VERSION = 2
GRADE_SCHEMA_VERSION = 4
CAPTURE_MANIFEST_NAME = "capture_manifest.json"
PREFLIGHT_NAME = "camera_preflight.json"
GRADES_DIRECTORY_NAME = "grades"
HEX_DIGEST_LENGTH = 64


class CameraArtifactError(RuntimeError):
    """Base error for malformed or incompatible camera artifacts."""


class CameraArtifactConflict(CameraArtifactError):
    """An immutable destination already contains different content."""


class CameraArtifactIncompatible(CameraArtifactError):
    """Legacy evidence cannot be safely bound to its required inputs."""


def _valid_digest(value: Any) -> bool:
    text = str(value or "")
    return len(text) == HEX_DIGEST_LENGTH and all(character in "0123456789abcdef" for character in text)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _file_entry(path: Path, relative_to: Path, **extra: Any) -> dict[str, Any]:
    if not path.is_file() or path.stat().st_size == 0:
        raise CameraArtifactIncompatible(f"camera input is missing or empty: {path.name}")
    try:
        relative = path.resolve().relative_to(relative_to.resolve()).as_posix()
    except ValueError:
        relative = f"../{path.name}"
    return {
        "path": relative,
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
        **extra,
    }


def _resolve_named_file(directory: Path, raw_name: Any) -> Path | None:
    name = Path(str(raw_name or "")).name
    if not name or name in {".", ".."}:
        return None
    candidate = directory / name
    return candidate if candidate.is_file() else None


def resolve_encounter_csv(replay_dir: Path, window_result: Mapping[str, Any] | None) -> Path | None:
    raw = window_result.get("encounter_csv_path") if window_result else ""
    candidate = _resolve_named_file(replay_dir, raw)
    if candidate is not None:
        return candidate
    matches = sorted(replay_dir.glob("encounters_*.csv"))
    return matches[0] if len(matches) == 1 else None


def _legacy_timing_anchor(camera_result: Mapping[str, Any]) -> dict[str, Any] | None:
    anchor = camera_result.get("timing_anchor")
    if isinstance(anchor, dict):
        value = anchor.get("video_seconds")
        if isinstance(value, (int, float)) and math.isfinite(value):
            return dict(anchor)
    value = camera_result.get("emulator_start_video_seconds")
    if isinstance(value, (int, float)) and math.isfinite(value):
        return {
            "kind": "emulator_process_start_legacy",
            "video_seconds": float(value),
            "clock_mapping": "host_monotonic_delta_to_video_pts",
        }
    return None


def build_capture_manifest(
    *,
    camera_dir: Path,
    camera_result: Mapping[str, Any],
    suite: str,
    product_fingerprint: str,
    scenario_fingerprint: str,
    encounter_csv_path: Path | None,
    timing_anchor: Mapping[str, Any] | None,
    traceability: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    camera_dir = camera_dir.resolve()
    safe_camera_result = sanitize_artifact_value(dict(camera_result), run_dir=camera_dir)
    if safe_camera_result.get("camera_name") != EXPECTED_CAMERA_NAME:
        safe_camera_result["camera_name"] = REDACTED_NAME
    safe_timing_anchor = sanitize_artifact_value(
        dict(timing_anchor) if isinstance(timing_anchor, Mapping) else None,
        run_dir=camera_dir,
    )
    safe_traceability = sanitize_artifact_value(dict(traceability or {}), run_dir=camera_dir)
    artifacts: dict[str, Any] = {}
    for field in ("video", "session_start_still", "bright_still", "dim_still"):
        path = _resolve_named_file(camera_dir, safe_camera_result.get(field))
        if path is None:
            raise CameraArtifactIncompatible(f"camera {field} is missing")
        extra = {}
        if field == "video":
            extra["duration_seconds"] = safe_camera_result.get("video_duration_seconds")
        artifacts[field] = _file_entry(path, camera_dir, **extra)
    for field in ("frame_timing", "preflight_frame_timing", "video_timing_verification"):
        raw_name = safe_camera_result.get(field)
        if not raw_name:
            continue
        path = _resolve_named_file(camera_dir, raw_name)
        if path is None:
            raise CameraArtifactIncompatible(f"camera {field} is missing")
        artifacts[field] = _file_entry(path, camera_dir)
    if encounter_csv_path is not None:
        artifacts["encounter_csv"] = _file_entry(encounter_csv_path, camera_dir)
    preflight_summary: dict[str, Any] = {}
    preflight_path = camera_dir / PREFLIGHT_NAME
    if preflight_path.is_file():
        preflight = _read_json(preflight_path)
        if preflight.get("kind") != "bench_camera_preflight" or preflight.get("result") != "PASS":
            raise CameraArtifactIncompatible("camera capture has no successful preflight")
        artifacts["preflight"] = _file_entry(preflight_path, camera_dir)
        preflight_summary = {
            "result": "PASS",
            "artifact": PREFLIGHT_NAME,
            "sha256": artifacts["preflight"]["sha256"],
            "registration": preflight.get("registration") or {},
        }

    normalized_anchor = safe_timing_anchor
    identity = {
        "schema_version": 1,
        "suite": suite,
        "product_fingerprint": str(product_fingerprint or ""),
        "scenario_fingerprint": str(scenario_fingerprint or ""),
        "camera": {
            "name": safe_camera_result.get("camera_name"),
            "device_index": safe_camera_result.get("camera_device_index"),
            "profile": (
                safe_camera_result.get("profile")
                if isinstance(safe_camera_result.get("profile"), dict)
                else {}
            ),
        },
        "expected_duration_seconds": safe_camera_result.get("expected_duration_seconds"),
        "timing_anchor": normalized_anchor,
        "artifacts": artifacts,
    }
    capture_id = hashlib.sha256(canonical_bytes(identity)).hexdigest()
    capture = {
        "result": safe_camera_result.get("result"),
        "video": str(safe_camera_result.get("video") or ""),
        "video_duration_seconds": safe_camera_result.get("video_duration_seconds"),
        "session_start_still": str(safe_camera_result.get("session_start_still") or ""),
        "bright_still": str(safe_camera_result.get("bright_still") or ""),
        "dim_still": str(safe_camera_result.get("dim_still") or ""),
        "frame_timing": str(safe_camera_result.get("frame_timing") or ""),
        "preflight_frame_timing": str(safe_camera_result.get("preflight_frame_timing") or ""),
        "video_timing_verification": str(safe_camera_result.get("video_timing_verification") or ""),
        "video_timing_verification_result": (
            safe_camera_result.get("video_timing_verification_result")
            if isinstance(safe_camera_result.get("video_timing_verification_result"), dict)
            else {}
        ),
        "camera_name": safe_camera_result.get("camera_name"),
        "camera_device_index": safe_camera_result.get("camera_device_index"),
        "profile": identity["camera"]["profile"],
        "expected_duration_seconds": safe_camera_result.get("expected_duration_seconds"),
        "profile_validation": safe_camera_result.get("profile_validation") or {},
        "errors": list(safe_camera_result.get("errors") or []),
    }
    return {
        "schema_version": CAPTURE_MANIFEST_SCHEMA_VERSION,
        "kind": "bench_camera_capture",
        "capture_id": capture_id,
        "result": safe_camera_result.get("result"),
        "timestamp_utc": safe_camera_result.get("timestamp_utc"),
        "identity": identity,
        "capture": capture,
        "timing_anchor": normalized_anchor,
        "profile_validation": safe_camera_result.get("profile_validation") or {},
        "preflight": preflight_summary,
        "evidence_contract": camera_evidence_contract(suite),
        "traceability": safe_traceability,
        "errors": list(safe_camera_result.get("errors") or []),
    }


def _read_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CameraArtifactError(f"invalid camera artifact {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise CameraArtifactError(f"camera artifact is not an object: {path}")
    return payload


def validate_capture_manifest(manifest: Mapping[str, Any]) -> None:
    if (
        manifest.get("schema_version") != CAPTURE_MANIFEST_SCHEMA_VERSION
        or manifest.get("kind") != "bench_camera_capture"
    ):
        raise CameraArtifactError("invalid camera capture manifest schema")
    identity = manifest.get("identity")
    if not isinstance(identity, dict):
        raise CameraArtifactError("camera capture manifest has no identity")
    if identity.get("schema_version") != 1 or identity.get("suite") not in {"core", "display", "replay"}:
        raise CameraArtifactError("camera capture manifest has an invalid identity contract")
    artifacts = identity.get("artifacts")
    if not isinstance(artifacts, dict):
        raise CameraArtifactError("camera capture manifest has no artifact ownership")
    required = {"video", "session_start_still", "bright_still", "dim_still"}
    if identity.get("suite") == "replay":
        required.add("encounter_csv")
    for name in required:
        entry = artifacts.get(name)
        if not isinstance(entry, dict) or not _valid_digest(entry.get("sha256")):
            raise CameraArtifactError(f"camera capture manifest has invalid {name} ownership")
        path = str(entry.get("path") or "")
        if not path or Path(path).name in {"", ".", ".."}:
            raise CameraArtifactError(f"camera capture manifest has invalid {name} path")
    for name in ("frame_timing", "preflight_frame_timing", "video_timing_verification"):
        entry = artifacts.get(name)
        if entry is None:
            continue
        if not isinstance(entry, dict) or not _valid_digest(entry.get("sha256")):
            raise CameraArtifactError(f"camera capture manifest has invalid {name} ownership")
        path = str(entry.get("path") or "")
        if not path or Path(path).name in {"", ".", ".."}:
            raise CameraArtifactError(f"camera capture manifest has invalid {name} path")
    preflight = manifest.get("preflight")
    if preflight:
        entry = artifacts.get("preflight")
        if (
            not isinstance(preflight, dict)
            or preflight.get("result") != "PASS"
            or not isinstance(entry, dict)
            or preflight.get("sha256") != entry.get("sha256")
        ):
            raise CameraArtifactError("camera capture manifest has invalid preflight ownership")
    expected = hashlib.sha256(canonical_bytes(identity)).hexdigest()
    if manifest.get("capture_id") != expected:
        raise CameraArtifactError("camera capture_id does not match its immutable identity")


def load_capture_manifest(path: Path) -> dict[str, Any]:
    manifest = _read_json(path)
    validate_capture_manifest(manifest)
    return manifest


def _publish_exclusive_json(path: Path, payload: Mapping[str, Any]) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        existing = _read_json(path)
        if existing == dict(payload):
            return False
        raise CameraArtifactConflict(f"immutable camera artifact already differs: {path}")
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        try:
            os.link(temporary, path)
        except FileExistsError:
            existing = _read_json(path)
            if existing == dict(payload):
                return False
            raise CameraArtifactConflict(f"immutable camera artifact already differs: {path}")
        return True
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def publish_capture_manifest(camera_dir: Path, manifest: Mapping[str, Any]) -> tuple[Path, bool]:
    safe_manifest = sanitize_artifact_value(dict(manifest), run_dir=camera_dir)
    validate_capture_manifest(safe_manifest)
    path = camera_dir / CAPTURE_MANIFEST_NAME
    return path, _publish_exclusive_json(path, safe_manifest)


def capture_input_hashes(manifest: Mapping[str, Any]) -> dict[str, str]:
    identity = manifest.get("identity") if isinstance(manifest.get("identity"), dict) else {}
    artifacts = identity.get("artifacts") if isinstance(identity.get("artifacts"), dict) else {}
    result: dict[str, str] = {}
    for name, entry in artifacts.items():
        if isinstance(entry, dict) and _valid_digest(entry.get("sha256")):
            result[str(name)] = str(entry["sha256"])
    return result


def camera_result_view(manifest: Mapping[str, Any]) -> dict[str, Any]:
    capture = manifest.get("capture")
    if not isinstance(capture, dict):
        raise CameraArtifactError("camera capture manifest has no capture view")
    result = dict(capture)
    result["timing_anchor"] = manifest.get("timing_anchor") or {}
    result["evidence_contract"] = manifest.get("evidence_contract") or {}
    result["preflight"] = manifest.get("preflight") or {}
    return result


def grade_path(camera_dir: Path, grader_fingerprint: str) -> Path:
    if not _valid_digest(grader_fingerprint):
        raise CameraArtifactError("invalid grader fingerprint")
    return camera_dir / GRADES_DIRECTORY_NAME / f"{grader_fingerprint}.json"


def validate_grade_ownership(
    grade: Mapping[str, Any],
    manifest: Mapping[str, Any],
    grader_fingerprint: str,
) -> None:
    if grade.get("schema_version") != GRADE_SCHEMA_VERSION or grade.get("kind") != "bench_camera_grade":
        raise CameraArtifactError("invalid strict camera grade schema")
    if grade.get("capture_id") != manifest.get("capture_id"):
        raise CameraArtifactError("camera grade capture ownership does not match")
    if grade.get("grader_fingerprint") != grader_fingerprint:
        raise CameraArtifactError("camera grade grader ownership does not match")
    identity = manifest.get("identity") if isinstance(manifest.get("identity"), dict) else {}
    if grade.get("suite") != identity.get("suite"):
        raise CameraArtifactError("camera grade suite ownership does not match")
    if grade.get("input_hashes") != capture_input_hashes(manifest):
        raise CameraArtifactError("camera grade input hashes do not match capture ownership")


def load_owned_grade(
    camera_dir: Path,
    manifest: Mapping[str, Any],
    grader_fingerprint: str,
) -> dict[str, Any] | None:
    path = grade_path(camera_dir, grader_fingerprint)
    if not path.exists():
        return None
    grade = _read_json(path)
    validate_grade_ownership(grade, manifest, grader_fingerprint)
    return grade


def validate_resumable_grade(grade: Mapping[str, Any]) -> None:
    """Require a complete schema-3 verdict before treating a grade as finished."""
    result = grade.get("result")
    if result not in {"PASS", "FAIL", "INCONCLUSIVE"}:
        raise CameraArtifactError("invalid current camera grade result")
    confidence = grade.get("confidence")
    if not isinstance(confidence, dict) or confidence.get("result") not in {
        "PASS",
        "INCONCLUSIVE",
    }:
        raise CameraArtifactError("invalid current camera grade confidence")
    if not isinstance(confidence.get("gates"), dict):
        raise CameraArtifactError("invalid current camera grade confidence gates")
    if not isinstance(grade.get("checks"), dict):
        raise CameraArtifactError("invalid current camera grade checks")
    diagnostics = grade.get("diagnostics")
    errors = grade.get("errors")
    if not isinstance(diagnostics, list) or not isinstance(errors, list):
        raise CameraArtifactError("invalid current camera grade diagnostics")
    if result in {"PASS", "FAIL"}:
        if confidence.get("result") != "PASS" or diagnostics or errors:
            raise CameraArtifactError("confident current camera grade has inconsistent status")
    elif confidence.get("result") != "INCONCLUSIVE":
        raise CameraArtifactError("inconclusive current camera grade has inconsistent status")


def publish_grade(
    camera_dir: Path,
    manifest: Mapping[str, Any],
    grader_fingerprint: str,
    grade: Mapping[str, Any],
) -> tuple[Path, bool]:
    safe_grade = sanitize_artifact_value(dict(grade), run_dir=camera_dir)
    validate_grade_ownership(safe_grade, manifest, grader_fingerprint)
    path = grade_path(camera_dir, grader_fingerprint)
    return path, _publish_exclusive_json(path, safe_grade)


def publish_immutable_json(path: Path, payload: Mapping[str, Any]) -> bool:
    """Publish an immutable JSON report with the same exclusive contract as evidence."""
    safe_payload = sanitize_artifact_value(dict(payload), run_dir=path.parent)
    return _publish_exclusive_json(path, safe_payload)


def load_or_adapt_capture(
    *,
    camera_dir: Path,
    replay_dir: Path,
    window_result: Mapping[str, Any] | None,
    identity_manifest: Mapping[str, Any] | None,
    publish_adapted: bool = False,
) -> dict[str, Any]:
    manifest_path = camera_dir / CAPTURE_MANIFEST_NAME
    if manifest_path.is_file():
        return load_capture_manifest(manifest_path)
    camera_result_path = camera_dir / "camera_result.json"
    camera_result = _read_json(camera_result_path)
    encounter = resolve_encounter_csv(replay_dir, window_result)
    product = str(
        (identity_manifest or {}).get("product_fingerprint")
        or (window_result or {}).get("product_fingerprint")
        or ""
    )
    scenario = str(
        (identity_manifest or {}).get("scenario_fingerprint")
        or (window_result or {}).get("scenario_fingerprint")
        or ""
    )
    traceability = (identity_manifest or {}).get("traceability")
    if not isinstance(traceability, dict):
        traceability = {
            "repository_sha": (window_result or {}).get("git_sha", ""),
            "repository_ref": (window_result or {}).get("git_ref", ""),
            "worktree_clean": (window_result or {}).get("git_worktree_clean", False),
        }
    manifest = build_capture_manifest(
        camera_dir=camera_dir,
        camera_result=camera_result,
        suite="replay",
        product_fingerprint=product,
        scenario_fingerprint=scenario,
        encounter_csv_path=encounter,
        timing_anchor=_legacy_timing_anchor(camera_result),
        traceability=traceability,
    )
    if publish_adapted:
        publish_capture_manifest(camera_dir, manifest)
    return manifest


def resolve_manifest_artifact(camera_dir: Path, manifest: Mapping[str, Any], name: str) -> Path | None:
    identity = manifest.get("identity") if isinstance(manifest.get("identity"), dict) else {}
    artifacts = identity.get("artifacts") if isinstance(identity.get("artifacts"), dict) else {}
    entry = artifacts.get(name)
    if not isinstance(entry, dict):
        return None
    path_name = Path(str(entry.get("path") or "")).name
    if not path_name:
        return None
    directory = camera_dir.parent if name == "encounter_csv" else camera_dir
    path = directory / path_name
    return path if path.is_file() else None


def verify_capture_files(camera_dir: Path, manifest: Mapping[str, Any]) -> None:
    """Verify every immutable manifest input against the bytes still on disk."""
    validate_capture_manifest(manifest)
    identity = manifest["identity"]
    artifacts = identity["artifacts"]
    for name, entry in sorted(artifacts.items()):
        if not isinstance(entry, dict):
            raise CameraArtifactError(f"camera capture manifest has invalid {name} ownership")
        path = resolve_manifest_artifact(camera_dir, manifest, str(name))
        if path is None:
            raise CameraArtifactError(f"camera capture input is missing: {name}")
        expected_size = entry.get("size_bytes")
        if not isinstance(expected_size, int) or isinstance(expected_size, bool) or expected_size < 1:
            raise CameraArtifactError(f"camera capture manifest has invalid {name} size")
        actual_size = path.stat().st_size
        if actual_size != expected_size:
            raise CameraArtifactError(
                f"camera capture input size changed: {name} ({actual_size} != {expected_size})"
            )
        if sha256_file(path) != entry.get("sha256"):
            raise CameraArtifactError(f"camera capture input hash changed: {name}")


def validate_capture_window_identity(
    manifest: Mapping[str, Any],
    *,
    suite: str,
    product_fingerprint: str,
    scenario_fingerprint: str,
) -> None:
    """Require a capture to belong to the exact product scenario being scored."""
    validate_capture_manifest(manifest)
    identity = manifest["identity"]
    expected = {
        "suite": suite,
        "product_fingerprint": product_fingerprint,
        "scenario_fingerprint": scenario_fingerprint,
    }
    for field, value in expected.items():
        actual = str(identity.get(field) or "")
        required = bool(value) if field == "suite" else _valid_digest(value)
        if not required or actual != value:
            raise CameraArtifactError(f"camera capture {field} does not match the current window")


def agreed_window_identity(
    window_result: Mapping[str, Any],
    performance_manifest: Mapping[str, Any],
) -> dict[str, str]:
    """Return product/scenario identity only when both owning artifacts agree."""
    agreed: dict[str, str] = {}
    for field in ("product_fingerprint", "scenario_fingerprint"):
        window_value = str(window_result.get(field) or "")
        manifest_value = str(performance_manifest.get(field) or "")
        if not _valid_digest(window_value) or window_value != manifest_value:
            raise CameraArtifactError(
                f"window and performance manifest {field} identities do not agree"
            )
        agreed[field] = window_value
    return agreed


def strict_grade_outcome(grade: Mapping[str, Any]) -> tuple[str, list[str]]:
    """Return the only camera result that may influence the product verdict."""
    result = str(grade.get("result") or "")
    confidence = grade.get("confidence")
    confidence_passed = isinstance(confidence, dict) and confidence.get("result") == "PASS"
    errors = [str(item) for item in grade.get("errors") or []]
    diagnostics = [
        str(item.get("message") or item.get("code") or item)
        if isinstance(item, dict)
        else str(item)
        for item in grade.get("diagnostics") or []
    ]
    if result in {"PASS", "FAIL"} and confidence_passed and not errors and not diagnostics:
        return result, []
    return "INCONCLUSIVE", [*diagnostics, *errors]


def replay_timing_anchor(
    suite: str,
    recording_started_monotonic: float | None,
    replay_started_monotonic: Any,
) -> tuple[float | None, dict[str, Any] | None]:
    """Map the replay host event to the current camera video's approximate clock."""
    if (
        suite != "replay"
        or recording_started_monotonic is None
        or not isinstance(replay_started_monotonic, (int, float))
        or not math.isfinite(replay_started_monotonic)
    ):
        return None, None
    video_seconds = round(replay_started_monotonic - recording_started_monotonic, 3)
    return video_seconds, {
        "kind": "first_emitted_replay_sample",
        "video_seconds": video_seconds,
        "clock_mapping": "host_monotonic_delta_to_video_pts",
    }
