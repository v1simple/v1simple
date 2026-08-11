#!/usr/bin/env python3
"""Admit the fixed-profile camera before a live bench window or smoke test."""

from __future__ import annotations

import argparse
import json
import os
import tempfile
import time
from pathlib import Path
from typing import Any, Callable

from bench_identity import current_grader_fingerprint
from camera_artifacts import CameraArtifactConflict, sha256_file
from camera_capture import CALIBRATION_VIDEO_TIME_S, CameraCapture, utc_now
from camera_contract import EXPECTED_CAMERA_NAME, EXPECTED_CAMERA_PROFILE
from camera_grade import (
    REGISTRATION_HEIGHT,
    REGISTRATION_WIDTH,
    CameraRegistrationError,
    calibrate_display_crop,
)


PREFLIGHT_SCHEMA_VERSION = 2
PREFLIGHT_NAME = "camera_preflight.json"
SMOKE_NAME = "camera_smoke.json"


def _publish_immutable_json(path: Path, payload: dict[str, Any]) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        existing = json.loads(path.read_text(encoding="utf-8"))
        if existing == payload:
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
            existing = json.loads(path.read_text(encoding="utf-8"))
            if existing == payload:
                return False
            raise CameraArtifactConflict(f"immutable camera artifact already differs: {path}")
        return True
    finally:
        temporary.unlink(missing_ok=True)


def _source_still(camera: CameraCapture) -> dict[str, Any]:
    source = {"name": camera.preflight_path.name, "sha256": "", "size_bytes": 0}
    if camera.preflight_path.is_file():
        source.update(
            {
                "sha256": sha256_file(camera.preflight_path),
                "size_bytes": camera.preflight_path.stat().st_size,
            }
        )
    return source


def _owned_artifact(path: Path, directory: Path) -> dict[str, Any]:
    resolved = path.resolve()
    try:
        relative = resolved.relative_to(directory.resolve())
    except ValueError as exc:
        raise RuntimeError(f"smoke artifact escapes its output directory: {path.name}") from exc
    if len(relative.parts) != 1 or relative.name in {"", ".", ".."}:
        raise RuntimeError(f"smoke artifact path is not a safe basename: {relative}")
    if not resolved.is_file() or resolved.stat().st_size <= 0:
        raise RuntimeError(f"smoke artifact is missing or empty: {relative}")
    return {
        "path": relative.as_posix(),
        "size_bytes": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
    }


def _base_payload(camera: CameraCapture) -> dict[str, Any]:
    return {
        "schema_version": PREFLIGHT_SCHEMA_VERSION,
        "kind": "bench_camera_preflight",
        "timestamp_utc": utc_now(),
        "camera": {
            "name": camera.camera_name,
            "device_index": camera.camera_device_index,
            "profile": camera.profile(),
            "exposure_time_abs": camera.profile()["video_exposure_time_abs"],
        },
        "source_still": _source_still(camera),
    }


def _failure_payload(camera: CameraCapture, diagnostic: dict[str, Any]) -> dict[str, Any]:
    return {
        **_base_payload(camera),
        "result": "INCONCLUSIVE",
        "registration": {},
        "diagnostics": [diagnostic],
    }


def _profile_diagnostic(camera: CameraCapture) -> dict[str, Any] | None:
    actual_profile = camera.profile()
    mismatched_fields = {
        field: {"measured": actual_profile.get(field), "expected": expected}
        for field, expected in EXPECTED_CAMERA_PROFILE.items()
        if actual_profile.get(field) != expected
    }
    if camera.camera_name == EXPECTED_CAMERA_NAME and not mismatched_fields:
        return None
    return {
        "code": "camera_profile_mismatch",
        "message": "configured camera does not match the fixed capture profile",
        "measured": {
            "camera_name": camera.camera_name,
            "profile": actual_profile,
            "mismatched_fields": mismatched_fields,
        },
        "thresholds": {
            "expected_camera_name": EXPECTED_CAMERA_NAME,
            "expected_profile": EXPECTED_CAMERA_PROFILE,
        },
    }


