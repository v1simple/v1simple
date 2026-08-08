#!/usr/bin/env python3
"""Regression tests for deterministic bench qualification policy."""

from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from bench_policy import (  # noqa: E402
    FULL_BATCH,
    REGRADE_AND_SMOKE,
    REUSE,
    QualificationError,
    build_grader_revalidation_record,
    build_qualification_record,
    classify_policy,
    load_qualification_record,
    validate_qualification_record,
    write_qualification_record,
)
from camera_artifacts import (  # noqa: E402
    build_capture_manifest,
    capture_input_hashes,
    publish_capture_manifest,
    publish_grade,
)


PRODUCT = "a" * 64
GRADER = "b" * 64
NEW_GRADER = "c" * 64
SCENARIOS = {"core": "1" * 64, "display": "2" * 64, "replay": "3" * 64}
CLEAN_TRACE = {
    "repository_sha": "0123456789abcdef0123456789abcdef01234567",
    "repository_ref": "main",
    "worktree_clean": True,
}


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_error(callable_object, expected: str) -> None:
    try:
        callable_object()
    except QualificationError as exc:
        assert_true(expected in str(exc), str(exc))
    else:
        raise AssertionError(f"expected QualificationError containing {expected!r}")


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def policy_identity(*, product: str = PRODUCT, grader: str = GRADER) -> dict:
    return {
        "product_fingerprint": product,
        "grader_fingerprint": grader,
        "scenario_fingerprints": dict(SCENARIOS),
        "traceability": dict(CLEAN_TRACE),
    }


def grade_fixture(capture: dict, grader: str) -> dict:
    return {
        "schema_version": 3,
        "kind": "bench_camera_grade",
        "capture_id": capture["capture_id"],
        "grader_fingerprint": grader,
        "grade_id": hashlib.sha256(f"{capture['capture_id']}:{grader}".encode("ascii")).hexdigest(),
        "suite": "replay",
        "input_hashes": capture_input_hashes(capture),
        "result": "PASS",
        "confidence": {"result": "PASS"},
        "checks": {},
        "diagnostics": [],
        "errors": [],
    }


def owned_file(path: Path) -> dict:
    return {
        "path": path.name,
        "size_bytes": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }


def smoke_fixture(root: Path, grader: str) -> tuple[Path, dict]:
    smoke_dir = root / "smoke"
    smoke_dir.mkdir()
    profile = {
        "focus_abs": 208,
        "video_exposure_time_abs": 156,
        "bright_exposure_time_abs": 5,
        "dim_exposure_time_abs": 1250,
        "framerate": 30,
        "video_size": "1920x1080",
    }
    camera = {"name": "Razer Kiyo", "device_index": 0, "profile": profile}
    source_still_path = smoke_dir / "session_start_exp156.jpg"
    source_still_path.write_bytes(b"owned-session-start-still")
    source_still = owned_file(source_still_path)
    preflight_path = smoke_dir / "camera_preflight.json"
    write_json(
        preflight_path,
        {
            "schema_version": 1,
            "kind": "bench_camera_preflight",
            "result": "PASS",
            "camera": camera,
            "source_still": {
                "name": source_still["path"],
                "size_bytes": source_still["size_bytes"],
                "sha256": source_still["sha256"],
            },
            "registration": {"transform": {"kind": "translation", "offset_pixels": [0, 0]}},
            "diagnostics": [],
        },
    )
    video_path = smoke_dir / "evidence_exp156.mp4"
    video_path.write_bytes(b"short-owned-video")
    result_path = smoke_dir / "camera_result.json"
    write_json(
        result_path,
        {
            "schema_version": 1,
            "kind": "bench_camera_evidence",
            "result": "CAPTURED",
            "camera_name": camera["name"],
            "camera_device_index": camera["device_index"],
            "profile": profile,
            "expected_duration_seconds": 1,
            "video": video_path.name,
            "session_start_still": source_still_path.name,
            "video_duration_seconds": 3.5,
            "profile_validation": {"result": "PASS"},
            "errors": [],
        },
    )
    artifacts = {
        "preflight": owned_file(preflight_path),
        "session_start_still": source_still,
        "video": owned_file(video_path),
        "camera_result": owned_file(result_path),
    }
    smoke = {
        "schema_version": 2,
        "kind": "bench_camera_smoke",
        "grader_fingerprint": grader,
        "result": "PASS",
        "capture_result": "CAPTURED",
        "camera": camera,
        "preflight": {
            "path": artifacts["preflight"]["path"],
            "sha256": artifacts["preflight"]["sha256"],
        },
        "artifacts": artifacts,
        "diagnostics": [],
    }
    smoke_path = smoke_dir / "camera_smoke.json"
    write_json(smoke_path, smoke)
    return smoke_path, smoke


