#!/usr/bin/env python3
"""Focused regressions for immutable camera capture and grade ownership."""

from __future__ import annotations

import hashlib
import io
import json
import os
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
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
    replay_timing_anchor,
    validate_capture_manifest,
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
        "video": "evidence_exp50.mov",
        "session_start_still": "session_start_exp50.jpg",
        "bright_still": "final_auto.jpg",
        "dim_still": "final_profile.jpg",
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
        "camera_name": "Global Shutter Camera",
        "camera_device_index": 0,
        "profile": {"focus_abs": 306, "video_exposure_time_abs": 50},
        "expected_duration_seconds": 300,
        "video_duration_seconds": 306.5,
        "profile_validation": {"result": "PASS"},
        "errors": [],
        **names,
    }
    return camera_dir, result, encounter


def manifest_fixture(
    root: Path,
    *,
    timing: float = 8.0,
    trace_sha: str = "1" * 40,
    mute_events: list[dict] | None = None,
    completed: bool = True,
) -> tuple[Path, dict]:
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
        replay_mute_signal={
            "schema_version": 1,
            "events": list(mute_events or []),
        },
        replay_completed=completed,
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
            "camera/evidence_exp50.mov",
            "camera/session_start_exp50.jpg",
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
                replay_mute_signal=unchanged["identity"]["replay_mute_signal"],
                replay_completed=True,
            )
            assert_true(
                unchanged["capture_id"] != rebuilt["capture_id"],
                f"capture input change was ignored: {relative}",
            )
        _camera, changed_timing = manifest_fixture(Path(second_tmp) / "other", timing=9.0)
        assert_true(first["capture_id"] != changed_timing["capture_id"], "timing change was ignored")
        _camera, changed_mute = manifest_fixture(
            Path(second_tmp) / "mute",
            mute_events=[
                {"state": "detector_mute", "replaySecond": 20.9, "muted": True},
            ],
        )
        assert_true(
            first["capture_id"] != changed_mute["capture_id"],
            "capture-owned mute schedule change was ignored",
        )
        _camera, incomplete = manifest_fixture(Path(second_tmp) / "incomplete", completed=False)
        assert_true(
            first["capture_id"] != incomplete["capture_id"],
            "capture-owned replay completion change was ignored",
        )
        tampered = json.loads(json.dumps(first))
        tampered["identity"]["replay_mute_signal"]["events"] = changed_mute["identity"][
            "replay_mute_signal"
        ]["events"]
        try:
            validate_capture_manifest(tampered)
        except CameraArtifactError:
            pass
        else:
            raise AssertionError("edited mute schedule retained valid capture ownership")
        tampered_capture = json.loads(json.dumps(first))
        tampered_capture["capture"]["video"] = "unowned.mov"
        assert_true(
            camera_result_view(tampered_capture)["video"] == "evidence_exp50.mov",
            "unowned capture view changed the graded video",
        )
        tampered_timing = json.loads(json.dumps(first))
        tampered_timing["timing_anchor"]["video_seconds"] = 99.0
        try:
            validate_capture_manifest(tampered_timing)
        except CameraArtifactError:
            pass
        else:
            raise AssertionError("unowned top-level timing changed the replay alignment")


def test_replay_timing_anchor_preserves_measured_precision() -> None:
    recording_started = 12_345.123_456_789
    replay_started = 12_353.987_654_321
    expected = replay_started - recording_started
    video_seconds, anchor = replay_timing_anchor(
        "replay",
        recording_started,
        replay_started,
    )
    assert_true(video_seconds == expected, f"measured timing was changed: {video_seconds}")
    assert_true(video_seconds != round(expected, 3), f"timing was rounded to milliseconds: {anchor}")
    assert_true(
        anchor is not None and anchor["video_seconds"] == expected,
        f"timing anchor lost measured precision: {anchor}",
    )


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


def test_camera_publishers_scrub_private_failure_details_before_hashing() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        camera_dir, result, encounter = camera_fixture(Path(tmp) / "run" / "replay")
        private = (
            "/Users/"
            + "private-owner/workspace /dev/cu."
            + "private-port AA:BB:CC:DD:EE:FF SSID='private-network'"
        )
        result["camera_name"] = "Private Owner Camera"
        result["errors"] = [private]
        manifest = build_capture_manifest(
            camera_dir=camera_dir,
            camera_result=result,
            suite="replay",
            product_fingerprint="a" * 64,
            scenario_fingerprint="b" * 64,
            encounter_csv_path=encounter,
            timing_anchor={"kind": "fixture", "detail": private},
            traceability={"repository_sha": "1" * 40, "detail": private},
        )
        assert_true(
            manifest["identity"]["camera"]["name"] == "<redacted-name>",
            f"private camera name survived: {manifest}",
        )
        manifest_path, _created = publish_capture_manifest(camera_dir, manifest)

        fingerprint = current_grader_fingerprint(ROOT)
        grade = grade_fixture(manifest, fingerprint)
        grade["diagnostics"] = [{"code": "fixture", "message": private}]
        grade_path, _created = publish_grade(camera_dir, manifest, fingerprint, grade)

        report_path = camera_dir / "privacy_report.json"
        publish_immutable_json(report_path, {"message": private})
        persisted = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (manifest_path, grade_path, report_path)
        )
        for private_value in (
            "private-owner",
            "cu.private-port",
            "AA:BB:CC:DD:EE:FF",
            "private-network",
            "Private Owner Camera",
        ):
            assert_true(private_value not in persisted, f"private camera data survived: {persisted}")


