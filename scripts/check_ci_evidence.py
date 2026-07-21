#!/usr/bin/env python3
"""Verify authoritative GitHub Actions CI evidence for a release base.

Release commits created by ``release.yml`` are pushed with ``GITHUB_TOKEN``.
GitHub intentionally does not start another push workflow for those commits, so
the current main commit may not have its own CI run.  This helper safely peels
consecutive release-only commits and verifies the latest authoritative CI run
for the first code-bearing ancestor.

The token named by ``--token-env`` is used only for GitHub REST API reads.  In
GitHub Actions it must have the repository ``actions: read`` permission.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping, Protocol, Sequence

from check_release_config_change import (
    ReleaseConfigChangeError,
    validate_release_config_change,
)


SHA_RE = re.compile(r"^[0-9a-f]{40}$")
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
RELEASE_SUBJECT_RE = re.compile(
    r"^chore\(release\): prepare v(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
)
SEMVER_TAG_RE = re.compile(
    r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
)
RUN_ID_RE = re.compile(r"^Release-Run-ID: ([1-9][0-9]*)$")
EXPECTED_RELEASE_CHANGES = {
    ("M", "CHANGELOG.md"),
    ("M", "include/config.h"),
}
DEFAULT_API_VERSION = "2022-11-28"
DEFAULT_WORKFLOW_FILE = "ci.yml"
DEFAULT_WORKFLOW_PATH = ".github/workflows/ci.yml"
DEFAULT_WORKFLOW_NAME = "CI"
DEFAULT_JOB_NAME = "ci-test.sh"
DEFAULT_STEP_NAME = "Run ci-test.sh"
ALLOWED_EVENTS = frozenset({"push", "workflow_dispatch"})


class CiEvidenceError(RuntimeError):
    """Raised when release or Actions evidence cannot be trusted."""


class GitHubApiError(CiEvidenceError):
    """Raised for an unsuccessful or malformed GitHub API response."""


@dataclass(frozen=True)
class ReleaseChain:
    base_sha: str
    ci_sha: str
    release_tags: tuple[str, ...]

    @property
    def release_commits_peeled(self) -> int:
        return len(self.release_tags)


@dataclass(frozen=True)
class CiEvidence:
    workflow_id: int
    run_id: int
    run_attempt: int
    job_id: int
    event: str
    url: str


class JsonApi(Protocol):
    def get(self, path: str, params: Mapping[str, object] | None = None) -> Mapping[str, Any]:
        """Return a decoded JSON object for an API path."""


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Keep the Actions token on the explicitly configured API origin."""

    def redirect_request(
        self,
        req: urllib.request.Request,
        fp: Any,
        code: int,
        msg: str,
        headers: Mapping[str, str],
        newurl: str,
    ) -> None:
        del req, fp, code, msg, headers, newurl
        return None


def _git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown git error"
        raise CiEvidenceError(f"git {' '.join(args)} failed: {detail}")
    return result.stdout.rstrip("\n")


def _require_sha(value: str, label: str) -> str:
    normalized = value.strip().lower()
    if not SHA_RE.fullmatch(normalized):
        raise CiEvidenceError(f"{label} must be a full 40-character commit SHA")
    return normalized


def _resolve_commit(root: Path, value: str, label: str) -> str:
    sha = _require_sha(value, label)
    resolved = _git(root, "rev-parse", "--verify", f"{sha}^{{commit}}").lower()
    if resolved != sha:
        raise CiEvidenceError(f"{label} resolved to {resolved}, expected {sha}")
    return sha


def _parents(root: Path, sha: str) -> tuple[str, ...]:
    fields = _git(root, "rev-list", "--parents", "-n", "1", sha).split()
    if not fields or fields[0].lower() != sha:
        raise CiEvidenceError(f"could not read parents for {sha}")
    return tuple(parent.lower() for parent in fields[1:])


def _tags_at(root: Path, sha: str) -> tuple[str, ...]:
    output = _git(root, "tag", "--points-at", sha, "--format=%(refname:short)")
    return tuple(sorted(line for line in output.splitlines() if line))


