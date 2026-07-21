#!/usr/bin/env python3
"""Offline regression tests for check_ci_evidence.py."""

from __future__ import annotations

import copy
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any, Mapping

from check_ci_evidence import (
    CiEvidence,
    CiEvidenceError,
    GitHubApiClient,
    GitHubApiError,
    ReleaseChain,
    resolve_release_chain,
    verify_actions_evidence,
    write_github_outputs,
)


REPOSITORY = "v1simple/v1simple"
BASE_SHA = "1" * 40
WORKFLOW_ID = 312521447
RUN_ID = 29341406154
JOB_ID = 87113808995


def workflow(**changes: Any) -> dict[str, Any]:
    value: dict[str, Any] = {
        "id": WORKFLOW_ID,
        "name": "CI",
        "path": ".github/workflows/ci.yml",
        "state": "active",
    }
    value.update(changes)
    return value


def run_record(**changes: Any) -> dict[str, Any]:
    value: dict[str, Any] = {
        "id": RUN_ID,
        "name": "CI",
        "head_branch": "main",
        "head_sha": BASE_SHA,
        "path": ".github/workflows/ci.yml",
        "event": "push",
        "status": "completed",
        "conclusion": "success",
        "workflow_id": WORKFLOW_ID,
        "run_number": 18,
        "run_attempt": 1,
        "created_at": "2026-07-14T14:33:49Z",
        "html_url": f"https://github.com/{REPOSITORY}/actions/runs/{RUN_ID}",
        "repository": {"full_name": REPOSITORY},
        "head_repository": {"full_name": REPOSITORY},
    }
    value.update(changes)
    return value


def job_record(**changes: Any) -> dict[str, Any]:
    value: dict[str, Any] = {
        "id": JOB_ID,
        "name": "ci-test.sh",
        "workflow_name": "CI",
        "head_branch": "main",
        "head_sha": BASE_SHA,
        "run_id": RUN_ID,
        "run_attempt": 1,
        "status": "completed",
        "conclusion": "success",
        "steps": [
            {
                "name": "Run ci-test.sh",
                "status": "completed",
                "conclusion": "success",
            }
        ],
    }
    value.update(changes)
    return value