def test_camera_publication_preserves_owned_hashes_but_scrubs_digest_narrative() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        private_digest = "ab" * 32
        terms_path = root / "terms.txt"
        terms_path.write_text(private_digest + "\n", encoding="utf-8")
        report_path = root / "camera" / "digest_report.json"
        previous = os.environ.get("V1SIMPLE_PRIVACY_TERMS")
        os.environ["V1SIMPLE_PRIVACY_TERMS"] = str(terms_path)
        try:
            publish_immutable_json(
                report_path,
                {
                    "grade_id": private_digest,
                    "input_hashes": {"video": private_digest},
                    "diagnostic": private_digest,
                },
            )
        finally:
            if previous is None:
                os.environ.pop("V1SIMPLE_PRIVACY_TERMS", None)
            else:
                os.environ["V1SIMPLE_PRIVACY_TERMS"] = previous
        published = json.loads(report_path.read_text(encoding="utf-8"))
        assert_true(published["grade_id"] == private_digest, str(published))
        assert_true(published["input_hashes"]["video"] == private_digest, str(published))
        assert_true(published["diagnostic"] == "<redacted-private-term>", str(published))


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


def test_current_grade_refresh_preserves_stale_owned_evidence() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        corpus = Path(tmp)
        camera_dir, manifest = manifest_fixture(corpus)
        publish_capture_manifest(camera_dir, manifest)
        stale_fingerprint = "0" * 64
        publish_grade(
            camera_dir,
            manifest,
            stale_fingerprint,
            grade_fixture(manifest, stale_fingerprint),
        )
        immutable_paths = [
            camera_dir / "capture_manifest.json",
            camera_dir / "grades" / f"{stale_fingerprint}.json",
            *(camera_dir / str(manifest["identity"]["artifacts"][key]["path"]) for key in (
                "video",
                "session_start_still",
                "bright_still",
                "dim_still",
            )),
        ]
        before = {
            path: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in immutable_paths
        }
        original = camera_regrade_module.grade_camera

        def fake_grade(**kwargs: object) -> dict:
            return grade_fixture(manifest, str(kwargs["grader_fingerprint"]))

        camera_regrade_module.grade_camera = fake_grade  # type: ignore[assignment]
        try:
            counts, returncode = camera_regrade_module.regrade_corpus(corpus)
        finally:
            camera_regrade_module.grade_camera = original

        current_fingerprint = current_grader_fingerprint(ROOT)
        current_grade = load_owned_grade(camera_dir, manifest, current_fingerprint)
        after = {
            path: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in immutable_paths
        }
        assert_true(returncode == 0, f"current grade refresh failed: {counts}")
        assert_true(
            counts["graded"] == 1 and counts["skipped"] == 0,
            f"stale grade was mistaken for the current grade: {counts}",
        )
        assert_true(current_grade is not None, "current fingerprint grade was not appended")
        assert_true(before == after, "refresh rewrote captured media or its prior owned grade")


def test_regrade_uses_capture_owned_replay_mute_signal() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        corpus = Path(tmp)
        mute_signal = {
            "schema_version": 1,
            "events": [
                {"state": "detector_mute", "replaySecond": 185, "muted": True},
                {"state": "detector_mute", "replaySecond": 189, "muted": False},
            ],
        }
        camera_dir, manifest = manifest_fixture(corpus, mute_events=mute_signal["events"])
        publish_capture_manifest(camera_dir, manifest)
        write_json(
            camera_dir.parent / "window_result.json",
            {
                "replay": {"completed": False},
                "replay_mute_signal": {
                    "schema_version": 1,
                    "events": [
                        {"state": "detector_mute", "replaySecond": 1, "muted": True},
                    ],
                },
            },
        )
        original = camera_regrade_module.grade_camera
        received: dict[str, object] = {}

        def fake_grade(**kwargs: object) -> dict:
            received.update(kwargs)
            return grade_fixture(manifest, str(kwargs["grader_fingerprint"]))

        camera_regrade_module.grade_camera = fake_grade  # type: ignore[assignment]
        try:
            counts, returncode = camera_regrade_module.regrade_corpus(corpus)
        finally:
            camera_regrade_module.grade_camera = original

        assert_true(returncode == 0 and counts["graded"] == 1, f"regrade failed: {counts}")
        assert_true(
            "replay_mute_signal" not in received
            and "emulator_result" not in received
            and received["capture_manifest"]["identity"]["replay_mute_signal"] == mute_signal,
            f"regrade accepted mutable replay state: {received}",
        )