def full_batch_fixture(
    root: Path,
    *,
    capture_product: str = PRODUCT,
) -> tuple[Path, Path, dict, dict]:
    run_dir = root / "run"
    camera_dir = run_dir / "replay" / "camera"
    camera_dir.mkdir(parents=True)
    evidence_names = {
        "video": "evidence.mp4",
        "session_start_still": "session_start.jpg",
        "bright_still": "bright.jpg",
        "dim_still": "dim.jpg",
    }
    for name in evidence_names.values():
        (camera_dir / name).write_bytes(f"evidence:{name}".encode("ascii"))
    write_json(
        camera_dir / "camera_preflight.json",
        {
            "schema_version": 1,
            "kind": "bench_camera_preflight",
            "result": "PASS",
            "registration": {"transform": {"kind": "translation", "offset_pixels": [0, 0]}},
            "diagnostics": [],
        },
    )
    encounter = run_dir / "replay" / "encounters.csv"
    encounter.parent.mkdir(parents=True, exist_ok=True)
    encounter.write_text("millis,event\n0,SAMPLE\n", encoding="utf-8")
    physical = {
        "result": "CAPTURED",
        "camera_name": "fixture",
        "camera_device_index": 0,
        "profile": {"video_exposure_time_abs": 156},
        "expected_duration_seconds": 300,
        "video_duration_seconds": 300.0,
        **evidence_names,
        "errors": [],
    }
    capture = build_capture_manifest(
        camera_dir=camera_dir,
        camera_result=physical,
        suite="replay",
        product_fingerprint=capture_product,
        scenario_fingerprint=SCENARIOS["replay"],
        encounter_csv_path=encounter,
        timing_anchor={"kind": "first_emitted_replay_sample", "video_seconds": 8.0},
    )
    publish_capture_manifest(camera_dir, capture)
    grade = grade_fixture(capture, GRADER)
    publish_grade(camera_dir, capture, GRADER, grade)

    windows = []
    for suite in ("core", "display", "replay"):
        window = {
            "suite": suite,
            "result": "PASS",
            "git_worktree_clean": True,
            "product_fingerprint": PRODUCT,
            "grader_fingerprint": GRADER,
            "scenario_fingerprint": SCENARIOS[suite],
        }
        if suite == "replay":
            window["camera"] = {
                "result": "PASS",
                "capture_result": "CAPTURED",
                "capture_id": capture["capture_id"],
                "grader_fingerprint": GRADER,
                "mechanical_result": "PASS",
                "visually_graded": True,
                "confidence": {"result": "PASS"},
                "diagnostics": [],
                "gate_required": True,
            }
        windows.append(window)
    result = {
        "schema_version": 3,
        "kind": "bench_result",
        "run_dir": str(run_dir.resolve()),
        "git_sha": CLEAN_TRACE["repository_sha"],
        "git_ref": "main",
        "product_fingerprint": PRODUCT,
        "grader_fingerprint": GRADER,
        "git_worktree_clean": True,
        "result": "PASS",
        "windows": windows,
    }
    result_path = run_dir / "bench_result.json"
    write_json(result_path, result)
    return result_path, camera_dir, capture, grade