class FakeApi:
    def __init__(
        self,
        *,
        workflow_record: Mapping[str, Any] | Exception | None = None,
        run_snapshots: list[list[Mapping[str, Any]]] | None = None,
        detail_snapshots: list[Mapping[str, Any]] | None = None,
        jobs_by_attempt: Mapping[int, list[Mapping[str, Any]]] | None = None,
        job_pages: Mapping[tuple[int, int], list[Mapping[str, Any]]] | None = None,
    ) -> None:
        self.workflow_record = workflow_record or workflow()
        self.run_snapshots = run_snapshots or [[run_record()]]
        self.detail_snapshots = detail_snapshots or [run_record()]
        self.jobs_by_attempt = jobs_by_attempt or {1: [job_record()]}
        self.job_pages = job_pages or {}
        self.calls: list[tuple[str, dict[str, object]]] = []

    @staticmethod
    def _next(values: list[Any]) -> Any:
        if len(values) > 1:
            return values.pop(0)
        return values[0]

    def get(self, path: str, params: Mapping[str, object] | None = None) -> Mapping[str, Any]:
        request_params = dict(params or {})
        self.calls.append((path, request_params))
        if path.endswith("/actions/workflows/ci.yml"):
            if isinstance(self.workflow_record, Exception):
                raise self.workflow_record
            return copy.deepcopy(dict(self.workflow_record))
        if path.endswith(f"/actions/workflows/{WORKFLOW_ID}/runs"):
            snapshot = copy.deepcopy(self._next(self.run_snapshots))
            return {"total_count": len(snapshot), "workflow_runs": snapshot}
        if f"/actions/runs/{RUN_ID}/attempts/" in path and path.endswith("/jobs"):
            attempt = int(path.split("/attempts/", 1)[1].split("/", 1)[0])
            page = int(request_params.get("page", 1))
            if (attempt, page) in self.job_pages:
                items = copy.deepcopy(self.job_pages[(attempt, page)])
                total = sum(
                    len(values)
                    for (candidate_attempt, _), values in self.job_pages.items()
                    if candidate_attempt == attempt
                )
            else:
                items = copy.deepcopy(self.jobs_by_attempt.get(attempt, []))
                total = len(items)
            return {"total_count": total, "jobs": items}
        if path.endswith(f"/actions/runs/{RUN_ID}"):
            return copy.deepcopy(self._next(self.detail_snapshots))
        raise AssertionError(f"unexpected fake API request: {path} {request_params}")


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0
        self.sleeps: list[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        self.now += seconds


class TemporaryGitRepository:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release@example.test")
        (self.root / "include").mkdir()
        (self.root / "include/config.h").write_text(
            '#pragma once\n#define FIRMWARE_VERSION "1.0.1"\n', encoding="utf-8"
        )
        (self.root / "CHANGELOG.md").write_text(
            "# Changelog\n\n## [Unreleased]\n\n## [1.0.1] - 2026-07-13\n\nInitial.\n",
            encoding="utf-8",
        )
        (self.root / "source.txt").write_text("tested code\n", encoding="utf-8")
        self.commit_all("initial tested code")
        self.initial_sha = self.rev_parse("HEAD")

    def close(self) -> None:
        self.temporary.cleanup()

    def git(self, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", *args],
            cwd=self.root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=check,
        )

    def rev_parse(self, value: str) -> str:
        return self.git("rev-parse", value).stdout.strip()

    def commit_all(self, message: str) -> str:
        self.git("add", "--all")
        self.git("commit", "-q", "-m", message)
        return self.rev_parse("HEAD")

    def make_release(
        self,
        version: str,
        *,
        annotated: bool = True,
        marker: str = "Release-Run-ID: 12345",
        config_version: str | None = None,
        extra_config_change: bool = False,
        extra_change: bool = False,
        create_tag: bool = True,
    ) -> str:
        config = self.root / "include/config.h"
        old = config.read_text(encoding="utf-8")
        config.write_text(
            old.replace(
                old.split('FIRMWARE_VERSION "', 1)[1].split('"', 1)[0],
                config_version or version,
            ),
            encoding="utf-8",
        )
        if extra_config_change:
            config.write_text(
                config.read_text(encoding="utf-8")
                + "#define UNTESTED_FIRMWARE_CHANGE 1\n",
                encoding="utf-8",
            )
        changelog = self.root / "CHANGELOG.md"
        changelog.write_text(
            changelog.read_text(encoding="utf-8").replace(
                "## [Unreleased]\n",
                f"## [Unreleased]\n\n## [{version}] - 2026-07-14\n\nGenerated notes.\n",
                1,
            ),
            encoding="utf-8",
        )
        if extra_change:
            (self.root / "source.txt").write_text("unexpected code change\n", encoding="utf-8")
        sha = self.commit_all(f"chore(release): prepare v{version}")
        if create_tag:
            if annotated:
                self.git("tag", "-a", f"v{version}", "-m", f"Release v{version}\n\n{marker}")
            else:
                self.git("tag", f"v{version}")
        return sha


class ReleaseChainTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TemporaryGitRepository()

    def tearDown(self) -> None:
        self.repo.close()

    def resolve(self, sha: str, **changes: Any) -> ReleaseChain:
        return resolve_release_chain(self.repo.root, sha, main_ref="main", **changes)

    def test_non_release_commit_is_its_own_ci_sha(self) -> None:
        chain = self.resolve(self.repo.initial_sha)
        self.assertEqual(chain.base_sha, self.repo.initial_sha)
        self.assertEqual(chain.ci_sha, self.repo.initial_sha)
        self.assertEqual(chain.release_tags, ())

    def test_peels_one_valid_annotated_release_commit(self) -> None:
        release_sha = self.repo.make_release("1.0.2")
        chain = self.resolve(release_sha)
        self.assertEqual(chain.base_sha, release_sha)
        self.assertEqual(chain.ci_sha, self.repo.initial_sha)
        self.assertEqual(chain.release_tags, ("v1.0.2",))

    def test_peels_consecutive_valid_release_commits(self) -> None:
        self.repo.make_release("1.0.2")
        latest = self.repo.make_release("1.0.3", marker="Release-Run-ID: 23456")
        chain = self.resolve(latest)
        self.assertEqual(chain.ci_sha, self.repo.initial_sha)
        self.assertEqual(chain.release_tags, ("v1.0.3", "v1.0.2"))

    def test_rejects_lightweight_or_missing_release_tag(self) -> None:
        for annotated, create_tag, expected in (
            (False, True, "must be annotated"),
            (True, False, "found none"),
        ):
            with self.subTest(expected=expected):
                repo = TemporaryGitRepository()
                try:
                    release_sha = repo.make_release(
                        "1.0.2", annotated=annotated, create_tag=create_tag
                    )
                    with self.assertRaisesRegex(CiEvidenceError, expected):
                        resolve_release_chain(repo.root, release_sha, main_ref="main")
                finally:
                    repo.close()

    def test_rejects_missing_or_duplicate_run_marker(self) -> None:
        for marker in (
            "No run marker",
            "Release-Run-ID: 0",
            "Release-Run-ID: 12345\nRelease-Run-ID: 23456",
        ):
            with self.subTest(marker=marker):
                repo = TemporaryGitRepository()
                try:
                    release_sha = repo.make_release("1.0.2", marker=marker)
                    with self.assertRaisesRegex(CiEvidenceError, "exactly one valid"):
                        resolve_release_chain(repo.root, release_sha, main_ref="main")
                finally:
                    repo.close()

    def test_rejects_extra_file_config_content_or_wrong_firmware_version(self) -> None:
        cases = (
            ({"extra_change": True}, "must modify exactly"),
            ({"extra_config_change": True}, "must change only the FIRMWARE_VERSION"),
            ({"config_version": "9.9.9"}, "release config defines FIRMWARE_VERSION"),
        )
        for kwargs, expected in cases:
            with self.subTest(expected=expected):
                repo = TemporaryGitRepository()
                try:
                    release_sha = repo.make_release("1.0.2", **kwargs)
                    with self.assertRaisesRegex(CiEvidenceError, expected):
                        resolve_release_chain(repo.root, release_sha, main_ref="main")
                finally:
                    repo.close()

    def test_rejects_ambiguous_semver_tags(self) -> None:
        release_sha = self.repo.make_release("1.0.2")
        self.repo.git("tag", "-a", "v2.0.0", "-m", "Release alias")
        with self.assertRaisesRegex(CiEvidenceError, "only annotated tag"):
            self.resolve(release_sha)

    def test_rejects_base_outside_main_history(self) -> None:
        self.repo.git("switch", "-q", "-c", "side")
        (self.repo.root / "source.txt").write_text("side\n", encoding="utf-8")
        side_sha = self.repo.commit_all("side commit")
        self.repo.git("switch", "-q", "main")
        with self.assertRaisesRegex(CiEvidenceError, "not in main history"):
            self.resolve(side_sha)

    def test_enforces_release_chain_limit(self) -> None:
        release_sha = self.repo.make_release("1.0.2")
        with self.assertRaisesRegex(CiEvidenceError, "exceeds configured limit"):
            self.resolve(release_sha, max_release_commits=0)


class ActionsEvidenceTests(unittest.TestCase):
    def verify(self, api: FakeApi, **changes: Any) -> CiEvidence:
        return verify_actions_evidence(
            api,
            repository=REPOSITORY,
            ci_sha=BASE_SHA,
            wait_seconds=0,
            **changes,
        )

    def test_accepts_exact_push_run_job_and_step(self) -> None:
        evidence = self.verify(FakeApi())
        self.assertEqual(evidence.workflow_id, WORKFLOW_ID)
        self.assertEqual(evidence.run_id, RUN_ID)
        self.assertEqual(evidence.run_attempt, 1)
        self.assertEqual(evidence.job_id, JOB_ID)
        self.assertEqual(evidence.event, "push")

    def test_binds_evidence_to_expected_triggering_run(self) -> None:
        expected = run_record(run_number=17)
        newer_same_sha = run_record(id=RUN_ID + 1, run_number=18)
        evidence = self.verify(
            FakeApi(run_snapshots=[[expected, newer_same_sha]]),
            expected_run_id=RUN_ID,
        )
        self.assertEqual(evidence.run_id, RUN_ID)

    def test_rejects_wrong_or_invalid_expected_triggering_run(self) -> None:
        with self.assertRaisesRegex(CiEvidenceError, "does not match"):
            self.verify(FakeApi(), expected_run_id=RUN_ID + 1)
        with self.assertRaisesRegex(CiEvidenceError, "positive integer"):
            self.verify(FakeApi(), expected_run_id=0)

    def test_accepts_exact_main_workflow_dispatch_as_recovery_evidence(self) -> None:
        dispatched = run_record(event="workflow_dispatch")
        evidence = self.verify(
            FakeApi(run_snapshots=[[dispatched]], detail_snapshots=[dispatched])
        )
        self.assertEqual(evidence.event, "workflow_dispatch")

    def test_rejects_wrong_or_inactive_workflow_metadata(self) -> None:
        cases = (
            ({"name": "Almost CI"}, "name"),
            ({"path": ".github/workflows/other.yml"}, "path"),
            ({"state": "disabled_manually"}, "not active"),
        )
        for changes, expected in cases:
            with self.subTest(changes=changes):
                with self.assertRaisesRegex(CiEvidenceError, expected):
                    self.verify(FakeApi(workflow_record=workflow(**changes)))

    def test_rejects_run_with_wrong_authoritative_identity(self) -> None:
        cases = (
            {"workflow_id": 99},
            {"name": "Other"},
            {"path": ".github/workflows/other.yml"},
            {"event": "pull_request"},
            {"head_branch": "feature"},
            {"head_sha": "2" * 40},
            {"repository": {"full_name": "other/repo"}},
            {"head_repository": {"full_name": "fork/repo"}},
        )
        for changes in cases:
            with self.subTest(changes=changes):
                candidate = run_record(**changes)
                with self.assertRaisesRegex(CiEvidenceError, "no authoritative CI run"):
                    self.verify(FakeApi(run_snapshots=[[candidate]]))

    def test_latest_failure_overrides_older_success(self) -> None:
        older = run_record(id=RUN_ID - 1, run_number=17)
        failed = run_record(
            run_number=18,
            status="completed",
            conclusion="failure",
            html_url="https://example.test/failed",
        )
        with self.assertRaisesRegex(CiEvidenceError, "concluded failure"):
            self.verify(FakeApi(run_snapshots=[[older, failed]]))

    def test_uses_latest_successful_rerun_attempt(self) -> None:
        rerun = run_record(run_attempt=2)
        rerun_job = job_record(run_attempt=2)
        evidence = self.verify(
            FakeApi(
                run_snapshots=[[rerun]],
                detail_snapshots=[rerun],
                jobs_by_attempt={2: [rerun_job]},
            )
        )
        self.assertEqual(evidence.run_attempt, 2)

    def test_rejects_failed_latest_rerun_attempt(self) -> None:
        failed = run_record(run_attempt=2, conclusion="failure")
        with self.assertRaisesRegex(CiEvidenceError, "attempt 2 concluded failure"):
            self.verify(FakeApi(run_snapshots=[[failed]]))

    def test_waits_for_indexing_and_queued_run_then_succeeds(self) -> None:
        clock = FakeClock()
        queued = run_record(status="queued", conclusion=None)
        api = FakeApi(run_snapshots=[[], [queued], [run_record()]])
        evidence = verify_actions_evidence(
            api,
            repository=REPOSITORY,
            ci_sha=BASE_SHA,
            wait_seconds=10,
            poll_seconds=2,
            clock=clock.monotonic,
            sleeper=clock.sleep,
        )
        self.assertEqual(evidence.run_id, RUN_ID)
        self.assertEqual(clock.sleeps, [2, 2])

    def test_times_out_waiting_for_queued_run(self) -> None:
        clock = FakeClock()
        queued = run_record(status="in_progress", conclusion=None)
        with self.assertRaisesRegex(CiEvidenceError, "is in_progress"):
            verify_actions_evidence(
                FakeApi(run_snapshots=[[queued]]),
                repository=REPOSITORY,
                ci_sha=BASE_SHA,
                wait_seconds=3,
                poll_seconds=2,
                clock=clock.monotonic,
                sleeper=clock.sleep,
            )
        self.assertEqual(clock.sleeps, [2, 1])

    def test_rejects_missing_duplicate_or_failed_authoritative_job(self) -> None:
        cases = (
            ([], "found 0"),
            ([job_record(), job_record(id=JOB_ID + 1)], "found 2"),
            ([job_record(conclusion="failure")], "unexpected conclusion"),
            ([job_record(head_sha="2" * 40)], "unexpected head_sha"),
            ([job_record(run_attempt=2)], "unexpected run_attempt"),
        )
        for jobs, expected in cases:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(CiEvidenceError, expected):
                    self.verify(FakeApi(jobs_by_attempt={1: jobs}))

    def test_rejects_missing_duplicate_or_failed_gate_step(self) -> None:
        successful_step = {
            "name": "Run ci-test.sh",
            "status": "completed",
            "conclusion": "success",
        }
        cases = (
            ([], "found 0"),
            ([successful_step, successful_step], "found 2"),
            ([{**successful_step, "conclusion": "failure"}], "completed successfully"),
            ([{**successful_step, "status": "in_progress"}], "completed successfully"),
        )
        for steps, expected in cases:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(CiEvidenceError, expected):
                    self.verify(FakeApi(jobs_by_attempt={1: [job_record(steps=steps)]}))

    def test_detects_run_attempt_change_during_verification(self) -> None:
        first = run_record(run_attempt=1)
        changed = run_record(run_attempt=2)
        with self.assertRaisesRegex(CiEvidenceError, "changed attempt during verification"):
            self.verify(FakeApi(detail_snapshots=[first, changed]))

    def test_repolls_when_a_new_attempt_starts_during_verification(self) -> None:
        clock = FakeClock()
        attempt_one = run_record(run_attempt=1)
        attempt_two_running = run_record(
            run_attempt=2,
            status="in_progress",
            conclusion=None,
        )
        attempt_two_success = run_record(run_attempt=2)
        api = FakeApi(
            run_snapshots=[[attempt_one], [attempt_two_success]],
            detail_snapshots=[
                attempt_one,
                attempt_two_running,
                attempt_two_success,
                attempt_two_success,
            ],
            jobs_by_attempt={
                1: [job_record(run_attempt=1)],
                2: [job_record(run_attempt=2)],
            },
        )
        evidence = verify_actions_evidence(
            api,
            repository=REPOSITORY,
            ci_sha=BASE_SHA,
            wait_seconds=5,
            poll_seconds=1,
            clock=clock.monotonic,
            sleeper=clock.sleep,
        )
        self.assertEqual(evidence.run_attempt, 2)
        self.assertEqual(clock.sleeps, [1])

    def test_paginates_attempt_jobs(self) -> None:
        filler = [job_record(id=index + 1, name=f"other-{index}") for index in range(100)]
        api = FakeApi(
            job_pages={(1, 1): filler, (1, 2): [job_record()]},
        )
        evidence = self.verify(api)
        self.assertEqual(evidence.job_id, JOB_ID)
        job_calls = [params for path, params in api.calls if path.endswith("/jobs")]
        self.assertEqual([params["page"] for params in job_calls], [1, 2])

    def test_propagates_action_permission_error(self) -> None:
        error = GitHubApiError("GITHUB_TOKEN must have actions: read")
        with self.assertRaisesRegex(GitHubApiError, "actions: read"):
            self.verify(FakeApi(workflow_record=error))


class OutputAndClientTests(unittest.TestCase):
    def test_writes_stable_single_line_github_outputs(self) -> None:
        chain = ReleaseChain(BASE_SHA, "2" * 40, ("v1.0.3", "v1.0.2"))
        evidence = CiEvidence(
            workflow_id=WORKFLOW_ID,
            run_id=RUN_ID,
            run_attempt=2,
            job_id=JOB_ID,
            event="push",
            url="https://github.com/v1simple/v1simple/actions/runs/29341406154",
        )
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "github-output"
            write_github_outputs(output, chain, evidence)
            values = dict(
                line.split("=", 1)
                for line in output.read_text(encoding="utf-8").splitlines()
            )
        self.assertEqual(values["base_sha"], BASE_SHA)
        self.assertEqual(values["ci_sha"], "2" * 40)
        self.assertEqual(values["ci_run_attempt"], "2")
        self.assertEqual(values["release_commits_peeled"], "2")
        self.assertEqual(values["release_tags"], "v1.0.3,v1.0.2")

    def test_client_requires_https_repository_and_actions_token(self) -> None:
        cases = (
            (
                {
                    "api_url": "http://api.github.test",
                    "repository": REPOSITORY,
                    "token": "token",
                },
                "HTTPS",
            ),
            (
                {
                    "api_url": "https://api.github.test",
                    "repository": "invalid",
                    "token": "token",
                },
                "invalid GitHub",
            ),
            (
                {
                    "api_url": "https://api.github.test",
                    "repository": REPOSITORY,
                    "token": "",
                },
                "actions: read",
            ),
            (
                {
                    "api_url": "https://api.github.test",
                    "repository": REPOSITORY,
                    "token": "bad token",
                },
                "actions: read",
            ),
        )
        for kwargs, expected in cases:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(CiEvidenceError, expected):
                    GitHubApiClient(**kwargs)


if __name__ == "__main__":
    # Make accidental network use obvious during this test suite.
    os.environ.pop("GITHUB_TOKEN", None)
    unittest.main(verbosity=2)
