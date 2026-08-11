#!/usr/bin/env python3
"""Focused regressions for immutable camera capture and grade ownership."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import camera_regrade as camera_regrade_module  # noqa: E402
from bench_identity import current_grader_fingerprint  # noqa: E402
from camera_artifacts import (  # noqa: E402
    CameraArtifactConflict,
    CameraArtifactError,
    build_capture_manifest,
    camera_result_view,
    capture_input_hashes,
    load_or_adapt_capture,
    load_owned_grade,
    publish_capture_manifest,
    publish_grade,
    publish_immutable_json,
    verify_capture_files,
)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def camera_fixture(replay_dir: Path) -> tuple[Path, dict, Path]:
    camera_dir = replay_dir / "camera"
    camera_dir.mkdir(parents=True)
    names = {
        "video": "evidence_exp156.mp4",
        "session_start_still": "session_start_exp156.jpg",
        "bright_still": "final_exp5.jpg",
        "dim_still": "final_exp1250.jpg",
    }
    for index, name in enumerate(names.values(), start=1):
        (camera_dir / name).write_bytes(bytes([index]) * (10 + index))
    encounter = replay_dir / "encounters_1-token.csv"
    encounter.write_text("millis,event,priority\n0,SAMPLE,1\n", encoding="utf-8")
    write_json(
        camera_dir / "camera_preflight.json",
        {
            "schema_version": 1,
            "kind": "bench_camera_preflight",
            "result": "PASS",
            "registration": {"result": "PASS"},
        },
    )
    result = {
        "schema_version": 1,
        "kind": "bench_camera_evidence",
        "result": "CAPTURED",
        "timestamp_utc": "2026-08-08T00:00:00Z",
        "camera_name": "Razer Kiyo",
        "camera_device_index": 0,
        "profile": {"focus_abs": 208, "video_exposure_time_abs": 156},
        "expected_duration_seconds": 300,
        "video_duration_seconds": 306.5,
        "profile_validation": {"result": "PASS"},
        "errors": [],
        **names,
    }
    return camera_dir, result, encounter


def manifest_fixture(root: Path, *, timing: float = 8.0, trace_sha: str = "1" * 40) -> tuple[Path, dict]:
    replay_dir = root / "run" / "replay"
    camera_dir, result, encounter = camera_fixture(replay_dir)
    manifest = build_capture_manifest(
        camera_dir=camera_dir,
        camera_result=result,
        suite="replay",
        product_fingerprint="a" * 64,
        scenario_fingerprint="b" * 64,
        encounter_csv_path=encounter,
        timing_anchor={"kind": "first_emitted_replay_sample", "video_seconds": timing},
        traceability={"repository_sha": trace_sha},
    )
    return camera_dir, manifest


def grade_fixture(manifest: dict, grader_fingerprint: str, result: str = "PASS") -> dict:
    return {
        "schema_version": 4,
        "kind": "bench_camera_grade",
        "capture_id": manifest["capture_id"],
        "grader_fingerprint": grader_fingerprint,
        "grade_id": hashlib.sha256(
            f"{manifest['capture_id']}:{grader_fingerprint}".encode("ascii")
        ).hexdigest(),
        "input_hashes": capture_input_hashes(manifest),
        "suite": "replay",
        "result": result,
        "confidence": {"result": "PASS", "gates": {}},
        "checks": {},
        "diagnostics": [],
        "errors": [],
    }


def test_capture_id_stability_and_sensitivity() -> None:
    with tempfile.TemporaryDirectory() as first_tmp, tempfile.TemporaryDirectory() as second_tmp:
        first_camera, first = manifest_fixture(Path(first_tmp), trace_sha="1" * 40)
        _second_camera, second = manifest_fixture(Path(second_tmp), trace_sha="2" * 40)
        assert_true(first["capture_id"] == second["capture_id"], "traceability or root changed capture_id")

        for relative in (
            "camera/evidence_exp156.mp4",
            "camera/session_start_exp156.jpg",
            "encounters_1-token.csv",
        ):
            change_root = Path(second_tmp) / relative.replace("/", "_")
            changed_camera, unchanged = manifest_fixture(change_root)
            changed_path = changed_camera.parent / relative
            changed_path.write_bytes(changed_path.read_bytes() + b"changed")
            rebuilt = build_capture_manifest(
                camera_dir=changed_camera,
                camera_result=unchanged["capture"],
                suite="replay",
                product_fingerprint="a" * 64,
                scenario_fingerprint="b" * 64,
                encounter_csv_path=changed_camera.parent / "encounters_1-token.csv",
                timing_anchor=unchanged["timing_anchor"],
            )
            assert_true(
                unchanged["capture_id"] != rebuilt["capture_id"],
                f"capture input change was ignored: {relative}",
            )
        _camera, changed_timing = manifest_fixture(Path(second_tmp) / "other", timing=9.0)
        assert_true(first["capture_id"] != changed_timing["capture_id"], "timing change was ignored")


def test_immutable_publication_and_conflicts() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        camera_dir, manifest = manifest_fixture(Path(tmp))
        _path, created = publish_capture_manifest(camera_dir, manifest)
        assert_true(created, "new capture manifest was not published")
        _path, created = publish_capture_manifest(camera_dir, manifest)
        assert_true(not created, "identical capture manifest was rewritten")
        conflicting = json.loads(json.dumps(manifest))
        conflicting["traceability"]["repository_sha"] = "f" * 40
        try:
            publish_capture_manifest(camera_dir, conflicting)
        except CameraArtifactConflict:
            pass
        else:
            raise AssertionError("conflicting immutable capture manifest was overwritten")

        fingerprint = current_grader_fingerprint(ROOT)
        grade = grade_fixture(manifest, fingerprint)
        _grade_path, created = publish_grade(camera_dir, manifest, fingerprint, grade)
        assert_true(created, "new grade was not published")
        _grade_path, created = publish_grade(camera_dir, manifest, fingerprint, grade)
        assert_true(not created, "identical grade was rewritten")
        conflicting_grade = {**grade, "result": "FAIL"}
        try:
            publish_grade(camera_dir, manifest, fingerprint, conflicting_grade)
        except CameraArtifactConflict:
            pass
        else:
            raise AssertionError("conflicting append-only grade was overwritten")


def test_resumable_skip_does_not_extract_frames() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        corpus = Path(tmp)
        camera_dir, manifest = manifest_fixture(corpus)
        publish_capture_manifest(camera_dir, manifest)
        fingerprint = current_grader_fingerprint(ROOT)
        publish_grade(camera_dir, manifest, fingerprint, grade_fixture(manifest, fingerprint))
        original = camera_regrade_module.grade_camera
        camera_regrade_module.grade_camera = lambda **_kwargs: (_ for _ in ()).throw(  # type: ignore[assignment]
            AssertionError("resumable skip decoded frames")
        )
        try:
            counts, returncode = camera_regrade_module.regrade_corpus(corpus)
        finally:
            camera_regrade_module.grade_camera = original
        assert_true(returncode == 0, f"valid resumable skip failed: {counts}")
        assert_true(counts["skipped"] == 1 and counts["graded"] == 0, f"capture was not skipped: {counts}")


def test_owned_bytes_are_verified_before_resume() -> None:
    for artifact, relative in (
        ("video", Path("camera/evidence_exp156.mp4")),
        ("encounter_csv", Path("encounters_1-token.csv")),
        ("preflight", Path("camera/camera_preflight.json")),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            corpus = Path(tmp)
            camera_dir, manifest = manifest_fixture(corpus)
            publish_capture_manifest(camera_dir, manifest)
            fingerprint = current_grader_fingerprint(ROOT)
            publish_grade(camera_dir, manifest, fingerprint, grade_fixture(manifest, fingerprint))
            verify_capture_files(camera_dir, manifest)
            path = camera_dir.parent / relative
            original = path.read_bytes()
            path.write_bytes(bytes([original[0] ^ 0xFF]) + original[1:])
            try:
                verify_capture_files(camera_dir, manifest)
            except CameraArtifactError as exc:
                assert_true(artifact in str(exc), f"wrong changed-input diagnostic: {exc}")
            else:
                raise AssertionError(f"changed {artifact} passed byte verification")

            original_grade = camera_regrade_module.grade_camera
            camera_regrade_module.grade_camera = lambda **_kwargs: (_ for _ in ()).throw(  # type: ignore[assignment]
                AssertionError("changed capture reached frame extraction")
            )
            try:
                counts, returncode = camera_regrade_module.regrade_corpus(corpus)
            finally:
                camera_regrade_module.grade_camera = original_grade
            assert_true(returncode == 2, f"changed capture was accepted: {counts}")
            assert_true(
                counts["incompatible"] == 1 and counts["skipped"] == 0,
                f"changed capture was resumably skipped: {counts}",
            )


def test_legacy_preservation_and_relocation() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        replay_dir = Path(tmp) / "moved" / "replay"
        camera_dir, result, _encounter = camera_fixture(replay_dir)
        result["emulator_start_video_seconds"] = 5.5
        result_path = camera_dir / "camera_result.json"
        grade_path = camera_dir / "camera_grade.json"
        write_json(result_path, result)
        write_json(grade_path, {"schema_version": 1, "kind": "bench_camera_grade", "result": "PASS"})
        before = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in (result_path, grade_path)}
        manifest = load_or_adapt_capture(
            camera_dir=camera_dir,
            replay_dir=replay_dir,
            window_result={"encounter_csv_path": "/old/machine/encounters_1-token.csv"},
            identity_manifest={},
            publish_adapted=True,
        )
        after = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in (result_path, grade_path)}
        assert_true(before == after, "legacy camera artifacts were modified")
        assert_true(
            manifest["timing_anchor"]["kind"] == "emulator_process_start_legacy",
            f"legacy timing was not adapted: {manifest}",
        )
        assert_true("encounter_csv" in manifest["identity"]["artifacts"], "relocated encounter was not found")


def test_missing_legacy_timing_abstains_before_decode() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        replay_dir = Path(tmp) / "run" / "replay"
        camera_dir, result, _encounter = camera_fixture(replay_dir)
        write_json(camera_dir / "camera_result.json", result)
        manifest = load_or_adapt_capture(
            camera_dir=camera_dir,
            replay_dir=replay_dir,
            window_result={"encounter_csv_path": "/moved/encounters_1-token.csv"},
            identity_manifest={},
        )
        original = camera_regrade_module.grade_camera.__globals__["extract_observations"]
        camera_regrade_module.grade_camera.__globals__["extract_observations"] = lambda *_args, **_kwargs: (
            (_ for _ in ()).throw(AssertionError("missing timing decoded video"))
        )
        try:
            grade = camera_regrade_module.grade_camera(
                suite="replay",
                camera_dir=camera_dir,
                camera_result=camera_result_view(manifest),
                capture_manifest=manifest,
                grader_fingerprint=current_grader_fingerprint(ROOT),
                emulator_result={"completed": True},
                encounter_csv_path=replay_dir / "encounters_1-token.csv",
                timeline_start_video_s=None,
            )
        finally:
            camera_regrade_module.grade_camera.__globals__["extract_observations"] = original
        assert_true(grade["result"] == "INCONCLUSIVE", f"missing timing did not abstain: {grade}")
        assert_true(
            grade["diagnostics"][0]["code"] == "timing_anchor_missing",
            f"missing timing diagnostic was imprecise: {grade}",
        )


def test_malformed_grade_is_a_conflict() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        corpus = Path(tmp)
        camera_dir, manifest = manifest_fixture(corpus)
        publish_capture_manifest(camera_dir, manifest)
        fingerprint = current_grader_fingerprint(ROOT)
        malformed = camera_dir / "grades" / f"{fingerprint}.json"
        malformed.parent.mkdir()
        malformed.write_text("{not json\n", encoding="utf-8")
        try:
            load_owned_grade(camera_dir, manifest, fingerprint)
        except CameraArtifactError:
            pass
        else:
            raise AssertionError("malformed grade passed ownership validation")
        counts, returncode = camera_regrade_module.regrade_corpus(corpus)
        assert_true(returncode == 2 and counts["conflict"] == 1, f"malformed grade was not a conflict: {counts}")


def test_invalid_owned_grade_cannot_complete_regrade() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        corpus = Path(tmp)
        camera_dir, manifest = manifest_fixture(corpus)
        publish_capture_manifest(camera_dir, manifest)
        fingerprint = current_grader_fingerprint(ROOT)
        invalid = grade_fixture(manifest, fingerprint, result="BROKEN")
        invalid_path = camera_dir / "grades" / f"{fingerprint}.json"
        invalid_path.parent.mkdir()
        write_json(invalid_path, invalid)

        report, returncode = camera_regrade_module.build_regrade_report(corpus)
        counts = report["counts"]
        assert_true(returncode == 2, f"invalid grade returned success: {report}")
        assert_true(report["completed"] is False, f"invalid grade completed regrade: {report}")
        assert_true(
            counts["conflict"] == 1
            and counts["skipped"] == 0
            and counts["pass"] == counts["fail"] == counts["inconclusive"] == 0,
            f"invalid grade was counted as a completed verdict: {report}",
        )
        capture = report["captures"][0]
        assert_true(capture["grade"]["status"] == "conflict", f"wrong invalid-grade status: {capture}")
        assert_true(
            capture["ownership_valid"] is True
            and capture["grade"]["ownership_valid"] is True
            and capture["grade"]["path"].endswith(f"grades/{fingerprint}.json"),
            f"owned invalid grade lost provenance: {capture}",
        )
        assert_true(
            "invalid current camera grade result" in capture["diagnostic"],
            f"invalid-grade diagnostic was imprecise: {capture}",
        )


def test_regrade_completion_report_is_owned_and_immutable() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        corpus = Path(tmp) / "corpus"
        camera_dir, manifest = manifest_fixture(corpus)
        publish_capture_manifest(camera_dir, manifest)
        fingerprint = current_grader_fingerprint(ROOT)
        publish_grade(camera_dir, manifest, fingerprint, grade_fixture(manifest, fingerprint))

        report, returncode = camera_regrade_module.build_regrade_report(corpus)
        assert_true(returncode == 0 and report["completed"] is True, f"report did not complete: {report}")
        assert_true(report["dry_run"] is False, f"real report was marked dry-run: {report}")
        assert_true(report["grader_fingerprint"] == fingerprint, f"wrong report grader: {report}")
        assert_true(
            report["counts"]["processed"] == report["counts"]["discovered"] == 1,
            f"report accounting is incomplete: {report}",
        )
        capture = report["captures"][0]
        assert_true(capture["capture_id"] == manifest["capture_id"], f"capture ID missing: {capture}")
        assert_true(
            capture["result"] == "PASS"
            and capture["confidence_result"] == "PASS"
            and capture["ownership_valid"] is True
            and capture["grade"]["status"] == "skipped",
            f"owned grade was not reported: {capture}",
        )
        assert_true(not Path(capture["capture_path"]).is_absolute(), f"absolute path leaked: {capture}")

        report_path = Path(tmp) / "report-v1.json"
        assert_true(publish_immutable_json(report_path, report), "new report was not published")
        assert_true(not publish_immutable_json(report_path, report), "identical report was rewritten")
        conflicting = json.loads(json.dumps(report))
        conflicting["completed"] = False
        try:
            publish_immutable_json(report_path, conflicting)
        except CameraArtifactConflict:
            pass
        else:
            raise AssertionError("immutable regrade report was overwritten")

        (camera_dir / "evidence_exp156.mp4").write_bytes(b"changed")
        dry_report, dry_returncode = camera_regrade_module.build_regrade_report(corpus, dry_run=True)
        assert_true(dry_returncode == 0, f"dry-run inventory failed: {dry_report}")
        assert_true(
            dry_report["completed"] is False and dry_report["counts"]["processed"] == 0,
            f"dry-run was accepted as completed: {dry_report}",
        )


def main() -> int:
    test_capture_id_stability_and_sensitivity()
    test_immutable_publication_and_conflicts()
    test_resumable_skip_does_not_extract_frames()
    test_owned_bytes_are_verified_before_resume()
    test_legacy_preservation_and_relocation()
    test_missing_legacy_timing_abstains_before_decode()
    test_malformed_grade_is_a_conflict()
    test_invalid_owned_grade_cannot_complete_regrade()
    test_regrade_completion_report_is_owned_and_immutable()
    print("camera artifact tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