def test_decision_precedence_and_repository_trace_is_inert() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        result_path, _camera_dir, _capture, _grade = full_batch_fixture(Path(temporary))
        accepted = build_qualification_record(
            result_path,
            board_id="fixture",
            current_identity=policy_identity(),
            current_traceability=CLEAN_TRACE,
        )
        current = policy_identity()
        current["traceability"] = {
            "repository_sha": "f" * 40,
            "repository_ref": "different-ref",
            "worktree_clean": False,
        }
        assert_true(classify_policy(current, accepted)[0] == REUSE, "repository trace changed policy")

        grader_only = copy.deepcopy(current)
        grader_only["grader_fingerprint"] = NEW_GRADER
        assert_true(
            classify_policy(grader_only, accepted)[0] == REGRADE_AND_SMOKE,
            "grader-only change did not choose regrade and smoke",
        )
        scenario_only = copy.deepcopy(grader_only)
        scenario_only["scenario_fingerprints"]["replay"] = "d" * 64
        assert_true(
            classify_policy(scenario_only, accepted)[0] == FULL_BATCH,
            "scenario change was ignored",
        )
        product_and_grader = copy.deepcopy(grader_only)
        product_and_grader["product_fingerprint"] = "e" * 64
        assert_true(
            classify_policy(product_and_grader, accepted)[0] == FULL_BATCH,
            "product change did not take precedence",
        )
        incomplete = copy.deepcopy(current)
        del incomplete["scenario_fingerprints"]["display"]
        assert_true(
            classify_policy(incomplete, accepted)[0] == FULL_BATCH,
            "missing scenario was accepted",
        )
        assert_true(classify_policy(current, None)[0] == FULL_BATCH, "missing record was accepted")
        assert_true(classify_policy(current, {})[0] == FULL_BATCH, "malformed record was accepted")


def test_full_record_requires_clean_complete_strict_current_evidence() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        result_path, camera_dir, _capture, _grade = full_batch_fixture(root)
        record = build_qualification_record(
            result_path,
            board_id="fixture",
            current_identity=policy_identity(),
            current_traceability=CLEAN_TRACE,
        )
        validate_qualification_record(record)
        assert_true(record["scenario_fingerprints"] == SCENARIOS, "suite scenarios not retained")

        payload = json.loads(result_path.read_text(encoding="utf-8"))
        for state in ("WARN", "FAIL", "EVIDENCE_FAILED"):
            changed = copy.deepcopy(payload)
            changed["windows"][1]["result"] = state
            write_json(result_path, changed)
            expect_error(
                lambda: build_qualification_record(
                    result_path,
                    board_id="fixture",
                    current_identity=policy_identity(),
                    current_traceability=CLEAN_TRACE,
                ),
                "display does not own",
            )
        partial = copy.deepcopy(payload)
        partial["windows"] = partial["windows"][:2]
        write_json(result_path, partial)
        expect_error(
            lambda: build_qualification_record(
                result_path,
                board_id="fixture",
                current_identity=policy_identity(),
                current_traceability=CLEAN_TRACE,
            ),
            "exactly the full batch",
        )
        dirty = copy.deepcopy(payload)
        dirty["git_worktree_clean"] = False
        write_json(result_path, dirty)
        expect_error(
            lambda: build_qualification_record(
                result_path,
                board_id="fixture",
                current_identity=policy_identity(),
                current_traceability=CLEAN_TRACE,
            ),
            "was not collected from a clean worktree",
        )
        write_json(result_path, payload)
        dirty_now = {**CLEAN_TRACE, "worktree_clean": False}
        expect_error(
            lambda: build_qualification_record(
                result_path,
                board_id="fixture",
                current_identity=policy_identity(),
                current_traceability=dirty_now,
            ),
            "not clean at qualification time",
        )
        non_strict = copy.deepcopy(payload)
        non_strict["windows"][2]["camera"]["visually_graded"] = False
        write_json(result_path, non_strict)
        expect_error(
            lambda: build_qualification_record(
                result_path,
                board_id="fixture",
                current_identity=policy_identity(),
                current_traceability=CLEAN_TRACE,
            ),
            "strict replay camera PASS",
        )
        write_json(result_path, payload)
        (camera_dir / "evidence.mp4").write_bytes(b"changed-media")
        expect_error(
            lambda: build_qualification_record(
                result_path,
                board_id="fixture",
                current_identity=policy_identity(),
                current_traceability=CLEAN_TRACE,
            ),
            "camera capture input",
        )

    with tempfile.TemporaryDirectory() as temporary:
        stale_path, _camera_dir, _capture, _grade = full_batch_fixture(
            Path(temporary), capture_product="f" * 64
        )
        expect_error(
            lambda: build_qualification_record(
                stale_path,
                board_id="fixture",
                current_identity=policy_identity(),
                current_traceability=CLEAN_TRACE,
            ),
            "stale product fingerprint",
        )


