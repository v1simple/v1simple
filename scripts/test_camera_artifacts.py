#!/usr/bin/env python3
"""Focused regressions for immutable ownership of raw camera evidence."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from camera_artifacts import (  # noqa: E402
    CameraArtifactConflict,
    CameraArtifactError,
    CameraArtifactIncompatible,
    build_capture_manifest,
    camera_result_view,
    load_capture_manifest,
    publish_capture_manifest,
    resolve_manifest_artifact,
    validate_capture_manifest,
    verify_capture_files,
)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def fixture(root: Path, *, suite: str = "replay") -> tuple[Path, dict, dict]:
    camera_dir = root / "camera"
    camera_dir.mkdir(parents=True)
    names = {
        "video": "evidence_exp50.mov",
        "frame_timing": "frame_timing.ndjson",
        "video_timing_verification": "video_timing_verification.json",
        "session_start_still": "session_start.jpg",
        "bright_still": "bright.jpg",
        "dim_still": "dim.jpg",
        "preflight_frame_timing": "preflight_frame_timing.ndjson",
    }
    for index, name in enumerate(names.values(), start=1):
        (camera_dir / name).write_bytes(bytes([index]) * (20 + index))
    (camera_dir / "camera_preflight.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "kind": "bench_camera_preflight",
                "result": "PASS",
                "registration": {"result": "PASS", "display_crop_x": 0.1},
            }
        )
        + "\n",
        encoding="utf-8",
    )
    result = {
        "schema_version": 1,
        "kind": "bench_camera_evidence",
        "result": "CAPTURED",
        "timestamp_utc": "2026-08-23T00:00:00Z",
        "camera_name": "Global Shutter Camera",
        "camera_device_index": 0,
        "profile": {"focus_abs": 306, "video_exposure_time_abs": 50},
        "expected_duration_seconds": 300,
        "video_duration_seconds": 306.5,
        "video_timing_verification_result": {"status": "verified", "frame_count": 1000},
        "profile_validation": {"result": "PASS"},
        "errors": [],
        **names,
    }
    manifest = build_capture_manifest(
        camera_dir=camera_dir,
        camera_result=result,
        suite=suite,
    )
    return camera_dir, result, manifest


def test_capture_identity_is_stable_and_raw_byte_sensitive() -> None:
    with tempfile.TemporaryDirectory() as first_tmp, tempfile.TemporaryDirectory() as second_tmp:
        _first_dir, _first_result, first = fixture(Path(first_tmp))
        second_dir, second_result, second = fixture(Path(second_tmp))
        assert_true(first["capture_id"] == second["capture_id"], "directory changed capture identity")

        video = second_dir / second_result["video"]
        video.write_bytes(video.read_bytes() + b"changed")
        changed = build_capture_manifest(
            camera_dir=second_dir,
            camera_result=second_result,
            suite="replay",
        )
        assert_true(first["capture_id"] != changed["capture_id"], "video bytes were not owned")

        core = build_capture_manifest(
            camera_dir=second_dir,
            camera_result=second_result,
            suite="core",
        )
        assert_true(changed["capture_id"] != core["capture_id"], "suite was not owned")


def test_publication_is_immutable_and_files_remain_verifiable() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        camera_dir, _result, manifest = fixture(Path(tmp))
        path, created = publish_capture_manifest(camera_dir, manifest)
        assert_true(created, "new capture manifest was not published")
        _same_path, created = publish_capture_manifest(camera_dir, manifest)
        assert_true(not created, "identical capture manifest was rewritten")
        loaded = load_capture_manifest(path)
        assert_true(loaded["capture_id"] == manifest["capture_id"], "published identity changed")
        verify_capture_files(camera_dir, loaded)
        assert_true(
            resolve_manifest_artifact(camera_dir, loaded, "video") == camera_dir / "evidence_exp50.mov",
            "video did not resolve from owned raw artifacts",
        )
        assert_true(camera_result_view(loaded)["video"] == "evidence_exp50.mov", "capture view changed")

        conflicting = json.loads(json.dumps(manifest))
        conflicting["timestamp_utc"] = "2026-08-24T00:00:00Z"
        try:
            publish_capture_manifest(camera_dir, conflicting)
        except CameraArtifactConflict:
            pass
        else:
            raise AssertionError("conflicting immutable manifest was overwritten")

        video = camera_dir / "evidence_exp50.mov"
        video.write_bytes(video.read_bytes() + b"tampered")
        try:
            verify_capture_files(camera_dir, loaded)
        except CameraArtifactError:
            pass
        else:
            raise AssertionError("changed raw video still verified")


def test_incomplete_or_unverified_capture_is_rejected() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        camera_dir, result, manifest = fixture(Path(tmp))
        validate_capture_manifest(manifest)

        bad_digest = json.loads(json.dumps(manifest))
        bad_digest["capture_id"] = "0" * 64
        try:
            validate_capture_manifest(bad_digest)
        except CameraArtifactError:
            pass
        else:
            raise AssertionError("edited capture identity remained valid")

        result["video_timing_verification_result"] = {"status": "failed"}
        try:
            build_capture_manifest(camera_dir=camera_dir, camera_result=result, suite="replay")
        except CameraArtifactIncompatible:
            pass
        else:
            raise AssertionError("unverified video timing was admitted")

        result["video_timing_verification_result"] = {"status": "verified"}
        (camera_dir / result["frame_timing"]).unlink()
        try:
            build_capture_manifest(camera_dir=camera_dir, camera_result=result, suite="replay")
        except CameraArtifactIncompatible:
            pass
        else:
            raise AssertionError("missing raw timing sidecar was admitted")


def test_manifest_scrubs_private_camera_details() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        camera_dir, result, _manifest = fixture(Path(tmp))
        private = "/Users/private/operator-camera"
        result["camera_name"] = private
        result["errors"] = [private]
        manifest = build_capture_manifest(
            camera_dir=camera_dir,
            camera_result=result,
            suite="display",
        )
        encoded = json.dumps(manifest)
        assert_true(private not in encoded, "private camera details leaked into the manifest")
        assert_true("<redacted" in encoded, "private camera details were not visibly redacted")


def main() -> int:
    test_capture_identity_is_stable_and_raw_byte_sensitive()
    test_publication_is_immutable_and_files_remain_verifiable()
    test_incomplete_or_unverified_capture_is_rejected()
    test_manifest_scrubs_private_camera_details()
    print("camera artifact tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
