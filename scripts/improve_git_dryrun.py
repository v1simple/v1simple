#!/usr/bin/env python3
"""Disposable Git-backed scenario for the Phase-B controller dry run.

Only Git is real in this scenario.  The repository, submitted candidate, and
evaluation worktree live below a temporary directory.  Firmware build, flash,
bench, serial, and camera operations remain deterministic simulations supplied
by :class:`DisposableGitDryRunAdapter`.

The helper deliberately has no dependency on ``improve.py``.  The controller
passes its plan, decision engine, and in-memory evidence-store factory into
``run_disposable_git_noop_scenario``.  Keeping that boundary explicit also
makes it straightforward for the regression test to prove that every child
process is Git.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
from typing import Any, Callable, Mapping, Sequence


BASE_TIMESTAMP = "2001-01-01T00:00:00Z"
CANDIDATE_TIMESTAMP = "2001-01-01T00:01:00Z"
REVERT_TIMESTAMP = "2001-01-01T00:02:00Z"
SOURCE_BRANCH = "main"
CANDIDATE_BRANCH = "submitted-candidate"
EVALUATION_BRANCH = "improve/dry-run/evaluation"
CANDIDATE_COMMENT = "// Phase-B dry-run: behavior intentionally unchanged."
BASE_SOURCE = "int render_period_us() {\n    return 16667;\n}\n"
DRY_RUN_CANDIDATE_PATH = "src/display_frequency.cpp"
UNSAFE_GIT_ENVIRONMENT = frozenset(
    {
        "GIT_DIR",
        "GIT_WORK_TREE",
        "GIT_COMMON_DIR",
        "GIT_INDEX_FILE",
        "GIT_OBJECT_DIRECTORY",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_NAMESPACE",
        "GIT_CONFIG_PARAMETERS",
        "GIT_CONFIG_SYSTEM",
        "GIT_CONFIG_GLOBAL",
    }
)


class DisposableGitError(RuntimeError):
    """A disposable Git invariant failed closed."""


def run_capture(
    argv: Sequence[str],
    *,
    cwd: Path,
    env: Mapping[str, str],
    allowed_returncodes: Sequence[int] = (0,),
) -> subprocess.CompletedProcess[str]:
    """Run one argv-only child and capture its output.

    This single process boundary is intentionally public within the module so
    the regression test can reject any command whose executable is not Git.
    """

    completed = subprocess.run(
        list(argv),
        cwd=cwd,
        env=dict(env),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode not in allowed_returncodes:
        detail = completed.stderr.strip() or completed.stdout.strip() or "no diagnostic"
        raise DisposableGitError(
            f"disposable Git command failed with exit {completed.returncode}: {detail}"
        )
    return completed


class _GitRunner:
    """Deterministic, local-only Git command runner."""

    def __init__(self, root: Path):
        self.root = root.resolve()
        self.actions = 0

    @staticmethod
    def _environment(timestamp: str) -> dict[str, str]:
        environment = {
            key: value
            for key, value in os.environ.items()
            if key not in UNSAFE_GIT_ENVIRONMENT
            and key != "GIT_CONFIG_COUNT"
            and not key.startswith(("GIT_CONFIG_KEY_", "GIT_CONFIG_VALUE_"))
        }
        environment.update(
            {
                "GIT_AUTHOR_NAME": "V1Simple Dry Run",
                "GIT_AUTHOR_EMAIL": "dry-run@example.invalid",
                "GIT_COMMITTER_NAME": "V1Simple Dry Run",
                "GIT_COMMITTER_EMAIL": "dry-run@example.invalid",
                "GIT_AUTHOR_DATE": timestamp,
                "GIT_COMMITTER_DATE": timestamp,
                "GIT_CONFIG_GLOBAL": os.devnull,
                "GIT_CONFIG_NOSYSTEM": "1",
                "LC_ALL": "C",
                "TZ": "UTC",
                "GIT_NO_REPLACE_OBJECTS": "1",
            }
        )
        return environment

    def command(
        self,
        cwd: Path,
        *arguments: str,
        timestamp: str = BASE_TIMESTAMP,
        allowed_returncodes: Sequence[int] = (0,),
    ) -> subprocess.CompletedProcess[str]:
        argv = [
            "git",
            "-c",
            "core.hooksPath=/dev/null",
            "-c",
            "commit.gpgSign=false",
            "-c",
            "tag.gpgSign=false",
            *arguments,
        ]
        self.actions += 1
        return run_capture(
            argv,
            cwd=cwd.resolve(),
            env=self._environment(timestamp),
            allowed_returncodes=allowed_returncodes,
        )

    def text(self, cwd: Path, *arguments: str) -> str:
        return self.command(cwd, *arguments).stdout.strip()


class _DisposableRepository:
    """Own and audit a real disposable source/evaluation Git pair."""

    def __init__(self, root: Path, validate_candidate_paths: Callable[[Sequence[str]], None]):
        self.root = root.resolve()
        self.source = self.root / "source"
        self.evaluation = self.root / "evaluation"
        self.git = _GitRunner(self.root)
        self.base_sha = ""
        self.candidate_sha = ""
        self.source_before: dict[str, str] = {}
        self.candidate_shape: dict[str, bool] = {}
        self.proof: dict[str, Any] | None = None
        self.validate_candidate_paths = validate_candidate_paths

    def create(self) -> dict[str, Any]:
        self.git.command(self.root, "init", "--initial-branch", SOURCE_BRANCH, str(self.source))
        source_file = self.source / DRY_RUN_CANDIDATE_PATH
        source_file.parent.mkdir(parents=True)
        source_file.write_text(BASE_SOURCE, encoding="utf-8")
        self.git.command(self.source, "add", "--", DRY_RUN_CANDIDATE_PATH)
        self.git.command(
            self.source,
            "commit",
            "-m",
            "dry-run base",
            timestamp=BASE_TIMESTAMP,
        )
        self.base_sha = self.git.text(self.source, "rev-parse", "HEAD")

        self.git.command(self.source, "switch", "-c", CANDIDATE_BRANCH)
        source_file.write_text(f"{CANDIDATE_COMMENT}\n{BASE_SOURCE}", encoding="utf-8")
        self.git.command(self.source, "add", "--", DRY_RUN_CANDIDATE_PATH)
        self.git.command(
            self.source,
            "commit",
            "-m",
            "dry-run behavior no-op candidate",
            timestamp=CANDIDATE_TIMESTAMP,
        )
        self.candidate_sha = self.git.text(self.source, "rev-parse", "HEAD")
        self.git.command(self.source, "switch", SOURCE_BRANCH)

        candidate_parents = self.git.text(
            self.source, "rev-list", "--parents", "-n", "1", self.candidate_sha
        ).split()
        changed_paths = self.git.text(
            self.source,
            "diff",
            "--name-only",
            self.base_sha,
            self.candidate_sha,
            "--",
        ).splitlines()
        self.validate_candidate_paths(changed_paths)
        candidate_text = self.git.text(
            self.source, "show", f"{self.candidate_sha}:{DRY_RUN_CANDIDATE_PATH}"
        )
        base_text = self.git.text(
            self.source, "show", f"{self.base_sha}:{DRY_RUN_CANDIDATE_PATH}"
        )
        comment_only = (
            changed_paths == [DRY_RUN_CANDIDATE_PATH]
            and candidate_text == f"{CANDIDATE_COMMENT}\n{base_text}"
        )
        single_parent = (
            len(candidate_parents) == 2
            and candidate_parents[0] == self.candidate_sha
            and candidate_parents[1] == self.base_sha
        )
        if not comment_only or not single_parent:
            raise DisposableGitError("disposable candidate is not the intended direct-child no-op")
        self.candidate_shape = {
            "direct_child_of_base": single_parent,
            "patch_is_comment_only": comment_only,
            "path_contract_validated": True,
        }

        self.source_before = self._source_snapshot()
        self.git.command(
            self.source,
            "worktree",
            "add",
            "-b",
            EVALUATION_BRANCH,
            str(self.evaluation),
            self.candidate_sha,
        )
        self._assert_evaluation_owned(candidate_expected=True)
        return {
            "candidate_patch": "comment_only",
            "candidate_parent": "pinned_base",
            "source_branch": SOURCE_BRANCH,
            "evaluation_branch": EVALUATION_BRANCH,
        }

    def _source_snapshot(self) -> dict[str, str]:
        return {
            "branch": self.git.text(self.source, "symbolic-ref", "--quiet", "--short", "HEAD"),
            "head": self.git.text(self.source, "rev-parse", "HEAD"),
            "status": self.git.text(
                self.source, "status", "--porcelain=v1", "--untracked-files=all"
            ),
            "candidate_ref": self.git.text(
                self.source, "rev-parse", f"refs/heads/{CANDIDATE_BRANCH}"
            ),
        }

    def _assert_evaluation_owned(self, *, candidate_expected: bool) -> None:
        if self.evaluation.resolve() != (self.root / "evaluation").resolve():
            raise DisposableGitError("evaluation worktree path escaped its disposable owner")
        toplevel = Path(self.git.text(self.evaluation, "rev-parse", "--show-toplevel")).resolve()
        common_dir = Path(
            self.git.text(
                self.evaluation,
                "rev-parse",
                "--path-format=absolute",
                "--git-common-dir",
            )
        ).resolve()
        branch = self.git.text(
            self.evaluation, "symbolic-ref", "--quiet", "--short", "HEAD"
        )
        status = self.git.text(
            self.evaluation, "status", "--porcelain=v1", "--untracked-files=all"
        )
        candidate_ref = self.git.text(
            self.source, "rev-parse", f"refs/heads/{CANDIDATE_BRANCH}"
        )
        head = self.git.text(self.evaluation, "rev-parse", "HEAD")
        if (
            toplevel != self.evaluation.resolve()
            or common_dir != (self.source / ".git").resolve()
            or branch != EVALUATION_BRANCH
            or status
            or candidate_ref != self.candidate_sha
            or (candidate_expected and head != self.candidate_sha)
        ):
            raise DisposableGitError("evaluation branch ownership changed before its revert")

    def finalize_and_prove(self) -> dict[str, Any]:
        self._assert_evaluation_owned(candidate_expected=True)
        self.git.command(
            self.evaluation,
            "revert",
            "--no-edit",
            self.candidate_sha,
            timestamp=REVERT_TIMESTAMP,
        )

        evaluation_branch = self.git.text(
            self.evaluation, "symbolic-ref", "--quiet", "--short", "HEAD"
        )
        evaluation_head = self.git.text(self.evaluation, "rev-parse", "HEAD")
        evaluation_tree = self.git.text(self.evaluation, "rev-parse", "HEAD^{tree}")
        base_tree = self.git.text(self.source, "rev-parse", f"{self.base_sha}^{{tree}}")
        evaluation_status = self.git.text(
            self.evaluation, "status", "--porcelain=v1", "--untracked-files=all"
        )
        source_after = self._source_snapshot()

        source_checks = {
            "branch_unchanged": source_after["branch"] == self.source_before["branch"] == SOURCE_BRANCH,
            "head_unchanged": source_after["head"] == self.source_before["head"] == self.base_sha,
            "status_unchanged": source_after["status"] == self.source_before["status"] == "",
        }
        candidate_checks = {
            "ref_unchanged": (
                source_after["candidate_ref"]
                == self.source_before["candidate_ref"]
                == self.candidate_sha
            ),
            **self.candidate_shape,
        }
        evaluation_checks = {
            "owned_branch": evaluation_branch == EVALUATION_BRANCH,
            "revert_commit_created": evaluation_head not in {self.base_sha, self.candidate_sha},
            "tree_matches_base": evaluation_tree == base_tree,
            "clean_after_revert": evaluation_status == "",
        }
        all_checks = [*source_checks.values(), *candidate_checks.values(), *evaluation_checks.values()]
        if not all(all_checks):
            raise DisposableGitError("disposable Git lifecycle did not preserve its ownership invariants")
        self.proof = {
            "result": "PASS",
            "real_git_actions": self.git.actions,
            "source_main": source_checks,
            "submitted_candidate": candidate_checks,
            "evaluation": evaluation_checks,
        }
        return dict(self.proof)


class DisposableGitDryRunAdapter:
    """Real disposable Git lifecycle with simulated product-side actions."""

    def __init__(
        self,
        repository: _DisposableRepository,
        runs_per_arm: int,
        no_change_exception: Callable[[str], BaseException],
    ):
        pattern = [100.0, 101.0, 99.0, 100.0, 100.0]
        values = [pattern[index % len(pattern)] for index in range(runs_per_arm)]
        self.values = {"baseline": list(values), "candidate": list(values)}
        self.repository = repository
        self.operations: list[str] = []
        self.context: dict[str, Any] = {}
        self.external_product_actions = 0
        self.no_change_exception = no_change_exception

    def prepare(self) -> dict[str, Any]:
        self.operations.append("prepare:disposable_git")
        git_context = self.repository.create()
        self.context = {
            "builds": "simulated_pass",
            "resources": "simulated_within_budget",
            "candidate_diff": "no-op",
            "disposable_git": git_context,
        }
        raise self.no_change_exception(
            "candidate source change produced the same firmware bytes as baseline"
        )

    def flash(self, arm: str, *, recovery: bool = False) -> None:
        self.operations.append(f"flash:{arm}:{'recovery' if recovery else 'normal'}:simulated")

    def check_stop(self) -> None:
        return None

    def collect(self, arm: str, arm_index: int, sequence: int) -> dict[str, Any]:
        self.operations.append(f"collect:{arm}:{arm_index}:simulated")
        value = self.values[arm][arm_index - 1]
        digest = hashlib.sha256(f"{arm}:{arm_index}:{sequence}:{value}".encode("ascii")).hexdigest()
        return {
            "arm": arm,
            "arm_index": arm_index,
            "sequence": sequence,
            "result": "PASS",
            "target_value": value,
            "bench_result": f"simulated/{arm}/{arm_index}/bench_result.json",
            "bench_result_sha256": digest,
            "metric_run_id": f"disposable-git-{arm}-{arm_index}",
            "qualification": f"simulated/{arm}/{arm_index}/qualification.json",
            "qualification_sha256": hashlib.sha256(
                f"{digest}:qualification".encode("ascii")
            ).hexdigest(),
            "simulated": True,
        }

    def validate_regressions(self, runs: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
        self.operations.append("validate_cross_arm_regressions:simulated")
        baseline_count = len([run for run in runs if run.get("arm") == "baseline"])
        candidate_count = len([run for run in runs if run.get("arm") == "candidate"])
        return [
            {
                "suite": suite,
                "candidate_arm_index": index,
                "result": "PASS",
                "baseline_count": baseline_count,
                "simulated": True,
            }
            for index in range(1, candidate_count + 1)
            for suite in ("core", "display", "replay")
        ]

    def finalize_evaluation(self) -> dict[str, Any]:
        self.operations.append("finalize_evaluation:real_disposable_git")
        proof = self.repository.finalize_and_prove()
        return {
            "message": "disposable evaluation branch reverted to the pinned base tree",
            "proof": proof,
        }


def run_disposable_git_noop_scenario(
    *,
    plan: Mapping[str, Any],
    execute_experiment: Callable[[Mapping[str, Any], Any, Any], Mapping[str, Any]],
    evidence_store_factory: Callable[[Path], Any],
    validate_candidate_paths: Callable[[Sequence[str]], None],
    no_change_exception: Callable[[str], BaseException],
) -> dict[str, Any]:
    """Run the controller's no-op rejection through a real disposable Git lifecycle."""

    with tempfile.TemporaryDirectory(prefix="v1simple-improve-git-dry-run-") as temporary:
        repository = _DisposableRepository(Path(temporary), validate_candidate_paths)
        adapter = DisposableGitDryRunAdapter(
            repository,
            int(plan["runs_per_arm"]),
            no_change_exception,
        )
        store = evidence_store_factory(repository.root / "evidence")
        decision = dict(execute_experiment(plan, adapter, store))
        proof = repository.proof
        if decision.get("result") != "REJECTED_NO_CHANGE" or not isinstance(proof, dict):
            raise DisposableGitError("controller did not complete the disposable no-op rejection")
        report = {
            "decision": decision,
            "operations": list(adapter.operations),
            "git": dict(proof),
            "file_evidence": {
                "event_count": store.state.get("event_count"),
                "terminal_anchor_matches": (
                    decision.get("last_event_sha256") == store.state.get("last_event_sha256")
                ),
                "decision_published": bool(getattr(store, "decision_path", Path()).is_file()),
            },
            "external_product_actions": adapter.external_product_actions,
        }
    return report