def test_qualification_publication_is_immutable_and_missing_is_conservative() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        missing, reason = load_qualification_record(root / "missing.json")
        assert_true(missing is None and "no accepted" in reason, reason)
        result_path, camera_dir, _capture, _grade = full_batch_fixture(root)
        record = build_qualification_record(
            result_path,
            board_id="fixture",
            current_identity=policy_identity(),
            current_traceability=CLEAN_TRACE,
        )
        destination = root / "qualification.json"
        write_qualification_record(destination, record)
        write_qualification_record(destination, record)
        changed = copy.deepcopy(record)
        changed["board_id"] = "other"
        changed.pop("qualification_id")
        from bench_identity import canonical_bytes  # noqa: PLC0415

        changed["qualification_id"] = hashlib.sha256(canonical_bytes(changed)).hexdigest()
        expect_error(lambda: write_qualification_record(destination, changed), "already differs")

        loaded, reason = load_qualification_record(destination)
        assert_true(loaded == record and not reason, f"valid qualification did not reload: {reason}")
        video_path = camera_dir / "evidence.mp4"
        original_video = video_path.read_bytes()
        video_path.write_bytes(original_video + b"tampered-after-acceptance")
        loaded, reason = load_qualification_record(destination)
        assert_true(loaded is None and "camera capture input" in reason, reason)
        assert_true(
            classify_policy(policy_identity(), record)[0] == FULL_BATCH,
            "tampered accepted replay video still allowed REUSE",
        )
        video_path.write_bytes(original_video)
        original_result = result_path.read_bytes()
        result_path.unlink()
        loaded, reason = load_qualification_record(destination)
        assert_true(loaded is None and "bench result" in reason, reason)
        result_path.write_bytes(original_result)


