#!/usr/bin/env python3
"""Plan bench evidence work from content identities, never from repository SHA."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Mapping

from bench_identity import (
    canonical_bytes,
    current_grader_fingerprint,
    current_hardware_scoring_fingerprint,
    current_product_fingerprint,
    git_traceability,
    scenario_manifest,
)
from camera_artifacts import (
    CAPTURE_MANIFEST_NAME,
    CameraArtifactError,
    load_capture_manifest,
    load_owned_grade,
    sha256_file,
    strict_grade_outcome,
    verify_capture_files,
)
from camera_contract import EXPECTED_CAMERA_NAME, EXPECTED_CAMERA_PROFILE
from artifact_privacy import privacy_safe_identifier


ROOT = Path(__file__).resolve().parents[2]
REQUIRED_SUITES = ("core", "display", "replay")
QUALIFICATION_SCHEMA_VERSION = 2
PLAN_SCHEMA_VERSION = 2
HEX_DIGEST_LENGTH = 64
FULL_BATCH = "FULL_BATCH"
REGRADE_AND_SMOKE = "REGRADE_AND_SMOKE"
REUSE = "REUSE"


class QualificationError(RuntimeError):
    """A candidate qualification does not own sufficient accepted evidence."""


def _valid_digest(value: Any) -> bool:
    text = str(value or "")
    return len(text) == HEX_DIGEST_LENGTH and all(character in "0123456789abcdef" for character in text)


def _read_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise QualificationError(f"invalid JSON artifact {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise QualificationError(f"JSON artifact is not an object: {path}")
    return payload


def _safe_relative_path(value: Any, label: str) -> PurePosixPath:
    text = str(value or "")
    path = PurePosixPath(text)
    if (
        not text
        or text in {".", ".."}
        or not path.parts
        or "\\" in text
        or path.is_absolute()
        or any(part in {"", ".", ".."} for part in path.parts)
        or path.as_posix() != text
    ):
        raise QualificationError(f"{label} is not a safe normalized relative path")
    return path


def _resolve_owned_file(root: Path, entry: Any, label: str) -> Path:
    if not isinstance(entry, dict) or set(entry) != {"path", "size_bytes", "sha256"}:
        raise QualificationError(f"{label} has an invalid ownership entry")
    relative = _safe_relative_path(entry.get("path"), f"{label} path")
    expected_size = entry.get("size_bytes")
    if (
        not isinstance(expected_size, int)
        or isinstance(expected_size, bool)
        or expected_size < 1
        or not _valid_digest(entry.get("sha256"))
    ):
        raise QualificationError(f"{label} has invalid size or hash ownership")
    resolved_root = root.resolve()
    candidate = root.joinpath(*relative.parts)
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(resolved_root)
    except (FileNotFoundError, ValueError) as exc:
        raise QualificationError(f"{label} does not resolve to an owned file") from exc
    if not resolved.is_file():
        raise QualificationError(f"{label} does not resolve to an owned file")
    if resolved.stat().st_size != expected_size or sha256_file(resolved) != entry.get("sha256"):
        raise QualificationError(f"{label} bytes do not match their ownership entry")
    return resolved


def current_policy_identity(
    root: Path,
    *,
    duration_seconds: int = 300,
    replay_duration_seconds: int = 300,
    profile: str = "drive_wifi_off",
    segment: str = "last",
    blink_profile: str = "scenario",
    traceability: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Return the identities needed for a policy decision."""
    root = root.resolve()
    scenarios = {
        suite: scenario_manifest(
            suite=suite,
            duration_seconds=replay_duration_seconds if suite == "replay" else duration_seconds,
            profile=profile,
            segment=segment,
            blink_profile=blink_profile if suite == "replay" else None,
        )["fingerprint"]
        for suite in REQUIRED_SUITES
    }
    return {
        "product_fingerprint": current_product_fingerprint(root),
        "grader_fingerprint": current_grader_fingerprint(root),
        "hardware_scoring_fingerprint": current_hardware_scoring_fingerprint(root),
        "scenario_fingerprints": scenarios,
        "scenario_parameters": {
            "duration_seconds": duration_seconds,
            "replay_duration_seconds": replay_duration_seconds,
            "profile": profile,
            "segment": segment,
            "blink_profile": blink_profile,
        },
        "traceability": dict(traceability) if traceability is not None else git_traceability(root),
    }