def test_malformed_replay_mute_signal_abstains_before_decode() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        malformed_camera, malformed_result, malformed_encounter = camera_fixture(
            root / "malformed" / "run" / "replay"
        )
        try:
            build_capture_manifest(
                camera_dir=malformed_camera,
                camera_result=malformed_result,
                suite="replay",
                product_fingerprint="a" * 64,
                scenario_fingerprint="b" * 64,
                encounter_csv_path=malformed_encounter,
                timing_anchor={"kind": "first_emitted_replay_sample", "video_seconds": 8.0},
                replay_mute_signal={
                    "schema_version": 1,
                    "events": [
                        {"state": "detector_mute", "replaySecond": 5, "muted": "yes"}
                    ],
                },
                replay_completed=True,
            )
        except CameraArtifactError:
            pass
        else:
            raise AssertionError("malformed mute schedule entered capture identity")

        legacy_camera, legacy_result, legacy_encounter = camera_fixture(
            root / "legacy" / "run" / "replay"
        )
        unowned_manifest = build_capture_manifest(
            camera_dir=legacy_camera,
            camera_result=legacy_result,
            suite="replay",
            product_fingerprint="a" * 64,
            scenario_fingerprint="b" * 64,
            encounter_csv_path=legacy_encounter,
            timing_anchor={"kind": "first_emitted_replay_sample", "video_seconds": 8.0},
        )
        unowned_grade = camera_regrade_module.grade_camera(
            camera_dir=legacy_camera,
            capture_manifest=unowned_manifest,
            grader_fingerprint=current_grader_fingerprint(ROOT),
        )
        assert_true(
            unowned_grade["result"] == "INCONCLUSIVE"
            and unowned_grade["diagnostics"][0]["code"] == "replay_mute_signal_unowned",
            f"unowned mute schedule did not abstain: {unowned_grade}",
        )

        incomplete_camera, incomplete_manifest = manifest_fixture(
            root / "incomplete",
            completed=False,
        )
        incomplete_grade = camera_regrade_module.grade_camera(
            camera_dir=incomplete_camera,
            capture_manifest=incomplete_manifest,
            grader_fingerprint=current_grader_fingerprint(ROOT),
        )
        assert_true(
            incomplete_grade["result"] == "INCONCLUSIVE"
            and incomplete_grade["diagnostics"][0]["code"] == "replay_incomplete",
            f"capture-owned incomplete replay reached video grading: {incomplete_grade}",
        )


def test_fresh_capture_owns_its_grade_before_bench_scoring() -> None:
    capture_owner = (ROOT / "scripts" / "bench" / "run_window.py").read_text(encoding="utf-8")
    grade = capture_owner.index("camera_grade = grade_camera(")
    publish = capture_owner.index("grade_path, _created = publish_grade(", grade)
    assert_true(grade < publish, "capture owner does not publish the grade it computed")

    bench = (ROOT / "bench.sh").read_text(encoding="utf-8")
    score = bench.index('score_args=(python3 "$ROOT_DIR/tools/bench_score.py"')
    assert_true('wait "$CURRENT_PID"' in bench[:score], "bench scores before the capture window completes")
    assert_true(
        'scripts/bench/camera_regrade.py"' not in bench,
        "fresh bench runs still regrade evidence already graded by the capture owner",
    )


def test_owned_bytes_are_verified_before_resume() -> None:
    for artifact, relative in (
        ("video", Path("camera/evidence_exp50.mov")),
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
                camera_dir=camera_dir,
                capture_manifest=manifest,
                grader_fingerprint=current_grader_fingerprint(ROOT),
            )
        finally:
            camera_regrade_module.grade_camera.__globals__["extract_observations"] = original
        assert_true(grade["result"] == "INCONCLUSIVE", f"missing timing did not abstain: {grade}")
        assert_true(
            grade["diagnostics"][0]["code"] == "timing_anchor_missing",
            f"missing timing diagnostic was imprecise: {grade}",
        )
        assert_true(
            grade["encounter_comparisons"] == [],
            f"missing timing manufactured encounter comparisons: {grade}",
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
            and capture["grade"]["grader_fingerprint"] == fingerprint
            and capture["grade"]["grade_id"] == invalid["grade_id"],
            f"owned invalid grade lost provenance: {capture}",
        )
        assert_true(
            "invalid current camera grade result" in capture["diagnostic"],
            f"invalid-grade diagnostic was imprecise: {capture}",
        )


