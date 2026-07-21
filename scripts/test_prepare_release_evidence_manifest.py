#!/usr/bin/env python3
"""Regression tests for generated, commit-bound release evidence."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_release_evidence_manifest as evidence_check  # type: ignore  # noqa: E402
import prepare_release_evidence_manifest as prepare  # type: ignore  # noqa: E402


RELEASE_SHA = "0123456789abcdef0123456789abcdef01234567"
OTHER_SHA = "89abcdef0123456789abcdef0123456789abcdef"
POLICY = {
    "schema_version": 1,
    "accepted_risk": {
        "id": "obd-proxy-arbitration",
        "scope": ["OBD", "BLE proxy", "connection arbitration", "shared BLE scheduling"],
        "minimum_rationale_characters": 20,
    },
}
RATIONALE = "No representative qualification rig is available for this release."


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_bench(root: Path, *, git_sha: str = RELEASE_SHA, result: str = "PASS") -> Path:
    path = root / ".artifacts" / "bench" / "release" / "bench_result.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "kind": "bench_result",
                "git_sha": git_sha,
                "git_worktree_clean": True,
                "git_ref": "dev/release",
                "result": result,
                "windows": [
                    {
                        "suite": "core",
                        "result": result,
                        "git_sha": git_sha,
                        "git_ref": "dev/release",
                        "git_worktree_clean": True,
                    },
                    {
                        "suite": "display",
                        "result": result,
                        "git_sha": git_sha,
                        "git_ref": "dev/release",
                        "git_worktree_clean": True,
                    },
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return path


def generate(root: Path, bench: Path) -> dict[str, object]:
    return prepare.generate_manifest(
        root=root,
        release_git_sha=RELEASE_SHA,
        bench_result=bench,
        policy=POLICY,
        accepted_risk_rationale=RATIONALE,
        generated_at_utc="2026-07-20T18:00:00Z",
    )


def test_valid_policy_scoped_waiver_is_generated_and_validated(root: Path) -> None:
    payload = generate(root, write_bench(root))
    assert_true(payload["schema_version"] == 2, f"unexpected schema: {payload}")
    assert_true(payload["release_git_sha"] == RELEASE_SHA, f"SHA not bound: {payload}")
    waiver = payload["evidence"][1]
    assert_true(waiver["scope"] == POLICY["accepted_risk"]["scope"], "scope must come from policy")
    errors = evidence_check.validate_manifest(
        payload,
        expected_git_sha=RELEASE_SHA,
        root=root,
    )
    assert_true(errors == [], f"generated payload must validate: {errors}")


def test_checked_in_policy_is_valid(root: Path) -> None:
    del root
    payload = prepare.load_json(prepare.DEFAULT_POLICY, "release evidence policy")
    risk = prepare.validate_policy(payload)
    assert_true(risk["id"] == evidence_check.OBD_PROXY_ID, f"wrong policy: {risk}")


def test_atomic_writer_round_trips_exact_payload(root: Path) -> None:
    payload = generate(root, write_bench(root))
    output = root / ".artifacts" / "release_evidence" / "manifest.json"
    prepare.write_json_atomic(output, payload)
    assert_true(json.loads(output.read_text(encoding="utf-8")) == payload, "atomic write drift")
    leftovers = list(output.parent.glob(f".{output.name}.*.tmp"))
    assert_true(leftovers == [], f"temporary files leaked: {leftovers}")


def test_mismatched_or_short_bench_sha_is_rejected(root: Path) -> None:
    for git_sha in (OTHER_SHA, "0123456"):
        bench = write_bench(root, git_sha=git_sha)
        try:
            generate(root, bench)
        except prepare.EvidencePreparationError as exc:
            assert_true("Git SHA does not match" in str(exc), str(exc))
        else:
            raise AssertionError(f"bench SHA {git_sha} should be rejected")


def test_failed_bench_is_rejected(root: Path) -> None:
    try:
        generate(root, write_bench(root, result="FAIL"))
    except prepare.EvidencePreparationError as exc:
        assert_true("must be PASS" in str(exc), str(exc))
    else:
        raise AssertionError("failed bench should be rejected")


def test_dirty_collection_worktree_is_rejected(root: Path) -> None:
    bench = write_bench(root)
    payload = json.loads(bench.read_text(encoding="utf-8"))
    payload["git_worktree_clean"] = False
    bench.write_text(json.dumps(payload), encoding="utf-8")
    try:
        generate(root, bench)
    except prepare.EvidencePreparationError as exc:
        assert_true("clean collection worktree" in str(exc), str(exc))
    else:
        raise AssertionError("dirty bench collection should be rejected")


def test_rationale_is_a_human_decision_but_json_shape_is_not(root: Path) -> None:
    bench = write_bench(root)
    try:
        prepare.generate_manifest(
            root=root,
            release_git_sha=RELEASE_SHA,
            bench_result=bench,
            policy=POLICY,
            accepted_risk_rationale="too short",
        )
    except prepare.EvidencePreparationError as exc:
        assert_true("at least 20 characters" in str(exc), str(exc))
    else:
        raise AssertionError("short rationale should be rejected")


def test_artifacts_outside_the_repo_are_rejected(root: Path, outside: Path) -> None:
    outside_bench = write_bench(outside)
    try:
        generate(root, outside_bench)
    except prepare.EvidencePreparationError as exc:
        assert_true("inside the public repository" in str(exc), str(exc))
    else:
        raise AssertionError("outside artifact should be rejected")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="prepare_release_evidence_") as temporary:
        base = Path(temporary)
        cases = (
            test_valid_policy_scoped_waiver_is_generated_and_validated,
            test_checked_in_policy_is_valid,
            test_atomic_writer_round_trips_exact_payload,
            test_mismatched_or_short_bench_sha_is_rejected,
            test_failed_bench_is_rejected,
            test_dirty_collection_worktree_is_rejected,
            test_rationale_is_a_human_decision_but_json_shape_is_not,
        )
        for index, test in enumerate(cases):
            root = base / f"repo_{index}"
            root.mkdir()
            test(root)
        root = base / "inside"
        outside = base / "outside"
        root.mkdir()
        outside.mkdir()
        test_artifacts_outside_the_repo_are_rejected(root, outside)
    print("[release-evidence] generator regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
