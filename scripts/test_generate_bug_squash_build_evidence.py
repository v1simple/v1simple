#!/usr/bin/env python3
"""Regressions for the fail-closed bug-squash build evidence generator."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import generate_bug_squash_build_evidence as generator  # type: ignore  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_output_must_be_below_ignored_artifact_root(tmpdir: Path) -> None:
    artifact_root = tmpdir / ".artifacts"
    with mock.patch.object(generator, "ARTIFACT_ROOT", artifact_root):
        accepted = generator.require_ignored_output(str(artifact_root / "qualification"))
        assert_true(accepted.is_dir(), "ignored child is accepted")
        try:
            generator.require_ignored_output(str(tmpdir / "public-output"))
        except generator.GenerationError as exc:
            assert_true("ignored .artifacts" in str(exc), "outside path fails closed")
        else:
            raise AssertionError("outside artifact path was accepted")


def test_contract_preflight_requires_real_exact_environments(tmpdir: Path) -> None:
    del tmpdir
    contracts = [
        {
            "kind": "production",
            "implementation_status": "active",
            "blocker_code": None,
            "environment": "real-env",
            "build_command": ["pio", "run", "-e", "real-env"],
        }
    ]
    assert_true(
        generator.preflight_build_contracts(contracts, {"real-env"}) == contracts,
        "declared exact environment passes",
    )
    for candidate, declared, expected in (
        (contracts, set(), "not implemented"),
        (
            [dict(contracts[0], build_command=["pio", "run"])],
            {"real-env"},
            "exact environment",
        ),
        (contracts * 2, {"real-env"}, "unique"),
    ):
        try:
            generator.preflight_build_contracts(candidate, declared)
        except generator.GenerationError as exc:
            assert_true(expected in str(exc), expected)
        else:
            raise AssertionError(f"invalid preflight accepted: {expected}")


def test_current_profile_exposes_blocked_hil_control(tmpdir: Path) -> None:
    del tmpdir
    profile, errors = generator.qualification.load_pinned_profile()
    assert_true(profile is not None and errors == [], "pinned profile loads")
    assert profile is not None
    try:
        generator.preflight_build_contracts(
            profile["build_contracts"],
            generator.declared_platformio_environments(),
        )
    except generator.GenerationError as exc:
        assert_true(
            "hil-fault-control-not-implemented" in str(exc),
            "blocked HIL control is explicit",
        )
    else:
        raise AssertionError("unimplemented HIL environment was treated as buildable")


def test_evidence_index_entry_is_relative_and_content_bound(tmpdir: Path) -> None:
    output = tmpdir / "pack"
    binary = output / "binaries" / "production.bin"
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"image")
    entry = generator.evidence_entry(
        "firmware-production",
        "firmware-production",
        "binary",
        binary,
        output,
    )
    assert_true(entry["path"] == "binaries/production.bin", "relative path")
    original_sha = entry["sha256"]
    binary.write_bytes(b"different-image")
    assert_true(
        generator.qualification.file_sha256(binary) != original_sha,
        "content mutation changes digest",
    )
    json.dumps(entry)


def main() -> int:
    tests = (
        test_output_must_be_below_ignored_artifact_root,
        test_contract_preflight_requires_real_exact_environments,
        test_current_profile_exposes_blocked_hil_control,
        test_evidence_index_entry_is_relative_and_content_bound,
    )
    with tempfile.TemporaryDirectory(prefix="bug_squash_build_evidence_") as tmp:
        root = Path(tmp)
        for index, test in enumerate(tests):
            case_dir = root / f"case-{index}"
            case_dir.mkdir()
            test(case_dir)
    print(f"[bug-squash-build-evidence] {len(tests)} regression groups passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