def _tag_contents(root: Path, tag: str) -> str:
    return _git(root, "for-each-ref", "--format=%(contents)", f"refs/tags/{tag}")


def _validate_release_commit(root: Path, sha: str, version: str, tag: str) -> str:
    parents = _parents(root, sha)
    if len(parents) != 1:
        raise CiEvidenceError(
            f"release-like commit {sha} must have exactly one parent; found {len(parents)}"
        )
    parent = parents[0]

    semver_tags = tuple(
        candidate
        for candidate in _tags_at(root, sha)
        if SEMVER_TAG_RE.fullmatch(candidate)
    )
    if semver_tags != (tag,):
        found = ", ".join(semver_tags) or "none"
        raise CiEvidenceError(
            f"release-like commit {sha} must have only annotated tag {tag}; found {found}"
        )
    tag_ref = f"refs/tags/{tag}"
    if _git(root, "cat-file", "-t", tag_ref) != "tag":
        raise CiEvidenceError(f"release tag {tag} must be annotated, not lightweight")
    target = _git(root, "rev-parse", f"{tag_ref}^{{commit}}").lower()
    if target != sha:
        raise CiEvidenceError(f"release tag {tag} resolves to {target}, expected {sha}")

    marker_lines = [
        line.strip()
        for line in _tag_contents(root, tag).splitlines()
        if line.strip().startswith("Release-Run-ID:")
    ]
    if len(marker_lines) != 1 or not RUN_ID_RE.fullmatch(marker_lines[0]):
        raise CiEvidenceError(
            f"annotated release tag {tag} must contain exactly one valid Release-Run-ID"
        )

    changes: set[tuple[str, str]] = set()
    diff = _git(
        root,
        "diff-tree",
        "--no-commit-id",
        "--name-status",
        "-r",
        parent,
        sha,
    )
    for line in diff.splitlines():
        fields = line.split("\t")
        if len(fields) != 2:
            raise CiEvidenceError(
                f"release commit {sha} contains a rename or malformed change: {line}"
            )
        changes.add((fields[0], fields[1]))
    if changes != EXPECTED_RELEASE_CHANGES:
        rendered = ", ".join(f"{status} {path}" for status, path in sorted(changes)) or "none"
        raise CiEvidenceError(
            f"release commit {sha} must modify exactly CHANGELOG.md and include/config.h; "
            f"found {rendered}"
        )

    try:
        validate_release_config_change(root, parent, sha, version)
    except ReleaseConfigChangeError as exc:
        raise CiEvidenceError(f"release commit {sha}: {exc}") from exc

    changelog = _git(root, "show", f"{sha}:CHANGELOG.md")
    heading = re.compile(
        rf"^## \[{re.escape(version)}\] - \d{{4}}-\d{{2}}-\d{{2}}\s*$",
        re.MULTILINE,
    )
    if not heading.search(changelog):
        raise CiEvidenceError(
            f"release commit {sha} is missing the CHANGELOG heading for {version}"
        )
    return parent