def run_camera_preflight(camera: CameraCapture) -> dict[str, Any]:
    """Start camera capture, register its session still, and leave it running on PASS."""
    profile_diagnostic = _profile_diagnostic(camera)
    if profile_diagnostic is not None:
        payload = _failure_payload(camera, profile_diagnostic)
        _publish_immutable_json(camera.preflight_result_path, payload)
        return payload
    if not camera.start():
        payload = _failure_payload(
            camera,
            {
                "code": "capture_start_failed",
                "message": "camera capture could not start with the fixed profile",
                "measured": {"started": False, "errors": list(camera.errors)},
                "thresholds": {"started_required": True},
            },
        )
        _publish_immutable_json(camera.preflight_result_path, payload)
        return payload

    try:
        offset_x, offset_y, registration = calibrate_display_crop(
            camera.preflight_path,
            camera.ffmpeg,
        )
    except CameraRegistrationError as exc:
        diagnostic = dict(exc.diagnostic)
    except (OSError, RuntimeError, ValueError) as exc:
        diagnostic = {
            "code": "preflight_decode_failed",
            "message": str(exc),
            "measured": {
                "source_size_bytes": (
                    camera.preflight_path.stat().st_size if camera.preflight_path.is_file() else 0
                )
            },
            "thresholds": {
                "decoded_rgb_bytes": REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3,
            },
        }
    else:
        payload = {
            **_base_payload(camera),
            "result": "PASS",
            "registration": {
                **registration,
            },
            "diagnostics": [],
        }
        try:
            _publish_immutable_json(camera.preflight_result_path, payload)
        except Exception:
            camera.abort("preflight_artifact_publish_failed")
            raise
        return payload

    payload = _failure_payload(camera, diagnostic)
    try:
        _publish_immutable_json(camera.preflight_result_path, payload)
    finally:
        camera.abort(str(diagnostic["code"]))
    return payload


def run_camera_smoke(
    out_dir: Path,
    *,
    camera_factory: Callable[[Path, int], CameraCapture] = CameraCapture,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[dict[str, Any], int]:
    """Exercise camera start, recorder handoff, registration, and clean stop."""
    camera = camera_factory(out_dir, 1)
    preflight = run_camera_preflight(camera)
    if preflight.get("result") != "PASS":
        return preflight, 3
    sleep(CALIBRATION_VIDEO_TIME_S + 0.5)
    capture = camera.stop(collection_completed=False)
    passed = capture.get("result") == "CAPTURED"
    artifacts: dict[str, Any] = {}
    ownership_error = ""
    if passed:
        try:
            artifacts = {
                "preflight": _owned_artifact(camera.preflight_result_path, out_dir),
                "session_start_still": _owned_artifact(camera.preflight_path, out_dir),
                "video": _owned_artifact(camera.video_path, out_dir),
                "camera_result": _owned_artifact(camera.result_path, out_dir),
            }
        except RuntimeError as exc:
            passed = False
            ownership_error = str(exc)
    payload = {
        "schema_version": 2,
        "kind": "bench_camera_smoke",
        "timestamp_utc": utc_now(),
        "result": "PASS" if passed else "INCONCLUSIVE",
        "grader_fingerprint": current_grader_fingerprint(),
        "camera": {
            "name": camera.camera_name,
            "device_index": camera.camera_device_index,
            "profile": camera.profile(),
        },
        "artifacts": artifacts,
        "preflight": {
            "path": artifacts.get("preflight", {}).get("path", camera.preflight_result_path.name),
            "sha256": artifacts.get("preflight", {}).get(
                "sha256",
                sha256_file(camera.preflight_result_path),
            ),
        },
        "capture_result": capture.get("result"),
        "diagnostics": []
        if passed
        else [
            {
                "code": "smoke_artifact_ownership_failed"
                if ownership_error
                else "recorder_handoff_failed",
                "message": ownership_error
                or "; ".join(str(item) for item in capture.get("errors") or [])
                or "camera recorder handoff could not be verified",
            }
        ],
    }
    _publish_immutable_json(out_dir / SMOKE_NAME, payload)
    return payload, 0 if passed else 3


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", required=True, help="New directory for short camera smoke evidence")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    payload, returncode = run_camera_smoke(Path(args.out_dir).resolve())
    diagnostic = payload.get("diagnostics") or []
    suffix = f": {diagnostic[0].get('message') or diagnostic[0].get('code')}" if diagnostic else ""
    print(f"camera smoke {payload['result']}{suffix}")
    return returncode


if __name__ == "__main__":
    raise SystemExit(main())
