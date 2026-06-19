#!/usr/bin/env python3
"""Regression tests for scripts/check_release_evidence_manifest.py."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_release_evidence_manifest as evidence  # type: ignore  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_valid_manifest_accepts_core_bench(tmpdir: Path) -> None:
    artifact = tmpdir / "bench_result.json"
    artifact.write_text(json.dumps({"result": "PASS"}), encoding="utf-8")
    payload = {
        "schema_version": 1,
        "evidence": [
            {"id": "core-display-bench", "kind": "bench", "result": "PASS", "artifact_path": str(artifact)}
        ],
    }

    assert_true(evidence.validate_manifest(payload) == [], "valid core bench evidence should pass")


def test_missing_core_bench_is_rejected(tmpdir: Path) -> None:
    artifact = tmpdir / "radio.json"
    artifact.write_text("{}", encoding="utf-8")
    payload = {
        "schema_version": 1,
        "evidence": [
            {"id": "radio-coexistence", "kind": "device", "result": "PASS", "artifact_path": str(artifact)}
        ],
    }

    errors = evidence.validate_manifest(payload)
    assert_true(any("core-display-bench" in error for error in errors), "core bench evidence is required")


def test_result_mismatch_is_rejected(tmpdir: Path) -> None:
    artifact = tmpdir / "bench_result.json"
    artifact.write_text(json.dumps({"result": "FAIL"}), encoding="utf-8")
    payload = {
        "schema_version": 1,
        "evidence": [
            {"id": "core-display-bench", "kind": "bench", "result": "PASS", "artifact_path": str(artifact)}
        ],
    }

    errors = evidence.validate_manifest(payload)
    assert_true(any("does not match" in error for error in errors), "bench result mismatch must fail")


def test_accepted_risk_waiver_requires_rationale(tmpdir: Path) -> None:
    artifact = tmpdir / "bench_result.json"
    artifact.write_text(json.dumps({"result": "PASS"}), encoding="utf-8")
    payload = {
        "schema_version": 1,
        "evidence": [
            {"id": "core-display-bench", "kind": "bench", "result": "PASS", "artifact_path": str(artifact)},
            {"id": "obd-proxy-arbitration", "kind": "accepted-risk", "result": "ACCEPTED_RISK"},
        ],
    }

    errors = evidence.validate_manifest(payload)
    assert_true(
        any("rationale is required" in error for error in errors),
        "accepted-risk waivers must explain the release decision",
    )


def test_accepted_risk_waiver_allows_missing_artifact(tmpdir: Path) -> None:
    artifact = tmpdir / "bench_result.json"
    artifact.write_text(json.dumps({"result": "PASS"}), encoding="utf-8")
    payload = {
        "schema_version": 1,
        "evidence": [
            {"id": "core-display-bench", "kind": "bench", "result": "PASS", "artifact_path": str(artifact)},
            {
                "id": "obd-proxy-arbitration",
                "kind": "accepted-risk",
                "result": "ACCEPTED_RISK",
                "rationale": "No representative OBD/proxy hardware qualification rig exists.",
            },
        ],
    }

    assert_true(evidence.validate_manifest(payload) == [], "accepted-risk waiver with rationale should pass")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="release_evidence_manifest_") as tmp:
        tmpdir = Path(tmp)
        test_valid_manifest_accepts_core_bench(tmpdir)
        test_missing_core_bench_is_rejected(tmpdir)
        test_result_mismatch_is_rejected(tmpdir)
        test_accepted_risk_waiver_requires_rationale(tmpdir)
        test_accepted_risk_waiver_allows_missing_artifact(tmpdir)
    print("[release-evidence] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
