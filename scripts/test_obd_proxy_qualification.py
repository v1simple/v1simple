#!/usr/bin/env python3
"""Synthetic regression tests for check_obd_proxy_qualification.py."""

from __future__ import annotations

import copy
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_obd_proxy_qualification as qualification  # type: ignore  # noqa: E402

EXPECTED_CASE_IDS = [
    "obd_pair_connect_pid",
    "obd_power_cycle_reconnect",
    "v1_power_cycle_reconnect_with_obd",
    "proxy_phone_connect",
    "proxy_takeover_stops_obd",
    "obd_recovery_after_proxy_exit",
    "sustained_obd",
    "sustained_proxy",
]


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def has_error(errors: list[str], text: str) -> bool:
    return any(text in error for error in errors)


def make_valid_artifact(tmpdir: Path) -> tuple[Path, dict[str, object], dict[str, object]]:
    profile = qualification.load_json(qualification.DEFAULT_PROFILE, "profile")
    case_ids, profile_errors = qualification.required_case_ids(profile)
    assert_true(not profile_errors, f"checked-in profile must be valid: {profile_errors}")

    pack_dir = tmpdir / "pack"
    logs_dir = pack_dir / "logs"
    logs_dir.mkdir(parents=True)
    cases: list[dict[str, str]] = []
    for case_id in case_ids:
        log_path = logs_dir / f"{case_id}.log"
        log_path.write_text(f"PASS {case_id}\n", encoding="utf-8")
        cases.append({"id": case_id, "result": "PASS", "evidence_log": f"logs/{case_id}.log"})

    artifact_path = pack_dir / "qualification_result.json"
    payload: dict[str, object] = {
        "schema_version": 1,
        "profile_id": profile["profile_id"],
        "profile_version": profile["profile_version"],
        "result": "PASS",
        "release_git_sha": "0123456789abcdef0123456789abcdef01234567",
        "firmware_version": "1.0.1",
        "dut_board_id": "waveshare-349-dut-01",
        "rig_id": "obd-proxy-rig-01",
        "started_at_utc": "2026-07-13T20:00:00Z",
        "completed_at_utc": "2026-07-13T20:30:00Z",
        "safety": {"watchdog_resets": 0, "panics": 0},
        "cases": cases,
    }
    artifact_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return artifact_path, payload, profile


def validate(payload: object, artifact_path: Path, profile: object) -> list[str]:
    return qualification.validate_artifact(payload, artifact_path, profile)


def test_checked_in_profile_has_expected_cases(tmpdir: Path) -> None:
    del tmpdir
    profile = qualification.load_json(qualification.DEFAULT_PROFILE, "profile")
    case_ids, errors = qualification.required_case_ids(profile)
    assert_true(not errors, f"checked-in profile must be valid: {errors}")
    assert_true(case_ids == EXPECTED_CASE_IDS, f"required case contract drifted: {case_ids}")


def test_valid_artifact_passes(tmpdir: Path) -> None:
    artifact_path, payload, profile = make_valid_artifact(tmpdir)
    assert_true(validate(payload, artifact_path, profile) == [], "valid typed artifact should pass")
    assert_true(
        qualification.validate_artifact_file(artifact_path) == [],
        "valid artifact file should pass the public file validator",
    )


def test_schema_profile_and_result_are_required(tmpdir: Path) -> None:
    artifact_path, payload, profile = make_valid_artifact(tmpdir)
    mutations = (
        ("schema_version", 2, "schema_version must be 1"),
        ("profile_id", "wrong-profile", "profile_id must be"),
        ("profile_version", 2, "profile_version must be"),
        ("result", "FAIL", "result must be PASS"),
    )
    for field, value, expected in mutations:
        candidate = copy.deepcopy(payload)
        candidate[field] = value
        assert_true(has_error(validate(candidate, artifact_path, profile), expected), f"must reject {field}")


def test_release_identity_fields_are_required(tmpdir: Path) -> None:
    artifact_path, payload, profile = make_valid_artifact(tmpdir)
    for field in ("firmware_version", "dut_board_id", "rig_id"):
        candidate = copy.deepcopy(payload)
        candidate[field] = " "
        assert_true(has_error(validate(candidate, artifact_path, profile), f"{field} is required"), field)

    for invalid_sha in ("", "0123456", "g" * 40, True):
        candidate = copy.deepcopy(payload)
        candidate["release_git_sha"] = invalid_sha
        assert_true(has_error(validate(candidate, artifact_path, profile), "release_git_sha"), "invalid SHA")


def test_utc_window_is_valid_and_ordered(tmpdir: Path) -> None:
    artifact_path, payload, profile = make_valid_artifact(tmpdir)
    candidate = copy.deepcopy(payload)
    candidate["started_at_utc"] = "2026-07-13T20:00:00-04:00"
    assert_true(has_error(validate(candidate, artifact_path, profile), "ending in Z"), "start must be UTC Z")

    candidate = copy.deepcopy(payload)
    candidate["completed_at_utc"] = "not-a-timeZ"
    assert_true(has_error(validate(candidate, artifact_path, profile), "RFC3339 UTC"), "invalid time")

    candidate = copy.deepcopy(payload)
    candidate["started_at_utc"] = "2026-07-13Z"
    assert_true(has_error(validate(candidate, artifact_path, profile), "RFC3339 UTC"), "date-only time")

    candidate = copy.deepcopy(payload)
    candidate["completed_at_utc"] = "2026-07-13T19:59:59Z"
    assert_true(has_error(validate(candidate, artifact_path, profile), "must not precede"), "time order")