def resolve_release_chain(
    root: Path,
    base_sha: str,
    *,
    main_ref: str,
    max_release_commits: int = 100,
) -> ReleaseChain:
    """Resolve a release base to the code-bearing commit that needs CI evidence."""

    if max_release_commits < 0:
        raise CiEvidenceError("max_release_commits must not be negative")
    root = root.resolve()
    if not (root / ".git").exists():
        raise CiEvidenceError(f"release root is not a Git repository: {root}")
    base = _resolve_commit(root, base_sha, "base SHA")
    ref_check = subprocess.run(
        ["git", "check-ref-format", "--allow-onelevel", main_ref],
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if ref_check.returncode != 0:
        raise CiEvidenceError(f"invalid main ref: {main_ref!r}")
    try:
        main_sha = _git(root, "rev-parse", "--verify", f"{main_ref}^{{commit}}").lower()
    except CiEvidenceError as exc:
        raise CiEvidenceError(f"could not resolve main ref {main_ref!r}: {exc}") from exc
    ancestry = subprocess.run(
        ["git", "merge-base", "--is-ancestor", base, main_sha],
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if ancestry.returncode == 1:
        raise CiEvidenceError(f"base SHA {base} is not in {main_ref} history")
    if ancestry.returncode != 0:
        raise CiEvidenceError(
            f"could not verify {base} ancestry against {main_ref}: {ancestry.stderr.strip()}"
        )

    current = base
    release_tags: list[str] = []
    while True:
        subject = _git(root, "show", "-s", "--format=%s", current)
        subject_match = RELEASE_SUBJECT_RE.fullmatch(subject)
        if not subject_match:
            break
        if len(release_tags) >= max_release_commits:
            raise CiEvidenceError(
                f"release-only chain exceeds configured limit of {max_release_commits} commits"
            )
        version = ".".join(subject_match.groups())
        tag = f"v{version}"
        current = _validate_release_commit(root, current, version, tag)
        release_tags.append(tag)

    return ReleaseChain(base_sha=base, ci_sha=current, release_tags=tuple(release_tags))


class GitHubApiClient:
    """Small authenticated GitHub JSON client with bounded transient retries."""

    def __init__(
        self,
        *,
        api_url: str,
        repository: str,
        token: str,
        timeout: float = 30.0,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        parsed = urllib.parse.urlparse(api_url)
        if (
            parsed.scheme != "https"
            or not parsed.netloc
            or parsed.username is not None
            or parsed.password is not None
            or parsed.query
            or parsed.fragment
        ):
            raise CiEvidenceError(
                "GitHub API URL must use HTTPS and must not contain a query or fragment"
            )
        if not REPOSITORY_RE.fullmatch(repository):
            raise CiEvidenceError(f"invalid GitHub repository name: {repository!r}")
        if not token or any(character.isspace() for character in token):
            raise CiEvidenceError(
                "missing or malformed Actions API token; provide GITHUB_TOKEN with actions: read"
            )
        self.api_url = api_url.rstrip("/")
        self.repository = repository
        self.token = token
        self.timeout = timeout
        self.sleeper = sleeper
        self.opener = urllib.request.build_opener(_NoRedirectHandler())

    def get(self, path: str, params: Mapping[str, object] | None = None) -> Mapping[str, Any]:
        if not path.startswith("/") or path.startswith("//"):
            raise GitHubApiError(f"invalid GitHub API path: {path!r}")
        query = urllib.parse.urlencode(params or {})
        url = f"{self.api_url}{path}"
        if query:
            url = f"{url}?{query}"
        request = urllib.request.Request(
            url,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self.token}",
                "User-Agent": "v1simple-release-ci-evidence",
                "X-GitHub-Api-Version": DEFAULT_API_VERSION,
            },
            method="GET",
        )
        transient_codes = {429, 500, 502, 503, 504}
        for attempt in range(3):
            try:
                with self.opener.open(request, timeout=self.timeout) as response:
                    payload = json.load(response)
                if not isinstance(payload, dict):
                    raise GitHubApiError(f"GitHub API returned non-object JSON for {path}")
                return payload
            except urllib.error.HTTPError as exc:
                if exc.code in transient_codes and attempt < 2:
                    retry_after = exc.headers.get("Retry-After")
                    try:
                        delay = min(float(retry_after), 30.0) if retry_after else float(2**attempt)
                    except ValueError:
                        delay = float(2**attempt)
                    self.sleeper(delay)
                    continue
                if exc.code in (401, 403):
                    raise GitHubApiError(
                        "GitHub Actions API authorization failed; GITHUB_TOKEN must have "
                        "actions: read"
                    ) from exc
                if exc.code == 404:
                    raise GitHubApiError(
                        f"GitHub Actions API resource not found for {path}; "
                        "verify repository access "
                        "and actions: read permission"
                    ) from exc
                raise GitHubApiError(
                    f"GitHub API request for {path} failed with HTTP {exc.code}"
                ) from exc
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
                if attempt < 2:
                    self.sleeper(float(2**attempt))
                    continue
                raise GitHubApiError(f"GitHub API request for {path} failed: {exc}") from exc
        raise AssertionError("unreachable GitHub API retry state")


def _required_string(record: Mapping[str, Any], key: str, label: str) -> str:
    value = record.get(key)
    if not isinstance(value, str) or not value:
        raise GitHubApiError(f"{label} is missing string field {key!r}")
    return value


def _required_positive_int(record: Mapping[str, Any], key: str, label: str) -> int:
    value = record.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise GitHubApiError(f"{label} is missing positive integer field {key!r}")
    return value


def _canonical_workflow_path(path: str) -> str:
    return path.split("@", 1)[0]


def _paginated_items(
    api: JsonApi,
    path: str,
    key: str,
    params: Mapping[str, object],
) -> list[Mapping[str, Any]]:
    items: list[Mapping[str, Any]] = []
    page = 1
    while True:
        page_params = dict(params)
        page_params.update({"page": page, "per_page": 100})
        payload = api.get(path, page_params)
        raw_items = payload.get(key)
        if not isinstance(raw_items, list):
            raise GitHubApiError(f"GitHub API response for {path} is missing list field {key!r}")
        for item in raw_items:
            if not isinstance(item, dict):
                raise GitHubApiError(
                    f"GitHub API response for {path} contains a non-object {key} item"
                )
            items.append(item)
        total = payload.get("total_count")
        if isinstance(total, int) and not isinstance(total, bool) and len(items) >= total:
            break
        if len(raw_items) < 100:
            break
        page += 1
        if page > 10:
            raise GitHubApiError(f"GitHub API pagination for {path} exceeded 1,000 records")
    return items


def _validate_workflow(
    record: Mapping[str, Any],
    *,
    expected_name: str,
    expected_path: str,
) -> int:
    workflow_id = _required_positive_int(record, "id", "CI workflow")
    if _required_string(record, "name", "CI workflow") != expected_name:
        raise CiEvidenceError(f"CI workflow name must be {expected_name!r}")
    actual_path = _canonical_workflow_path(_required_string(record, "path", "CI workflow"))
    if actual_path != expected_path:
        raise CiEvidenceError(f"CI workflow path must be {expected_path!r}; found {actual_path!r}")
    if _required_string(record, "state", "CI workflow") != "active":
        raise CiEvidenceError("authoritative CI workflow is not active")
    return workflow_id


def _run_matches_identity(
    run: Mapping[str, Any],
    *,
    repository: str,
    ci_sha: str,
    branch: str,
    workflow_id: int,
    workflow_name: str,
    workflow_path: str,
) -> bool:
    repository_record = run.get("repository")
    head_repository = run.get("head_repository")
    if not isinstance(repository_record, dict) or not isinstance(head_repository, dict):
        return False
    return (
        run.get("workflow_id") == workflow_id
        and run.get("name") == workflow_name
        and isinstance(run.get("path"), str)
        and _canonical_workflow_path(run["path"]) == workflow_path
        and run.get("event") in ALLOWED_EVENTS
        and run.get("head_branch") == branch
        and run.get("head_sha") == ci_sha
        and repository_record.get("full_name") == repository
        and head_repository.get("full_name") == repository
    )


def _latest_matching_run(
    runs: Sequence[Mapping[str, Any]],
    *,
    repository: str,
    ci_sha: str,
    branch: str,
    workflow_id: int,
    workflow_name: str,
    workflow_path: str,
    expected_run_id: int | None = None,
) -> Mapping[str, Any] | None:
    matching = [
        run
        for run in runs
        if _run_matches_identity(
            run,
            repository=repository,
            ci_sha=ci_sha,
            branch=branch,
            workflow_id=workflow_id,
            workflow_name=workflow_name,
            workflow_path=workflow_path,
        )
        and (expected_run_id is None or run.get("id") == expected_run_id)
    ]
    if not matching:
        return None
    for run in matching:
        _required_positive_int(run, "id", "CI run")
        _required_positive_int(run, "run_number", "CI run")
        _required_string(run, "created_at", "CI run")
    return max(matching, key=lambda run: (run["run_number"], run["created_at"], run["id"]))


def _require_successful_run(run: Mapping[str, Any]) -> tuple[int, int]:
    run_id = _required_positive_int(run, "id", "CI run")
    attempt = _required_positive_int(run, "run_attempt", "CI run")
    status = _required_string(run, "status", "CI run")
    if status != "completed":
        raise CiEvidenceError(f"CI run {run_id} attempt {attempt} is {status}, not completed")
    conclusion = _required_string(run, "conclusion", "CI run")
    if conclusion != "success":
        url = run.get("html_url", "")
        raise CiEvidenceError(
            f"latest CI run {run_id} attempt {attempt} concluded {conclusion}, not success"
            + (f": {url}" if url else "")
        )
    return run_id, attempt


def _validate_job(
    jobs: Sequence[Mapping[str, Any]],
    *,
    repository: str,
    ci_sha: str,
    branch: str,
    workflow_name: str,
    run_id: int,
    run_attempt: int,
    job_name: str,
    step_name: str,
) -> int:
    del repository  # Identity is already pinned by the workflow run response.
    matching = [job for job in jobs if job.get("name") == job_name]
    if len(matching) != 1:
        raise CiEvidenceError(
            f"CI run {run_id} attempt {run_attempt} must contain exactly one {job_name!r} job; "
            f"found {len(matching)}"
        )
    job = matching[0]
    expected = {
        "workflow_name": workflow_name,
        "head_branch": branch,
        "head_sha": ci_sha,
        "run_id": run_id,
        "run_attempt": run_attempt,
        "status": "completed",
        "conclusion": "success",
    }
    for key, value in expected.items():
        if job.get(key) != value:
            raise CiEvidenceError(
                f"CI job {job_name!r} has unexpected {key}: {job.get(key)!r}; expected {value!r}"
            )
    job_id = _required_positive_int(job, "id", "CI job")
    steps = job.get("steps")
    if not isinstance(steps, list):
        raise GitHubApiError(f"CI job {job_id} is missing its steps list")
    matching_steps = [
        step
        for step in steps
        if isinstance(step, dict) and step.get("name") == step_name
    ]
    if len(matching_steps) != 1:
        raise CiEvidenceError(
            f"CI job {job_id} must contain exactly one {step_name!r} step; "
            f"found {len(matching_steps)}"
        )
    step = matching_steps[0]
    if step.get("status") != "completed" or step.get("conclusion") != "success":
        raise CiEvidenceError(
            f"CI step {step_name!r} must be completed successfully; "
            f"found status={step.get('status')!r}, conclusion={step.get('conclusion')!r}"
        )
    return job_id


def verify_actions_evidence(
    api: JsonApi,
    *,
    repository: str,
    ci_sha: str,
    branch: str = "main",
    workflow_file: str = DEFAULT_WORKFLOW_FILE,
    workflow_path: str = DEFAULT_WORKFLOW_PATH,
    workflow_name: str = DEFAULT_WORKFLOW_NAME,
    job_name: str = DEFAULT_JOB_NAME,
    step_name: str = DEFAULT_STEP_NAME,
    expected_run_id: int | None = None,
    wait_seconds: float = 0.0,
    poll_seconds: float = 15.0,
    clock: Callable[[], float] = time.monotonic,
    sleeper: Callable[[float], None] = time.sleep,
) -> CiEvidence:
    """Verify the specified or latest exact Actions run, attempt, job, and gate step."""

    if not REPOSITORY_RE.fullmatch(repository):
        raise CiEvidenceError(f"invalid GitHub repository name: {repository!r}")
    ci_sha = _require_sha(ci_sha, "CI SHA")
    if expected_run_id is not None and (
        isinstance(expected_run_id, bool) or expected_run_id <= 0
    ):
        raise CiEvidenceError("expected CI run ID must be a positive integer")
    if wait_seconds < 0:
        raise CiEvidenceError("wait_seconds must not be negative")
    if poll_seconds <= 0:
        raise CiEvidenceError("poll_seconds must be greater than zero")

    quoted_repository = "/".join(
        urllib.parse.quote(part, safe="") for part in repository.split("/")
    )
    quoted_workflow = urllib.parse.quote(workflow_file, safe="")
    workflow_record = api.get(
        f"/repos/{quoted_repository}/actions/workflows/{quoted_workflow}"
    )
    workflow_id = _validate_workflow(
        workflow_record,
        expected_name=workflow_name,
        expected_path=workflow_path,
    )
    runs_path = f"/repos/{quoted_repository}/actions/workflows/{workflow_id}/runs"
    deadline = clock() + wait_seconds

    while True:
        runs = _paginated_items(
            api,
            runs_path,
            "workflow_runs",
            {"branch": branch, "head_sha": ci_sha},
        )
        latest = _latest_matching_run(
            runs,
            repository=repository,
            ci_sha=ci_sha,
            branch=branch,
            workflow_id=workflow_id,
            workflow_name=workflow_name,
            workflow_path=workflow_path,
            expected_run_id=expected_run_id,
        )
        if latest is None:
            if expected_run_id is None:
                reason = f"no authoritative CI run exists for {ci_sha} on {branch}"
            else:
                reason = (
                    f"authoritative CI run {expected_run_id} does not match "
                    f"{ci_sha} on {branch}"
                )
        elif latest.get("status") != "completed":
            run_id = latest.get("id", "unknown")
            reason = f"latest authoritative CI run {run_id} is {latest.get('status', 'unknown')}"
        else:
            # A completed non-success is final.  Do not hide it behind an older success.
            run_id, run_attempt = _require_successful_run(latest)
            detail_path = f"/repos/{quoted_repository}/actions/runs/{run_id}"
            detail = api.get(detail_path)
            if not _run_matches_identity(
                detail,
                repository=repository,
                ci_sha=ci_sha,
                branch=branch,
                workflow_id=workflow_id,
                workflow_name=workflow_name,
                workflow_path=workflow_path,
            ):
                raise CiEvidenceError(
                    f"CI run detail {run_id} no longer matches authoritative identity"
                )
            if detail.get("status") != "completed":
                reason = (
                    f"CI run {run_id} changed to {detail.get('status', 'unknown')} "
                    "during verification"
                )
            else:
                detail_id, detail_attempt = _require_successful_run(detail)
                if detail_id != run_id:
                    raise GitHubApiError(
                        f"CI run detail returned ID {detail_id}, expected {run_id}"
                    )
                if detail_attempt != run_attempt:
                    reason = (
                        f"CI run {run_id} changed from attempt {run_attempt} "
                        f"to {detail_attempt}"
                    )
                else:
                    jobs_path = (
                        f"/repos/{quoted_repository}/actions/runs/{run_id}/attempts/"
                        f"{run_attempt}/jobs"
                    )
                    jobs = _paginated_items(api, jobs_path, "jobs", {})
                    job_id = _validate_job(
                        jobs,
                        repository=repository,
                        ci_sha=ci_sha,
                        branch=branch,
                        workflow_name=workflow_name,
                        run_id=run_id,
                        run_attempt=run_attempt,
                        job_name=job_name,
                        step_name=step_name,
                    )
                    final_detail = api.get(detail_path)
                    if not _run_matches_identity(
                        final_detail,
                        repository=repository,
                        ci_sha=ci_sha,
                        branch=branch,
                        workflow_id=workflow_id,
                        workflow_name=workflow_name,
                        workflow_path=workflow_path,
                    ):
                        raise CiEvidenceError(
                            f"CI run detail {run_id} changed authoritative identity "
                            "during verification"
                        )
                    if final_detail.get("status") != "completed":
                        reason = (
                            f"CI run {run_id} changed to "
                            f"{final_detail.get('status', 'unknown')} during verification"
                        )
                    else:
                        final_id, final_attempt = _require_successful_run(final_detail)
                        if final_id == run_id and final_attempt == run_attempt:
                            return CiEvidence(
                                workflow_id=workflow_id,
                                run_id=run_id,
                                run_attempt=run_attempt,
                                job_id=job_id,
                                event=_required_string(final_detail, "event", "CI run"),
                                url=_required_string(final_detail, "html_url", "CI run"),
                            )
                        reason = (
                            f"CI run {run_id} changed attempt during verification "
                            f"({run_attempt} to {final_attempt})"
                        )

        remaining = deadline - clock()
        if remaining <= 0:
            raise CiEvidenceError(reason)
        sleeper(min(poll_seconds, remaining))


def write_github_outputs(path: Path, chain: ReleaseChain, evidence: CiEvidence) -> None:
    values = {
        "base_sha": chain.base_sha,
        "ci_sha": chain.ci_sha,
        "ci_workflow_id": str(evidence.workflow_id),
        "ci_run_id": str(evidence.run_id),
        "ci_run_attempt": str(evidence.run_attempt),
        "ci_job_id": str(evidence.job_id),
        "ci_event": evidence.event,
        "ci_url": evidence.url,
        "release_commits_peeled": str(chain.release_commits_peeled),
        "release_tags": ",".join(chain.release_tags),
    }
    for key, value in values.items():
        if "\n" in value or "\r" in value:
            raise CiEvidenceError(f"refusing multiline GITHUB_OUTPUT value for {key}")
    with path.open("a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Verify that a release base, after validated release-only commits are peeled, "
            "has successful authoritative GitHub Actions CI evidence."
        )
    )
    parser.add_argument("--root", type=Path, default=Path("."), help="Git repository root")
    parser.add_argument("--base-sha", required=True, help="Full release base commit SHA")
    parser.add_argument(
        "--main-ref",
        default="origin/main",
        help="Fetched main ref that must contain the base SHA (default: origin/main)",
    )
    parser.add_argument(
        "--repository",
        default=os.environ.get("GITHUB_REPOSITORY", ""),
        help="GitHub OWNER/REPO (default: GITHUB_REPOSITORY)",
    )
    parser.add_argument("--branch", default="main", help="Authoritative CI branch")
    parser.add_argument(
        "--api-url",
        default=os.environ.get("GITHUB_API_URL", "https://api.github.com"),
        help="GitHub REST API origin",
    )
    parser.add_argument(
        "--token-env",
        default="GITHUB_TOKEN",
        help="Environment variable containing an actions: read token",
    )
    parser.add_argument(
        "--expected-run-id",
        type=int,
        help="Require evidence from this exact triggering workflow run ID",
    )
    parser.add_argument(
        "--wait-seconds",
        type=float,
        default=0.0,
        help="Maximum time to wait for missing, queued, or rerun CI evidence",
    )
    parser.add_argument(
        "--poll-seconds",
        type=float,
        default=15.0,
        help="Polling interval while waiting for CI evidence",
    )
    parser.add_argument(
        "--max-release-commits",
        type=int,
        default=100,
        help="Maximum consecutive generated release commits to validate",
    )
    parser.add_argument(
        "--github-output",
        type=Path,
        default=Path(os.environ["GITHUB_OUTPUT"]) if os.environ.get("GITHUB_OUTPUT") else None,
        help="Append verified evidence fields to this GitHub Actions output file",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if not args.repository:
            raise CiEvidenceError("--repository or GITHUB_REPOSITORY is required")
        token = os.environ.get(args.token_env, "")
        chain = resolve_release_chain(
            args.root,
            args.base_sha,
            main_ref=args.main_ref,
            max_release_commits=args.max_release_commits,
        )
        api = GitHubApiClient(
            api_url=args.api_url,
            repository=args.repository,
            token=token,
        )
        evidence = verify_actions_evidence(
            api,
            repository=args.repository,
            ci_sha=chain.ci_sha,
            branch=args.branch,
            expected_run_id=args.expected_run_id,
            wait_seconds=args.wait_seconds,
            poll_seconds=args.poll_seconds,
        )
        if args.github_output is not None:
            write_github_outputs(args.github_output, chain, evidence)
    except (CiEvidenceError, OSError) as exc:
        print(f"[ci-evidence] ERROR: {exc}", file=sys.stderr)
        return 1

    print("[ci-evidence] authoritative CI evidence verified")
    print(f"[ci-evidence] release base: {chain.base_sha}")
    print(
        f"[ci-evidence] tested commit: {chain.ci_sha} "
        f"({chain.release_commits_peeled} release-only commit(s) peeled)"
    )
    print(
        f"[ci-evidence] CI run: {evidence.run_id} attempt {evidence.run_attempt}; "
        f"job {evidence.job_id}; {evidence.url}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
