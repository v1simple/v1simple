#!/usr/bin/env python3
"""Regression tests for scripts/check_release_evidence_manifest.py."""

from __future__ import annotations

import copy
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_obd_proxy_qualification as qualification  # type: ignore  # noqa: E402
import check_release_evidence_manifest as evidence  # type: ignore  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def has_error(errors: list[str], text: str) -> bool:
    return any(text in error for error in errors)


def write_bench_result(tmpdir: Path, result: str = "PASS") -> Path:
    tmpdir.mkdir(parents=True, exist_ok=True)
    artifact = tmpdir / "bench_result.json"
    artifact.write_text(json.dumps({"result": result}), encoding="utf-8")
    return artifact


def accepted_risk_item() -> dict[str, object]:
    return {
        "id": "obd-proxy-arbitration",
        "kind": "accepted-risk",
        "result": "ACCEPTED_RISK",
        "rationale": "No representative OBD/proxy hardware qualification rig exists.",
        "scope": ["OBD", "BLE proxy", "connection arbitration"],
    }


def write_hardware_qualification(tmpdir: Path) -> Path:
    profile = qualification.load_json(qualification.DEFAULT_PROFILE, "profile")
    case_ids, errors = qualification.required_case_ids(profile)
    assert_true(not errors, f"checked-in profile must be valid: {errors}")

    pack_dir = tmpdir / "obd_proxy_pack"
    logs_dir = pack_dir / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    cases = []
    for case_id in case_ids:
        log_path = logs_dir / f"{case_id}.log"
        log_path.write_text(f"PASS {case_id}\n", encoding="utf-8")
        cases.append({"id": case_id, "result": "PASS", "evidence_log": f"logs/{case_id}.log"})

    artifact = pack_dir / "qualification_result.json"
    artifact.write_text(
        json.dumps(
            {
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
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return artifact


def manifest(bench: Path, obd_item: dict[str, object] | None = None) -> dict[str, object]:
    rows: list[dict[str, object]] = [
        {"id": "core-display-bench", "kind": "bench", "result": "PASS", "artifact_path": str(bench)}
    ]
    if obd_item is not None:
        rows.append(obd_item)
    return {"schema_version": 1, "evidence": rows}


def test_valid_accepted_risk_manifest_passes(tmpdir: Path) -> None:
    payload = manifest(write_bench_result(tmpdir), accepted_risk_item())
    assert_true(evidence.validate_manifest(payload) == [], "valid structured waiver should pass")


def test_valid_hardware_qualification_manifest_passes(tmpdir: Path) -> None:
    bench = write_bench_result(tmpdir)
    artifact = write_hardware_qualification(tmpdir)
    item = {
        "id": "obd-proxy-arbitration",
        "kind": "hardware-qualification",
        "result": "PASS",
        "artifact_path": str(artifact),
    }
    assert_true(evidence.validate_manifest(manifest(bench, item)) == [], "valid hardware evidence should pass")


def test_missing_core_bench_is_rejected(tmpdir: Path) -> None:
    payload = {"schema_version": 1, "evidence": [accepted_risk_item()]}
    errors = evidence.validate_manifest(payload)
    assert_true(has_error(errors, "missing required evidence id: core-display-bench"), "core required")


def test_missing_obd_proxy_evidence_is_rejected(tmpdir: Path) -> None:
    errors = evidence.validate_manifest(manifest(write_bench_result(tmpdir)))
    assert_true(has_error(errors, "missing required evidence id: obd-proxy-arbitration"), "OBD required")


def test_core_bench_result_mismatch_is_preserved(tmpdir: Path) -> None:
    bench = write_bench_result(tmpdir, "FAIL")
    errors = evidence.validate_manifest(manifest(bench, accepted_risk_item()))
    assert_true(has_error(errors, "does not match"), "bench result mismatch must still fail")


def test_accepted_risk_requires_rationale_and_scope(tmpdir: Path) -> None:
    bench = write_bench_result(tmpdir)
    item = accepted_risk_item()
    item.pop("rationale")
    item.pop("scope")
    errors = evidence.validate_manifest(manifest(bench, item))
    assert_true(has_error(errors, "rationale is required"), "waiver rationale required")
    assert_true(has_error(errors, ".scope must be"), "waiver scope required")

    for invalid_scope in ("", [], ["OBD", ""], {}, 1):
        item = accepted_risk_item()
        item["scope"] = invalid_scope
        errors = evidence.validate_manifest(manifest(bench, item))
        assert_true(has_error(errors, ".scope must be"), f"reject scope {invalid_scope!r}")

    item = accepted_risk_item()
    item["scope"] = "OBD, BLE proxy, and connection arbitration"
    assert_true(evidence.validate_manifest(manifest(bench, item)) == [], "nonempty string scope passes")


def test_obd_proxy_kind_and_result_pairs_are_strict(tmpdir: Path) -> None:
    bench = write_bench_result(tmpdir)
    cases = (
        ({"id": "obd-proxy-arbitration", "kind": "device", "result": "PASS"}, ".kind must be"),
        (
            {
                "id": "obd-proxy-arbitration",
                "kind": "hardware-qualification",
                "result": "FAIL",
                "artifact_path": str(write_hardware_qualification(tmpdir)),
            },
            ".result must be PASS",
        ),
        (
            {
                "id": "obd-proxy-arbitration",
                "kind": "accepted-risk",
                "result": "PASS",
                "rationale": "Temporary waiver.",
                "scope": "OBD/proxy",
            },
            ".result must be ACCEPTED_RISK",
        ),
    )
    for item, expected in cases:
        errors = evidence.validate_manifest(manifest(bench, item))
        assert_true(has_error(errors, expected), f"must reject invalid pair: {item}")


def test_hardware_artifact_is_required_and_typed(tmpdir: Path) -> None:
    bench = write_bench_result(tmpdir)
    missing_item = {
        "id": "obd-proxy-arbitration",
        "kind": "hardware-qualification",
        "result": "PASS",
    }
    errors = evidence.validate_manifest(manifest(bench, missing_item))
    assert_true(has_error(errors, "artifact_path is required"), "hardware artifact required")

    invalid = tmpdir / "invalid_qualification.json"
    invalid.write_text("{}", encoding="utf-8")
    invalid_item = copy.deepcopy(missing_item)
    invalid_item["artifact_path"] = str(invalid)
    errors = evidence.validate_manifest(manifest(bench, invalid_item))
    assert_true(has_error(errors, ".artifact: schema_version must be 1"), "typed validation required")

    directory = tmpdir / "qualification_directory"
    directory.mkdir()
    directory_item = copy.deepcopy(missing_item)
    directory_item["artifact_path"] = str(directory)
    errors = evidence.validate_manifest(manifest(bench, directory_item))
    assert_true(has_error(errors, "artifact_path must be a file"), "artifact directory rejected")


def test_accepted_risk_must_not_carry_artifact(tmpdir: Path) -> None:
    bench = write_bench_result(tmpdir)
    item = accepted_risk_item()
    item["artifact_path"] = str(write_hardware_qualification(tmpdir))
    errors = evidence.validate_manifest(manifest(bench, item))
    assert_true(has_error(errors, "artifact_path must be omitted"), "waiver must not cite PASS artifact")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="release_evidence_manifest_") as tmp:
        tmpdir = Path(tmp)
        tests = (
            test_valid_accepted_risk_manifest_passes,
            test_valid_hardware_qualification_manifest_passes,
            test_missing_core_bench_is_rejected,
            test_missing_obd_proxy_evidence_is_rejected,
            test_core_bench_result_mismatch_is_preserved,
            test_accepted_risk_requires_rationale_and_scope,
            test_obd_proxy_kind_and_result_pairs_are_strict,
            test_hardware_artifact_is_required_and_typed,
            test_accepted_risk_must_not_carry_artifact,
        )
        for index, test in enumerate(tests):
            case_dir = tmpdir / f"case_{index}"
            case_dir.mkdir()
            test(case_dir)
    print("[release-evidence] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