def test_regrade_failure_report_sanitizes_diagnostics_before_returning() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        corpus = Path(tmp) / "corpus"
        camera_dir, manifest = manifest_fixture(corpus)
        publish_capture_manifest(camera_dir, manifest)
        original = camera_regrade_module.grade_camera
        private_home = "/Users/" + "private-owner/project"
        private_device = "/dev/cu." + "private-camera"
        camera_regrade_module.grade_camera = lambda **_kwargs: (_ for _ in ()).throw(  # type: ignore[assignment]
            RuntimeError(f"decode failed in {corpus / 'private'} at {private_home} via {private_device}")
        )
        try:
            report, returncode = camera_regrade_module.build_regrade_report(corpus)
        finally:
            camera_regrade_module.grade_camera = original

        serialized = json.dumps(report)
        assert_true(returncode == 2 and report["completed"] is False, f"failure looked complete: {report}")
        for private_value in (str(corpus), private_home, private_device):
            assert_true(private_value not in serialized, "regrade report retained private diagnostics")
        assert_true(
            ("/Users/" + "<redacted-user>") in serialized
            and ("/dev/" + "<redacted-device>") in serialized,
            f"regrade diagnostic redaction lost its markers: {report}",
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
        assert_true(
            report["schema_version"] == 2
            and report["scope"] == "complete_corpus_inventory"
            and capture["capture_index"] == 1
            and "capture_path" not in capture
            and "path" not in capture["grade"],
            f"regrade report retained filesystem identity: {report}",
        )

        report_path = Path(tmp) / "report-v2.json"
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

        (camera_dir / "evidence_exp50.mov").write_bytes(b"changed")
        dry_report, dry_returncode = camera_regrade_module.build_regrade_report(corpus, dry_run=True)
        assert_true(dry_returncode == 0, f"dry-run inventory failed: {dry_report}")
        assert_true(
            dry_report["completed"] is False and dry_report["counts"]["processed"] == 0,
            f"dry-run was accepted as completed: {dry_report}",
        )


def test_regrade_cli_scrubs_immutable_report_conflict_path() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        corpus = root / "corpus"
        private_term = "Private" + "ReportDestination"
        terms_path = root / "terms.txt"
        terms_path.write_text(private_term + "\n", encoding="utf-8")
        report_path = root / f"{private_term}.json"
        report_path.write_text("{}\n", encoding="utf-8")
        previous = os.environ.get("V1SIMPLE_PRIVACY_TERMS")
        os.environ["V1SIMPLE_PRIVACY_TERMS"] = str(terms_path)
        stdout = io.StringIO()
        stderr = io.StringIO()
        try:
            with redirect_stdout(stdout), redirect_stderr(stderr):
                returncode = camera_regrade_module.main(
                    ["--corpus-root", str(corpus), "--report", str(report_path)]
                )
        finally:
            if previous is None:
                os.environ.pop("V1SIMPLE_PRIVACY_TERMS", None)
            else:
                os.environ["V1SIMPLE_PRIVACY_TERMS"] = previous
        output = stdout.getvalue() + stderr.getvalue()
        assert_true(returncode == 2, output)
        assert_true(private_term not in output and "Traceback" not in output, output)
        assert_true(
            "<redacted-private-term>" in output or "<redacted-host-path>" in output,
            output,
        )


def main() -> int:
    test_capture_id_stability_and_sensitivity()
    test_replay_timing_anchor_preserves_measured_precision()
    test_immutable_publication_and_conflicts()
    test_camera_publishers_scrub_private_failure_details_before_hashing()
    test_camera_publication_preserves_owned_hashes_but_scrubs_digest_narrative()
    test_resumable_skip_does_not_extract_frames()
    test_current_grade_refresh_preserves_stale_owned_evidence()
    test_regrade_uses_capture_owned_replay_mute_signal()
    test_malformed_replay_mute_signal_abstains_before_decode()
    test_fresh_capture_owns_its_grade_before_bench_scoring()
    test_owned_bytes_are_verified_before_resume()
    test_legacy_preservation_and_relocation()
    test_missing_legacy_timing_abstains_before_decode()
    test_malformed_grade_is_a_conflict()
    test_invalid_owned_grade_cannot_complete_regrade()
    test_regrade_failure_report_sanitizes_diagnostics_before_returning()
    test_regrade_completion_report_is_owned_and_immutable()
    test_regrade_cli_scrubs_immutable_report_conflict_path()
    print("camera artifact tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
