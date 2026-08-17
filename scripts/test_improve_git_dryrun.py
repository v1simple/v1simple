#!/usr/bin/env python3
"""Focused regression test for the disposable-Git Phase-B dry run."""

from __future__ import annotations

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]

import improve  # noqa: E402
import improve_git_dryrun  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_scenario() -> dict:
    return improve_git_dryrun.run_disposable_git_noop_scenario(
        plan=improve.dry_plan(),
        execute_experiment=improve.execute_experiment,
        validate_candidate_paths=improve.validate_candidate_paths,
        no_change_exception=improve.NoChangeCandidate,
        evidence_store_factory=lambda path: improve.FileEvidenceStore(
            path, clock=lambda: "2001-01-01T00:03:00Z"
        ),
    )


def test_disposable_git_scenario_is_deterministic_and_git_only() -> None:
    original = improve_git_dryrun.run_capture
    observed: list[tuple[str, ...]] = []

    def git_only(
        argv: list[str],
        *,
        cwd: Path,
        env: dict[str, str],
        allowed_returncodes: tuple[int, ...] = (0,),
    ) -> subprocess.CompletedProcess[str]:
        assert_true(bool(argv) and argv[0] == "git", f"non-Git dry-run process: {argv}")
        observed.append(tuple(argv))
        return original(
            argv,
            cwd=cwd,
            env=env,
            allowed_returncodes=allowed_returncodes,
        )

    improve_git_dryrun.run_capture = git_only  # type: ignore[assignment]
    try:
        first = run_scenario()
        first_action_count = len(observed)
        observed.clear()
        second = run_scenario()
        second_action_count = len(observed)
    finally:
        improve_git_dryrun.run_capture = original  # type: ignore[assignment]

    assert_true(first == second, "disposable Git dry-run output is not deterministic")
    assert_true(first_action_count > 0, "disposable Git dry run did not invoke Git")
    assert_true(first_action_count == second_action_count, "Git action count changed between runs")
    assert_true(first["external_product_actions"] == 0, "dry run touched a product-side tool")
    assert_true(first["decision"]["result"] == "REJECTED_NO_CHANGE", "no-op was not rejected")
    assert_true(first["git"]["result"] == "PASS", "Git proof failed")
    assert_true(first["file_evidence"]["decision_published"], "file decision was not published")
    assert_true(
        first["file_evidence"]["terminal_anchor_matches"],
        "file decision does not anchor its terminal journal event",
    )
    assert_true(
        first["git"]["real_git_actions"] == first_action_count,
        "reported Git action count does not cite the actual commands",
    )
    assert_true(all(first["git"]["source_main"].values()), "source main changed")
    assert_true(
        all(first["git"]["submitted_candidate"].values()),
        "submitted candidate ref or no-op shape changed",
    )
    assert_true(all(first["git"]["evaluation"].values()), "evaluation finalization proof failed")
    assert_true(
        first["operations"][-1] == "finalize_evaluation:real_disposable_git",
        "decision engine did not finish through real evaluation finalization",
    )


def main() -> int:
    test_disposable_git_scenario_is_deterministic_and_git_only()
    print("improve disposable Git dry-run tests: PASS (1 test)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