def test_grader_revalidation_converges_and_rejects_incomplete_evidence() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        result_path, camera_dir, capture, _grade = full_batch_fixture(root)
        prior = build_qualification_record(
            result_path,
            board_id="fixture",
            current_identity=policy_identity(),
            current_traceability=CLEAN_TRACE,
        )
        prior_path = root / "qualification-old.json"
        write_qualification_record(prior_path, prior)
        current_grade = grade_fixture(capture, NEW_GRADER)
        publish_grade(camera_dir, capture, NEW_GRADER, current_grade)

        report = {
            "schema_version": 1,
            "kind": "bench_camera_regrade_report",
            "completed": True,
            "dry_run": False,
            "grader_fingerprint": NEW_GRADER,
            "path_base": "corpus_root",
            "corpus_root": ".",
            "counts": {
                "discovered": 1,
                "processed": 1,
                "graded": 1,
                "skipped": 0,
                "pass": 1,
                "fail": 0,
                "inconclusive": 0,
                "incompatible": 0,
                "conflict": 0,
            },
            "captures": [
                {
                    "capture_path": "run/replay/camera",
                    "capture_id": capture["capture_id"],
                    "result": "PASS",
                    "confidence_result": "PASS",
                    "ownership_valid": True,
                    "grade": {
                        "status": "graded",
                        "path": f"run/replay/camera/grades/{NEW_GRADER}.json",
                        "ownership_valid": True,
                    },
                    "diagnostic": "",
                }
            ],
        }
        report_path = root / "regrade-report.json"
        write_json(report_path, report)
        smoke_path, smoke = smoke_fixture(root, NEW_GRADER)
        current = policy_identity(grader=NEW_GRADER)
        advanced = build_grader_revalidation_record(
            prior_path,
            report_path,
            smoke_path,
            current_identity=current,
            current_traceability=CLEAN_TRACE,
        )
        validate_qualification_record(advanced)
        assert_true(classify_policy(current, advanced)[0] == REUSE, "grader transition did not converge")
        assert_true(
            advanced["evidence"]["replay_capture_id"] == prior["evidence"]["replay_capture_id"],
            "grader transition replaced accepted product capture",
        )
        advanced_path = root / "qualification-new.json"
        write_qualification_record(advanced_path, advanced)
        loaded, reason = load_qualification_record(advanced_path)
        assert_true(loaded == advanced and not reason, f"grader qualification did not reload: {reason}")

        for linked_path, label in (
            (report_path, "regrade report"),
            (smoke_path, "camera smoke"),
            (prior_path, "prior qualification"),
        ):
            original = linked_path.read_bytes()
            linked_path.write_bytes(original + b"tampered-link")
            loaded, reason = load_qualification_record(advanced_path)
            assert_true(loaded is None and label in reason, reason)
            linked_path.write_bytes(original)

        current_grade_path = camera_dir / "grades" / f"{NEW_GRADER}.json"
        original_grade = current_grade_path.read_bytes()
        changed_grade = json.loads(original_grade)
        changed_grade["result"] = "FAIL"
        write_json(current_grade_path, changed_grade)
        loaded, reason = load_qualification_record(advanced_path)
        assert_true(loaded is None and "confident current PASS" in reason, reason)
        current_grade_path.write_bytes(original_grade)

        escaped = copy.deepcopy(advanced)
        escaped["evidence"]["regrade_report"] = "../regrade-report.json"
        escaped.pop("qualification_id")
        from bench_identity import canonical_bytes  # noqa: PLC0415

        escaped["qualification_id"] = hashlib.sha256(canonical_bytes(escaped)).hexdigest()
        assert_true(
            classify_policy(current, escaped)[0] == FULL_BATCH,
            "unsafe grader-transition evidence path still allowed REUSE",
        )

        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                root / "missing-report.json",
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "invalid JSON artifact",
        )
        stale = copy.deepcopy(report)
        stale["grader_fingerprint"] = GRADER
        write_json(report_path, stale)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "wrong grader",
        )
        incomplete = copy.deepcopy(report)
        incomplete["counts"]["processed"] = 0
        write_json(report_path, incomplete)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "complete corpus",
        )
        inconsistent = copy.deepcopy(report)
        inconsistent["counts"]["pass"] = 0
        inconsistent["counts"]["fail"] = 1
        write_json(report_path, inconsistent)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "aggregates",
        )
        fake_unowned = copy.deepcopy(report)
        fake_unowned["counts"].update({"discovered": 2, "processed": 2, "graded": 2, "pass": 2})
        fake_unowned["captures"].append(
            {
                "capture_path": "fake/replay/camera",
                "capture_id": "d" * 64,
                "result": "PASS",
                "confidence_result": "PASS",
                "ownership_valid": False,
                "grade": {
                    "status": "graded",
                    "path": f"fake/replay/camera/grades/{NEW_GRADER}.json",
                    "ownership_valid": False,
                },
                "diagnostic": "",
            }
        )
        write_json(report_path, fake_unowned)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "unowned",
        )
        unsafe = copy.deepcopy(report)
        unsafe["captures"][0]["capture_path"] = "../replay/camera"
        write_json(report_path, unsafe)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "safe normalized relative path",
        )
        duplicate_path = copy.deepcopy(report)
        duplicate_path["counts"].update({"discovered": 2, "processed": 2, "graded": 2, "pass": 2})
        duplicate_entry = copy.deepcopy(duplicate_path["captures"][0])
        duplicate_entry["capture_id"] = "d" * 64
        duplicate_path["captures"].append(duplicate_entry)
        write_json(report_path, duplicate_path)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "duplicate capture ownership",
        )
        duplicate_id = copy.deepcopy(report)
        duplicate_id["counts"].update({"discovered": 2, "processed": 2, "graded": 2, "pass": 2})
        duplicate_entry = copy.deepcopy(duplicate_id["captures"][0])
        duplicate_entry["capture_path"] = "other/replay/camera"
        duplicate_entry["grade"]["path"] = f"other/replay/camera/grades/{NEW_GRADER}.json"
        duplicate_id["captures"].append(duplicate_entry)
        write_json(report_path, duplicate_id)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "duplicate capture ownership",
        )
        wrong_capture = copy.deepcopy(report)
        wrong_capture["captures"][0]["capture_id"] = "d" * 64
        write_json(report_path, wrong_capture)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "accepted capture",
        )
        write_json(report_path, report)
        metadata_only_smoke = {
            "schema_version": 1,
            "kind": "bench_camera_smoke",
            "grader_fingerprint": NEW_GRADER,
            "result": "PASS",
            "capture_result": "CAPTURED",
            "diagnostics": [],
        }
        write_json(smoke_path, metadata_only_smoke)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "current-grader PASS",
        )
        write_json(smoke_path, smoke)
        missing_source_ownership = copy.deepcopy(smoke)
        del missing_source_ownership["artifacts"]["session_start_still"]
        write_json(smoke_path, missing_source_ownership)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "complete live lifecycle",
        )
        write_json(smoke_path, smoke)
        for artifact_name in ("preflight", "session_start_still", "video", "camera_result"):
            artifact_path = smoke_path.parent / smoke["artifacts"][artifact_name]["path"]
            original = artifact_path.read_bytes()
            artifact_path.unlink()
            expect_error(
                lambda: build_grader_revalidation_record(
                    prior_path,
                    report_path,
                    smoke_path,
                    current_identity=current,
                    current_traceability=CLEAN_TRACE,
                ),
                "owned file",
            )
            artifact_path.write_bytes(original + b"tampered")
            expect_error(
                lambda: build_grader_revalidation_record(
                    prior_path,
                    report_path,
                    smoke_path,
                    current_identity=current,
                    current_traceability=CLEAN_TRACE,
                ),
                "bytes do not match",
            )
            artifact_path.write_bytes(original)

        preflight_path = smoke_path.parent / smoke["artifacts"]["preflight"]["path"]
        original_preflight = json.loads(preflight_path.read_text(encoding="utf-8"))
        mismatched_preflight = copy.deepcopy(original_preflight)
        mismatched_preflight["source_still"]["sha256"] = "f" * 64
        write_json(preflight_path, mismatched_preflight)
        source_mismatch_smoke = copy.deepcopy(smoke)
        source_mismatch_smoke["artifacts"]["preflight"] = owned_file(preflight_path)
        source_mismatch_smoke["preflight"] = {
            "path": source_mismatch_smoke["artifacts"]["preflight"]["path"],
            "sha256": source_mismatch_smoke["artifacts"]["preflight"]["sha256"],
        }
        write_json(smoke_path, source_mismatch_smoke)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "source-still ownership",
        )
        write_json(preflight_path, original_preflight)
        write_json(smoke_path, smoke)
        failed_smoke = copy.deepcopy(smoke)
        failed_smoke["result"] = "INCONCLUSIVE"
        write_json(smoke_path, failed_smoke)
        expect_error(
            lambda: build_grader_revalidation_record(
                prior_path,
                report_path,
                smoke_path,
                current_identity=current,
                current_traceability=CLEAN_TRACE,
            ),
            "current-grader PASS",
        )


def main() -> None:
    test_decision_precedence_and_repository_trace_is_inert()
    test_full_record_requires_clean_complete_strict_current_evidence()
    test_qualification_publication_is_immutable_and_missing_is_conservative()
    test_grader_revalidation_converges_and_rejects_incomplete_evidence()
    print("bench policy tests passed")


if __name__ == "__main__":
    main()