def validate_qualification_record(record: Mapping[str, Any]) -> None:
    if (
        record.get("schema_version") != QUALIFICATION_SCHEMA_VERSION
        or record.get("kind") != "bench_qualification"
    ):
        raise QualificationError("invalid qualification schema")
    for field in (
        "product_fingerprint",
        "grader_fingerprint",
        "hardware_scoring_fingerprint",
    ):
        if not _valid_digest(record.get(field)):
            raise QualificationError(f"invalid qualification {field}")
    scenarios = record.get("scenario_fingerprints")
    if not isinstance(scenarios, dict) or set(scenarios) != set(REQUIRED_SUITES):
        raise QualificationError("qualification does not own all required suite scenarios")
    if any(not _valid_digest(scenarios.get(suite)) for suite in REQUIRED_SUITES):
        raise QualificationError("qualification contains an invalid scenario fingerprint")
    trace = record.get("traceability")
    if not isinstance(trace, dict) or trace.get("worktree_clean") is not True:
        raise QualificationError("qualification was not recorded from a clean worktree")
    evidence = record.get("evidence")
    if not isinstance(evidence, dict) or evidence.get("kind") not in {
        "full_batch",
        "grader_revalidation",
    }:
        raise QualificationError("qualification has no accepted evidence")
    if evidence.get("suites") != list(REQUIRED_SUITES):
        raise QualificationError("qualification evidence does not cover the full batch")
    for field in ("bench_result_sha256", "replay_capture_id", "replay_grade_id"):
        if not _valid_digest(evidence.get(field)):
            raise QualificationError(f"qualification evidence has invalid {field}")
    if evidence.get("kind") == "grader_revalidation":
        for field in (
            "prior_qualification_id",
            "prior_qualification_sha256",
            "regrade_report_sha256",
            "camera_smoke_sha256",
        ):
            if not _valid_digest(evidence.get(field)):
                raise QualificationError(f"grader revalidation has invalid {field}")
    qualification_id = record.get("qualification_id")
    content = {key: value for key, value in record.items() if key != "qualification_id"}
    if qualification_id != hashlib.sha256(canonical_bytes(content)).hexdigest():
        raise QualificationError("qualification_id does not match the accepted record")


def load_qualification_record(
    path: Path,
    *,
    validate_evidence: bool = True,
) -> tuple[dict[str, Any] | None, str]:
    if not path.is_file():
        return None, "no accepted qualification record exists"
    try:
        record = _read_json(path)
        validate_qualification_record(record)
        if validate_evidence:
            validate_qualification_evidence(record)
    except QualificationError as exc:
        return None, str(exc)
    return record, ""


def classify_policy(
    current: Mapping[str, Any],
    accepted: Mapping[str, Any] | None,
    *,
    invalid_reason: str = "",
) -> tuple[str, str]:
    """Choose the minimum sufficient evidence action, conservatively."""
    if accepted is None:
        return FULL_BATCH, invalid_reason or "no valid accepted qualification exists"
    try:
        validate_qualification_record(accepted)
    except QualificationError as exc:
        return FULL_BATCH, str(exc)

    current_product = str(current.get("product_fingerprint") or "")
    current_grader = str(current.get("grader_fingerprint") or "")
    current_hardware_scoring = str(current.get("hardware_scoring_fingerprint") or "")
    current_scenarios = current.get("scenario_fingerprints")
    if (
        not _valid_digest(current_product)
        or not _valid_digest(current_grader)
        or not _valid_digest(current_hardware_scoring)
    ):
        return FULL_BATCH, "current product, grader, or hardware-scoring identity is invalid"
    if current_product != accepted.get("product_fingerprint"):
        return FULL_BATCH, "product fingerprint changed"
    if current_hardware_scoring != accepted.get("hardware_scoring_fingerprint"):
        return FULL_BATCH, "hardware scoring fingerprint changed"
    if not isinstance(current_scenarios, dict) or set(current_scenarios) != set(REQUIRED_SUITES):
        return FULL_BATCH, "current scenario coverage is incomplete"
    if any(not _valid_digest(current_scenarios.get(suite)) for suite in REQUIRED_SUITES):
        return FULL_BATCH, "current scenario identity is invalid"
    accepted_scenarios = accepted["scenario_fingerprints"]
    changed_suites = [
        suite for suite in REQUIRED_SUITES if current_scenarios[suite] != accepted_scenarios[suite]
    ]
    if changed_suites:
        return FULL_BATCH, "scenario fingerprint changed for: " + ", ".join(changed_suites)
    try:
        validate_qualification_evidence(accepted)
    except QualificationError as exc:
        return FULL_BATCH, f"accepted qualification evidence is invalid: {exc}"
    if current_grader != accepted.get("grader_fingerprint"):
        return REGRADE_AND_SMOKE, "camera grader fingerprint changed"
    return REUSE, "product, scenario, hardware-scoring, and grader fingerprints match"