def test_safety_counters_must_be_integer_zero(tmpdir: Path) -> None:
    artifact_path, payload, profile = make_valid_artifact(tmpdir)
    for field in ("watchdog_resets", "panics"):
        for value in (1, -1, False, "0"):
            candidate = copy.deepcopy(payload)
            candidate["safety"][field] = value  # type: ignore[index]
            errors = validate(candidate, artifact_path, profile)
            assert_true(has_error(errors, f"safety.{field} must be integer 0"), f"reject {field}={value}")


def test_required_cases_are_exactly_once_and_pass(tmpdir: Path) -> None:
    artifact_path, payload, profile = make_valid_artifact(tmpdir)
    original_cases = payload["cases"]
    assert isinstance(original_cases, list)

    candidate = copy.deepcopy(payload)
    removed = candidate["cases"].pop()  # type: ignore[union-attr]
    assert_true(
        has_error(validate(candidate, artifact_path, profile), f"missing required case id: {removed['id']}"),
        "missing case must fail",
    )

    candidate = copy.deepcopy(payload)
    candidate["cases"].append(copy.deepcopy(candidate["cases"][0]))  # type: ignore[index,union-attr]
    assert_true(has_error(validate(candidate, artifact_path, profile), "duplicate case id"), "duplicate case")

    candidate = copy.deepcopy(payload)
    unknown = copy.deepcopy(candidate["cases"][0])  # type: ignore[index]
    unknown["id"] = "unknown_case"
    candidate["cases"].append(unknown)  # type: ignore[union-attr]
    assert_true(has_error(validate(candidate, artifact_path, profile), "unknown case id"), "unknown case")

    candidate = copy.deepcopy(payload)
    candidate["cases"][0]["result"] = "FAIL"  # type: ignore[index]
    assert_true(has_error(validate(candidate, artifact_path, profile), ".result must be PASS"), "failed case")


def test_evidence_logs_must_be_safe_relative_existing_files(tmpdir: Path) -> None:
    artifact_path, payload, profile = make_valid_artifact(tmpdir)
    invalid_paths = (
        ("/tmp/absolute.log", "relative POSIX path"),
        ("../outside.log", "path traversal"),
        ("logs/../../outside.log", "path traversal"),
        (r"C:\\temp\\windows.log", "relative POSIX path"),
        ("logs/missing.log", "does not exist"),
    )
    for raw, expected in invalid_paths:
        candidate = copy.deepcopy(payload)
        candidate["cases"][0]["evidence_log"] = raw  # type: ignore[index]
        assert_true(has_error(validate(candidate, artifact_path, profile), expected), f"reject {raw}")

    empty_log = artifact_path.parent / "logs" / "empty.log"
    empty_log.write_text("", encoding="utf-8")
    candidate = copy.deepcopy(payload)
    candidate["cases"][0]["evidence_log"] = "logs/empty.log"  # type: ignore[index]
    assert_true(has_error(validate(candidate, artifact_path, profile), "is empty"), "empty log")

    log_dir = artifact_path.parent / "logs" / "directory.log"
    log_dir.mkdir()
    candidate = copy.deepcopy(payload)
    candidate["cases"][0]["evidence_log"] = "logs/directory.log"  # type: ignore[index]
    assert_true(has_error(validate(candidate, artifact_path, profile), "is not a file"), "directory log")

    outside = tmpdir / "outside.log"
    outside.write_text("outside\n", encoding="utf-8")
    symlink = artifact_path.parent / "logs" / "escape.log"
    symlink.symlink_to(outside)
    candidate = copy.deepcopy(payload)
    candidate["cases"][0]["evidence_log"] = "logs/escape.log"  # type: ignore[index]
    assert_true(has_error(validate(candidate, artifact_path, profile), "resolves outside"), "symlink escape")


def test_file_loader_rejects_missing_and_invalid_json(tmpdir: Path) -> None:
    missing = tmpdir / "missing.json"
    assert_true(has_error(qualification.validate_artifact_file(missing), "artifact not found"), "missing")

    invalid = tmpdir / "invalid.json"
    invalid.write_text("{", encoding="utf-8")
    assert_true(has_error(qualification.validate_artifact_file(invalid), "not valid JSON"), "invalid JSON")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="obd_proxy_qualification_") as tmp:
        tmpdir = Path(tmp)
        tests = (
            test_checked_in_profile_has_expected_cases,
            test_valid_artifact_passes,
            test_schema_profile_and_result_are_required,
            test_release_identity_fields_are_required,
            test_utc_window_is_valid_and_ordered,
            test_safety_counters_must_be_integer_zero,
            test_required_cases_are_exactly_once_and_pass,
            test_evidence_logs_must_be_safe_relative_existing_files,
            test_file_loader_rejects_missing_and_invalid_json,
        )
        for index, test in enumerate(tests):
            case_dir = tmpdir / f"case_{index}"
            case_dir.mkdir()
            test(case_dir)
    print("[obd-proxy-qualification] regression tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