def _strict_replay_evidence(
    bench_result: Mapping[str, Any],
    bench_result_path: Path,
    replay_window: Mapping[str, Any],
    grader_fingerprint: str,
    product_fingerprint: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    run_dir = bench_result_path.resolve().parent
    declared_run_dir = Path(str(bench_result.get("run_dir") or ""))
    if not declared_run_dir.is_absolute():
        declared_run_dir = bench_result_path.parent / declared_run_dir
    if declared_run_dir.resolve() != run_dir:
        raise QualificationError("bench result run_dir does not own its artifact location")
    camera_dir = run_dir / "replay" / "camera"
    try:
        capture = load_capture_manifest(camera_dir / CAPTURE_MANIFEST_NAME)
        verify_capture_files(camera_dir, capture)
        grade = load_owned_grade(camera_dir, capture, grader_fingerprint)
    except CameraArtifactError as exc:
        raise QualificationError(f"replay camera ownership is not strict: {exc}") from exc
    if grade is None:
        raise QualificationError("replay camera has no current owned grade")
    identity = capture.get("identity") if isinstance(capture.get("identity"), dict) else {}
    if identity.get("suite") != "replay":
        raise QualificationError("replay camera capture owns the wrong suite")
    if identity.get("product_fingerprint") != product_fingerprint:
        raise QualificationError("replay camera capture owns a stale product fingerprint")
    if identity.get("scenario_fingerprint") != replay_window.get("scenario_fingerprint"):
        raise QualificationError("replay camera capture owns a stale scenario fingerprint")
    if capture.get("result") != "CAPTURED" or (capture.get("preflight") or {}).get("result") != "PASS":
        raise QualificationError("replay camera capture lacks a successful owned preflight")
    if strict_grade_outcome(grade)[0] != "PASS":
        raise QualificationError("replay camera grade is not a confident strict PASS")
    camera = replay_window.get("camera") if isinstance(replay_window.get("camera"), dict) else {}
    if (
        camera.get("gate_required") is not True
        or camera.get("capture_result") != "CAPTURED"
        or camera.get("result") != "PASS"
        or camera.get("mechanical_result") != "PASS"
        or camera.get("visually_graded") is not True
        or camera.get("capture_id") != capture.get("capture_id")
        or camera.get("grader_fingerprint") != grader_fingerprint
    ):
        raise QualificationError("bench result does not retain the strict replay camera PASS")
    return capture, grade


def build_qualification_record(
    bench_result_path: Path,
    *,
    board_id: str,
    current_identity: Mapping[str, Any],
    current_traceability: Mapping[str, Any],
) -> dict[str, Any]:
    """Validate a full batch and create its accepted qualification record."""
    bench_result_path = bench_result_path.resolve()
    bench_result = _read_json(bench_result_path)
    if bench_result.get("kind") != "bench_result" or bench_result.get("schema_version") != 5:
        raise QualificationError("candidate is not a current bench result")
    if bench_result.get("result") != "PASS":
        raise QualificationError("candidate full batch did not PASS")
    if bench_result.get("git_worktree_clean") is not True:
        raise QualificationError("candidate full batch was not collected from a clean worktree")
    if current_traceability.get("worktree_clean") is not True:
        raise QualificationError("repository is not clean at qualification time")

    product = str(bench_result.get("product_fingerprint") or "")
    grader = str(bench_result.get("grader_fingerprint") or "")
    hardware_scoring = str(bench_result.get("hardware_scoring_fingerprint") or "")
    if (
        not _valid_digest(product)
        or not _valid_digest(grader)
        or not _valid_digest(hardware_scoring)
    ):
        raise QualificationError(
            "candidate full batch has invalid product, grader, or hardware-scoring identity"
        )
    if product != current_identity.get("product_fingerprint"):
        raise QualificationError("candidate full batch owns a stale product fingerprint")
    if grader != current_identity.get("grader_fingerprint"):
        raise QualificationError("candidate full batch owns a stale grader fingerprint")
    if hardware_scoring != current_identity.get("hardware_scoring_fingerprint"):
        raise QualificationError("candidate full batch owns a stale hardware-scoring fingerprint")

    raw_windows = bench_result.get("windows")
    if not isinstance(raw_windows, list) or len(raw_windows) != len(REQUIRED_SUITES):
        raise QualificationError("candidate does not contain exactly the full batch")
    windows: dict[str, Mapping[str, Any]] = {}
    for window in raw_windows:
        if not isinstance(window, dict):
            raise QualificationError("candidate contains an invalid suite result")
        suite = str(window.get("suite") or "")
        if suite in windows or suite not in REQUIRED_SUITES:
            raise QualificationError("candidate contains duplicate or unexpected suites")
        windows[suite] = window
    if set(windows) != set(REQUIRED_SUITES):
        raise QualificationError("candidate does not cover core, display, and replay")

    scenarios: dict[str, str] = {}
    for suite in REQUIRED_SUITES:
        window = windows[suite]
        scenario = str(window.get("scenario_fingerprint") or "")
        if (
            window.get("result") != "PASS"
            or window.get("window_schema_version") != 3
            or window.get("git_worktree_clean") is not True
            or window.get("product_fingerprint") != product
            or window.get("grader_fingerprint") != grader
            or window.get("hardware_scoring_fingerprint") != hardware_scoring
            or not _valid_digest(scenario)
        ):
            raise QualificationError(f"{suite} does not own a clean current PASS")
        scenarios[suite] = scenario

    capture, grade = _strict_replay_evidence(
        bench_result,
        bench_result_path,
        windows["replay"],
        grader,
        product,
    )
    trace = {
        "repository_sha": str(current_traceability.get("repository_sha") or ""),
        "repository_ref": str(current_traceability.get("repository_ref") or ""),
        "worktree_clean": True,
    }
    safe_board_id = privacy_safe_identifier(board_id, namespace="board")
    record = {
        "schema_version": QUALIFICATION_SCHEMA_VERSION,
        "kind": "bench_qualification",
        "board_id": safe_board_id,
        "product_fingerprint": product,
        "grader_fingerprint": grader,
        "hardware_scoring_fingerprint": hardware_scoring,
        "scenario_fingerprints": scenarios,
        "traceability": trace,
        "evidence": {
            "kind": "full_batch",
            "suites": list(REQUIRED_SUITES),
            "bench_result": str(bench_result_path),
            "bench_result_sha256": sha256_file(bench_result_path),
            "replay_capture_id": capture["capture_id"],
            "replay_grade_id": grade.get("grade_id"),
        },
    }
    record["qualification_id"] = hashlib.sha256(canonical_bytes(record)).hexdigest()
    validate_qualification_record(record)
    return record


def _accepted_capture_with_current_grade(
    prior: Mapping[str, Any],
    current_grader: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    evidence = prior["evidence"]
    bench_result_path = Path(str(evidence.get("bench_result") or ""))
    if not bench_result_path.is_file() or sha256_file(bench_result_path) != evidence.get(
        "bench_result_sha256"
    ):
        raise QualificationError("prior accepted bench result is missing or changed")
    bench_result = _read_json(bench_result_path)
    run_dir = bench_result_path.resolve().parent
    declared_run_dir = Path(str(bench_result.get("run_dir") or ""))
    if not declared_run_dir.is_absolute():
        declared_run_dir = bench_result_path.parent / declared_run_dir
    if declared_run_dir.resolve() != run_dir:
        raise QualificationError("prior bench result no longer owns its run directory")
    camera_dir = run_dir / "replay" / "camera"
    try:
        capture = load_capture_manifest(camera_dir / CAPTURE_MANIFEST_NAME)
        verify_capture_files(camera_dir, capture)
        grade = load_owned_grade(camera_dir, capture, current_grader)
    except CameraArtifactError as exc:
        raise QualificationError(f"accepted replay capture cannot be revalidated: {exc}") from exc
    if capture.get("capture_id") != evidence.get("replay_capture_id"):
        raise QualificationError("regraded replay capture is not the accepted capture")
    identity = capture.get("identity") if isinstance(capture.get("identity"), dict) else {}
    if (
        identity.get("product_fingerprint") != prior.get("product_fingerprint")
        or identity.get("scenario_fingerprint")
        != prior.get("scenario_fingerprints", {}).get("replay")
    ):
        raise QualificationError("accepted replay capture owns stale product or scenario evidence")
    if grade is None:
        raise QualificationError("accepted replay capture has no current grader result")
    if (
        strict_grade_outcome(grade)[0] != "PASS"
        or not _valid_digest(grade.get("grade_id"))
    ):
        raise QualificationError("accepted replay capture did not earn a confident current PASS")
    return capture, grade


def _validate_regrade_report(
    report: Mapping[str, Any],
    *,
    current_grader: str,
    accepted_capture_id: str,
) -> None:
    if (
        report.get("schema_version") != 2
        or report.get("kind") != "bench_camera_regrade_report"
        or report.get("completed") is not True
        or report.get("dry_run") is not False
        or report.get("grader_fingerprint") != current_grader
        or report.get("scope") != "complete_corpus_inventory"
    ):
        raise QualificationError("regrade report is incomplete or owns the wrong grader")
    counts = report.get("counts")
    if not isinstance(counts, dict):
        raise QualificationError("regrade report has no completion counts")
    required_counts = (
        "discovered",
        "processed",
        "graded",
        "skipped",
        "pass",
        "fail",
        "inconclusive",
        "incompatible",
        "conflict",
    )
    if any(
        not isinstance(counts.get(name), int)
        or isinstance(counts[name], bool)
        or counts[name] < 0
        for name in required_counts
    ):
        raise QualificationError("regrade report has invalid completion counts")
    if (
        counts["discovered"] < 1
        or counts["processed"] != counts["discovered"]
        or counts["graded"] + counts["skipped"] != counts["discovered"]
        or counts["pass"] + counts["fail"] + counts["inconclusive"]
        != counts["graded"] + counts["skipped"]
        or counts["incompatible"] != 0
        or counts["conflict"] != 0
    ):
        raise QualificationError("regrade report did not account for the complete corpus")
    captures = report.get("captures")
    if not isinstance(captures, list) or len(captures) != counts["discovered"]:
        raise QualificationError("regrade report capture inventory is incomplete")
    seen_indexes: set[int] = set()
    seen_capture_ids: set[str] = set()
    status_counts = {"graded": 0, "skipped": 0}
    result_counts = {"PASS": 0, "FAIL": 0, "INCONCLUSIVE": 0}
    for item in captures:
        if not isinstance(item, dict):
            raise QualificationError("regrade report contains a malformed capture entry")
        capture_index = item.get("capture_index")
        if (
            not isinstance(capture_index, int)
            or isinstance(capture_index, bool)
            or capture_index < 1
        ):
            raise QualificationError("regrade report contains an invalid capture index")
        capture_id = str(item.get("capture_id") or "")
        if not _valid_digest(capture_id):
            raise QualificationError("regrade report contains an invalid capture ID")
        if capture_index in seen_indexes or capture_id in seen_capture_ids:
            raise QualificationError("regrade report contains duplicate capture ownership")
        seen_indexes.add(capture_index)
        seen_capture_ids.add(capture_id)

        result = str(item.get("result") or "")
        confidence = str(item.get("confidence_result") or "")
        if result not in result_counts:
            raise QualificationError("regrade report contains an invalid capture result")
        if confidence not in {"PASS", "INCONCLUSIVE"}:
            raise QualificationError("regrade report contains an invalid confidence result")
        if (result in {"PASS", "FAIL"} and confidence != "PASS") or (
            result == "INCONCLUSIVE" and confidence != "INCONCLUSIVE"
        ):
            raise QualificationError("regrade report result and confidence are inconsistent")
        if item.get("ownership_valid") is not True or item.get("diagnostic") != "":
            raise QualificationError("regrade report contains unowned or diagnostic capture evidence")

        grade = item.get("grade") if isinstance(item.get("grade"), dict) else {}
        status = str(grade.get("status") or "")
        grade_id = str(grade.get("grade_id") or "")
        expected_grade_id = hashlib.sha256(
            f"{capture_id}:{current_grader}".encode("ascii")
        ).hexdigest()
        if (
            status not in status_counts
            or grade.get("ownership_valid") is not True
            or grade.get("grader_fingerprint") != current_grader
            or not _valid_digest(grade_id)
            or grade_id != expected_grade_id
        ):
            raise QualificationError("regrade report contains invalid grade ownership")
        status_counts[status] += 1
        result_counts[result] += 1

    if (
        status_counts["graded"] != counts["graded"]
        or status_counts["skipped"] != counts["skipped"]
        or result_counts["PASS"] != counts["pass"]
        or result_counts["FAIL"] != counts["fail"]
        or result_counts["INCONCLUSIVE"] != counts["inconclusive"]
    ):
        raise QualificationError("regrade report entry aggregates do not match its counts")

    accepted_entries = [item for item in captures if item.get("capture_id") == accepted_capture_id]
    if len(accepted_entries) != 1:
        raise QualificationError("regrade report does not uniquely include the accepted capture")
    accepted = accepted_entries[0]
    grade = accepted["grade"]
    if (
        accepted.get("result") != "PASS"
        or accepted.get("confidence_result") != "PASS"
        or accepted.get("ownership_valid") is not True
        or grade.get("ownership_valid") is not True
        or grade.get("status") not in {"graded", "skipped"}
    ):
        raise QualificationError("regrade report did not confidently pass the accepted capture")


def _validate_camera_smoke(
    smoke: Mapping[str, Any],
    smoke_path: Path,
    *,
    current_grader: str,
) -> None:
    if (
        smoke.get("schema_version") != 2
        or smoke.get("kind") != "bench_camera_smoke"
        or smoke.get("result") != "PASS"
        or smoke.get("capture_result") != "CAPTURED"
        or smoke.get("grader_fingerprint") != current_grader
        or smoke.get("diagnostics") != []
    ):
        raise QualificationError("camera smoke is not an unambiguous current-grader PASS")
    camera = smoke.get("camera") if isinstance(smoke.get("camera"), dict) else {}
    profile = camera.get("profile") if isinstance(camera.get("profile"), dict) else {}
    if (
        camera.get("name") != EXPECTED_CAMERA_NAME
        or not isinstance(camera.get("device_index"), int)
        or isinstance(camera.get("device_index"), bool)
        or not profile
        or any(profile.get(field) != value for field, value in EXPECTED_CAMERA_PROFILE.items())
    ):
        raise QualificationError("camera smoke does not own its fixed camera profile")

    artifacts = smoke.get("artifacts")
    if not isinstance(artifacts, dict) or set(artifacts) != {
        "preflight",
        "session_start_still",
        "video",
        "camera_result",
    }:
        raise QualificationError("camera smoke does not own the complete live lifecycle")
    smoke_dir = smoke_path.resolve().parent
    preflight_path = _resolve_owned_file(smoke_dir, artifacts["preflight"], "smoke preflight")
    source_still_path = _resolve_owned_file(
        smoke_dir,
        artifacts["session_start_still"],
        "smoke session-start still",
    )
    video_path = _resolve_owned_file(smoke_dir, artifacts["video"], "smoke video")
    result_path = _resolve_owned_file(smoke_dir, artifacts["camera_result"], "smoke camera result")

    preflight = _read_json(preflight_path)
    preflight_camera = preflight.get("camera") if isinstance(preflight.get("camera"), dict) else {}
    preflight_source = (
        preflight.get("source_still") if isinstance(preflight.get("source_still"), dict) else {}
    )
    preflight_summary = smoke.get("preflight")
    if (
        preflight.get("schema_version") != 2
        or preflight.get("kind") != "bench_camera_preflight"
        or preflight.get("result") != "PASS"
        or preflight.get("diagnostics") != []
        or preflight_camera.get("name") != camera.get("name")
        or preflight_camera.get("device_index") != camera.get("device_index")
        or preflight_camera.get("profile") != profile
        or not isinstance(preflight_summary, dict)
        or preflight_summary.get("path") != artifacts["preflight"]["path"]
        or preflight_summary.get("sha256") != artifacts["preflight"]["sha256"]
    ):
        raise QualificationError("camera smoke preflight ownership is invalid")
    if (
        set(preflight_source) != {"name", "size_bytes", "sha256"}
        or preflight_source.get("name") != artifacts["session_start_still"]["path"]
        or preflight_source.get("size_bytes") != artifacts["session_start_still"]["size_bytes"]
        or preflight_source.get("sha256") != artifacts["session_start_still"]["sha256"]
        or source_still_path.name != artifacts["session_start_still"]["path"]
    ):
        raise QualificationError("camera smoke preflight source-still ownership is invalid")

    camera_result = _read_json(result_path)
    duration = camera_result.get("video_duration_seconds")
    profile_validation = (
        camera_result.get("profile_validation")
        if isinstance(camera_result.get("profile_validation"), dict)
        else {}
    )
    video_probe = (
        camera_result.get("video_probe")
        if isinstance(camera_result.get("video_probe"), dict)
        else {}
    )
    expected_width, expected_height = (
        int(value) for value in str(profile.get("video_size") or "0x0").split("x", 1)
    )
    if (
        camera_result.get("schema_version") != 2
        or camera_result.get("kind") != "bench_camera_evidence"
        or camera_result.get("result") != "CAPTURED"
        or camera_result.get("errors") != []
        or camera_result.get("camera_name") != camera.get("name")
        or camera_result.get("camera_device_index") != camera.get("device_index")
        or camera_result.get("profile") != profile
        or camera_result.get("expected_duration_seconds") != 1
        or profile_validation.get("result") != "PASS"
        or camera_result.get("video") != artifacts["video"]["path"]
        or camera_result.get("session_start_still") != artifacts["session_start_still"]["path"]
        or video_path.name != artifacts["video"]["path"]
        or not isinstance(duration, (int, float))
        or isinstance(duration, bool)
        or duration < 1.0
        or video_probe.get("width") != expected_width
        or video_probe.get("height") != expected_height
        or not isinstance(video_probe.get("average_frame_rate"), (int, float))
        or isinstance(video_probe.get("average_frame_rate"), bool)
        or float(video_probe["average_frame_rate"]) < float(profile.get("framerate") or 0) - 1.0
    ):
        raise QualificationError("camera smoke result does not own a completed short video lifecycle")


def build_grader_revalidation_record(
    prior_qualification_path: Path,
    regrade_report_path: Path,
    camera_smoke_path: Path,
    *,
    current_identity: Mapping[str, Any],
    current_traceability: Mapping[str, Any],
) -> dict[str, Any]:
    """Advance only the grader identity after complete regrade and live smoke evidence."""
    prior_qualification_path = prior_qualification_path.resolve()
    regrade_report_path = regrade_report_path.resolve()
    camera_smoke_path = camera_smoke_path.resolve()
    prior = _read_json(prior_qualification_path)
    validate_qualification_record(prior)
    if current_traceability.get("worktree_clean") is not True:
        raise QualificationError("repository is not clean at grader revalidation time")
    if current_identity.get("product_fingerprint") != prior.get("product_fingerprint"):
        raise QualificationError("product changed; grader-only revalidation is not allowed")
    if current_identity.get("hardware_scoring_fingerprint") != prior.get(
        "hardware_scoring_fingerprint"
    ):
        raise QualificationError(
            "hardware scoring changed; grader-only revalidation is not allowed"
        )
    if current_identity.get("scenario_fingerprints") != prior.get("scenario_fingerprints"):
        raise QualificationError("scenario changed; grader-only revalidation is not allowed")
    current_grader = str(current_identity.get("grader_fingerprint") or "")
    if not _valid_digest(current_grader) or current_grader == prior.get("grader_fingerprint"):
        raise QualificationError("grader-only revalidation requires one new valid grader identity")

    report = _read_json(regrade_report_path)
    _validate_regrade_report(
        report,
        current_grader=current_grader,
        accepted_capture_id=str(prior["evidence"]["replay_capture_id"]),
    )
    smoke = _read_json(camera_smoke_path)
    _validate_camera_smoke(smoke, camera_smoke_path, current_grader=current_grader)

    capture, grade = _accepted_capture_with_current_grade(prior, current_grader)
    prior_evidence = prior["evidence"]
    trace = {
        "repository_sha": str(current_traceability.get("repository_sha") or ""),
        "repository_ref": str(current_traceability.get("repository_ref") or ""),
        "worktree_clean": True,
    }
    record = {
        "schema_version": QUALIFICATION_SCHEMA_VERSION,
        "kind": "bench_qualification",
        "board_id": privacy_safe_identifier(
            prior.get("board_id", "release"), namespace="board"
        ),
        "product_fingerprint": prior["product_fingerprint"],
        "grader_fingerprint": current_grader,
        "hardware_scoring_fingerprint": prior["hardware_scoring_fingerprint"],
        "scenario_fingerprints": dict(prior["scenario_fingerprints"]),
        "traceability": trace,
        "evidence": {
            "kind": "grader_revalidation",
            "suites": list(REQUIRED_SUITES),
            "bench_result": prior_evidence["bench_result"],
            "bench_result_sha256": prior_evidence["bench_result_sha256"],
            "replay_capture_id": capture["capture_id"],
            "replay_grade_id": grade["grade_id"],
            "prior_qualification": str(prior_qualification_path),
            "prior_qualification_id": prior["qualification_id"],
            "prior_qualification_sha256": sha256_file(prior_qualification_path),
            "regrade_report": str(regrade_report_path),
            "regrade_report_sha256": sha256_file(regrade_report_path),
            "camera_smoke": str(camera_smoke_path),
            "camera_smoke_sha256": sha256_file(camera_smoke_path),
        },
    }
    record["qualification_id"] = hashlib.sha256(canonical_bytes(record)).hexdigest()
    validate_qualification_record(record)
    return record


def _linked_absolute_path(value: Any, label: str) -> Path:
    raw = str(value or "")
    path = Path(raw)
    if not raw or not path.is_absolute():
        raise QualificationError(f"{label} is not an absolute owned path")
    try:
        resolved = path.resolve(strict=True)
    except OSError as exc:
        raise QualificationError(f"{label} is missing") from exc
    if raw != str(resolved) or not resolved.is_file():
        raise QualificationError(f"{label} is not a normalized owned file path")
    return resolved


def validate_qualification_evidence(
    record: Mapping[str, Any],
    *,
    _visited_ids: frozenset[str] = frozenset(),
) -> None:
    """Rebuild a qualification from every immutable artifact it links."""
    validate_qualification_record(record)
    qualification_id = str(record["qualification_id"])
    if qualification_id in _visited_ids:
        raise QualificationError("qualification evidence contains a recursive prior-record link")
    visited = _visited_ids | {qualification_id}
    evidence = record["evidence"]
    bench_result_path = _linked_absolute_path(
        evidence.get("bench_result"),
        "accepted bench result",
    )
    if sha256_file(bench_result_path) != evidence.get("bench_result_sha256"):
        raise QualificationError("accepted bench result bytes changed")

    identity = {
        "product_fingerprint": record["product_fingerprint"],
        "grader_fingerprint": record["grader_fingerprint"],
        "hardware_scoring_fingerprint": record["hardware_scoring_fingerprint"],
        "scenario_fingerprints": dict(record["scenario_fingerprints"]),
    }
    trace = dict(record["traceability"])
    if evidence.get("kind") == "full_batch":
        rebuilt = build_qualification_record(
            bench_result_path,
            board_id=str(record.get("board_id") or "release"),
            current_identity=identity,
            current_traceability=trace,
        )
    else:
        prior_path = _linked_absolute_path(
            evidence.get("prior_qualification"),
            "prior qualification",
        )
        if sha256_file(prior_path) != evidence.get("prior_qualification_sha256"):
            raise QualificationError("prior qualification bytes changed")
        prior = _read_json(prior_path)
        validate_qualification_record(prior)
        if prior.get("qualification_id") != evidence.get("prior_qualification_id"):
            raise QualificationError("prior qualification ID does not match its link")
        validate_qualification_evidence(prior, _visited_ids=visited)

        report_path = _linked_absolute_path(
            evidence.get("regrade_report"),
            "regrade report",
        )
        smoke_path = _linked_absolute_path(
            evidence.get("camera_smoke"),
            "camera smoke",
        )
        if sha256_file(report_path) != evidence.get("regrade_report_sha256"):
            raise QualificationError("regrade report bytes changed")
        if sha256_file(smoke_path) != evidence.get("camera_smoke_sha256"):
            raise QualificationError("camera smoke bytes changed")
        rebuilt = build_grader_revalidation_record(
            prior_path,
            report_path,
            smoke_path,
            current_identity=identity,
            current_traceability=trace,
        )
    if rebuilt != dict(record):
        raise QualificationError("qualification record does not exactly match its linked evidence")


def write_qualification_record(path: Path, record: Mapping[str, Any]) -> None:
    """Publish an accepted qualification immutably and exclusively."""
    validate_qualification_record(record)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if _read_json(path) == dict(record):
            return
        raise QualificationError(f"immutable qualification already differs: {path}")
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(record, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        try:
            os.link(temporary, path)
        except FileExistsError:
            if _read_json(path) == dict(record):
                return
            raise QualificationError(f"immutable qualification already differs: {path}")
    finally:
        temporary.unlink(missing_ok=True)


def _commands(
    action: str,
    qualification_path: Path,
    board_id: str,
    current: Mapping[str, Any],
) -> list[str]:
    board_id = privacy_safe_identifier(board_id, namespace="board")
    parameters = current.get("scenario_parameters")
    if not isinstance(parameters, dict):
        parameters = {}
    duration = int(parameters.get("duration_seconds") or 300)
    replay_duration = int(parameters.get("replay_duration_seconds") or 300)
    segment = str(parameters.get("segment") or "last")
    blink = str(parameters.get("blink_profile") or "scenario")
    scenario_args = (
        f"--duration-seconds {duration} --replay-duration-seconds {replay_duration} "
        f"--segment {segment} --blink-profile {blink}"
    )
    if action == FULL_BATCH:
        qualification_target = (
            "<new-qualification.json>" if qualification_path.exists() else str(qualification_path)
        )
        return [
            f"./bench.sh --all --camera {scenario_args}",
            "python3 scripts/bench/bench_policy.py record-full "
            "--bench-result <new-run>/bench_result.json "
            f"--qualification {qualification_target}",
        ]
    if action == REGRADE_AND_SMOKE:
        return [
            "python3 scripts/bench/camera_regrade.py "
            f"--corpus-root .artifacts/bench/{board_id}/runs "
            "--report <new-regrade-report.json>",
            "python3 scripts/bench/camera_preflight.py "
            f"--out-dir .artifacts/bench/{board_id}/camera-smoke/<new-directory>",
            "python3 scripts/bench/bench_policy.py record-grader "
            f"--prior-qualification {qualification_path} "
            "--regrade-report <new-regrade-report.json> "
            "--camera-smoke <new-camera-smoke.json> "
            f"--qualification <new-qualification.json> {scenario_args}",
        ]
    return []


def build_plan(
    current: Mapping[str, Any],
    accepted: Mapping[str, Any] | None,
    *,
    invalid_reason: str,
    qualification_path: Path,
    board_id: str,
) -> dict[str, Any]:
    board_id = privacy_safe_identifier(board_id, namespace="board")
    action, reason = classify_policy(current, accepted, invalid_reason=invalid_reason)
    return {
        "schema_version": PLAN_SCHEMA_VERSION,
        "kind": "bench_qualification_plan",
        "action": action,
        "reason": reason,
        "current": dict(current),
        "accepted": dict(accepted) if accepted is not None else None,
        "repository_traceability_affects_action": False,
        "commands": _commands(action, qualification_path, board_id, current),
    }


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    plan = subparsers.add_parser("plan", help="print the minimum evidence action without running it")
    plan.add_argument("--qualification", required=True)
    plan.add_argument("--board-id", default="release")
    plan.add_argument("--duration-seconds", type=int, default=300)
    plan.add_argument("--replay-duration-seconds", type=int, default=300)
    plan.add_argument("--segment", default="last")
    plan.add_argument("--blink-profile", choices=("scenario", "steady", "stress"), default="scenario")
    plan.add_argument("--json", action="store_true")

    record = subparsers.add_parser("record-full", help="accept a clean, strict, full-batch PASS")
    record.add_argument("--bench-result", required=True)
    record.add_argument("--qualification", required=True)
    record.add_argument("--board-id", default="release")

    grader = subparsers.add_parser(
        "record-grader",
        help="advance only the grader after complete archive regrade and live smoke",
    )
    grader.add_argument("--prior-qualification", required=True)
    grader.add_argument("--regrade-report", required=True)
    grader.add_argument("--camera-smoke", required=True)
    grader.add_argument("--qualification", required=True)
    grader.add_argument("--duration-seconds", type=int, default=300)
    grader.add_argument("--replay-duration-seconds", type=int, default=300)
    grader.add_argument("--segment", default="last")
    grader.add_argument("--blink-profile", choices=("scenario", "steady", "stress"), default="scenario")
    return parser.parse_args(argv)


def _render_plan(plan: Mapping[str, Any]) -> str:
    lines = [f"bench action: {plan['action']}", f"reason: {plan['reason']}"]
    commands = plan.get("commands") if isinstance(plan.get("commands"), list) else []
    if commands:
        lines.append("next commands (not executed):")
        lines.extend(f"  {command}" for command in commands)
    else:
        lines.append("next commands: none")
    return "\n".join(lines) + "\n"


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    qualification_path = Path(args.qualification).resolve()
    if args.command == "plan":
        if args.duration_seconds < 1 or args.replay_duration_seconds < 1:
            raise SystemExit("bench durations must be positive")
        accepted, invalid_reason = load_qualification_record(
            qualification_path,
            validate_evidence=False,
        )
        current = current_policy_identity(
            ROOT,
            duration_seconds=args.duration_seconds,
            replay_duration_seconds=args.replay_duration_seconds,
            segment=args.segment,
            blink_profile=args.blink_profile,
        )
        plan = build_plan(
            current,
            accepted,
            invalid_reason=invalid_reason,
            qualification_path=qualification_path,
            board_id=args.board_id,
        )
        print(json.dumps(plan, indent=2, sort_keys=True) if args.json else _render_plan(plan), end="")
        return 0

    if args.command == "record-full":
        current = current_policy_identity(ROOT)
        record = build_qualification_record(
            Path(args.bench_result),
            board_id=args.board_id,
            current_identity=current,
            current_traceability=current["traceability"],
        )
        write_qualification_record(qualification_path, record)
        print(f"accepted full-batch qualification: {qualification_path}")
        return 0

    if args.duration_seconds < 1 or args.replay_duration_seconds < 1:
        raise SystemExit("bench durations must be positive")
    current = current_policy_identity(
        ROOT,
        duration_seconds=args.duration_seconds,
        replay_duration_seconds=args.replay_duration_seconds,
        segment=args.segment,
        blink_profile=args.blink_profile,
    )
    record = build_grader_revalidation_record(
        Path(args.prior_qualification),
        Path(args.regrade_report),
        Path(args.camera_smoke),
        current_identity=current,
        current_traceability=current["traceability"],
    )
    write_qualification_record(qualification_path, record)
    print(f"accepted grader-only qualification: {qualification_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
