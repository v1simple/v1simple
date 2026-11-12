#!/usr/bin/env python3
"""Publish one already-built release without hiding policy in workflow YAML.

``prepare_release.py`` owns version and tag selection. This module owns the
publication decision after a tag has been selected: exact checkout identity,
same-run retries, main-advance handling, tag reservation, ambiguous push
recovery, and Pages eligibility.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys
from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import prepare_release


ROOT = Path(__file__).resolve().parents[1]


class PublicationError(RuntimeError):
    """Publication cannot proceed without violating a release invariant."""


class Action(str, Enum):
    ATTEMPT_PUBLISH = "attempt_publish"
    PUBLISHED = "published"
    REUSE = "reuse"
    RECOVER = "recover"
    SKIP_MAIN_ADVANCED = "skip_main_advanced"
    FAIL = "fail"


@dataclass(frozen=True)
class PublicationFacts:
    release_sha: str
    base_sha: str
    release_tag: str
    head_sha: str
    dirty: bool
    remote_main_sha: str
    existing_tag_sha: str | None = None
    existing_marker_matches: bool = False
    existing_release_public: bool = False
    push_attempted: bool = False
    push_succeeded: bool | None = None
    updated_main_sha: str | None = None
    updated_tag_sha: str | None = None
    updated_marker_matches: bool = False


@dataclass(frozen=True)
class Decision:
    action: Action
    diagnostic: str
    published: bool = False


@dataclass(frozen=True)
class PublicationResult:
    action: Action
    published: bool
    deploy_pages: bool
    release_sha: str


@dataclass(frozen=True)
class RemoteReleaseRefs:
    main: str
    tag_commit: str | None
    marker_matches: bool
    latest_tag: str


def decide_publication(facts: PublicationFacts) -> Decision:
    """Return the publication action for a fully explicit repository snapshot."""

    if facts.head_sha != facts.release_sha:
        return Decision(
            Action.FAIL,
            "Release SHA no longer matches the tested working tree.",
        )
    if facts.dirty:
        return Decision(Action.FAIL, "Refusing to publish a dirty release working tree.")

    if facts.existing_tag_sha is not None:
        if facts.existing_tag_sha != facts.release_sha:
            return Decision(
                Action.FAIL,
                f"Tag {facts.release_tag} already exists at {facts.existing_tag_sha}; "
                "it will not be moved.",
            )
        if not facts.existing_marker_matches:
            return Decision(
                Action.FAIL,
                f"{facts.release_tag} does not carry this run's publication marker.",
            )
        if facts.remote_main_sha != facts.base_sha and not facts.existing_release_public:
            return Decision(
                Action.SKIP_MAIN_ADVANCED,
                f"main advanced before {facts.release_tag} was released; its tag remains reserved.",
            )
        return Decision(
            Action.REUSE,
            f"Tag {facts.release_tag} already points at tested commit "
            f"{facts.release_sha}; reusing it.",
            published=True,
        )

    if facts.remote_main_sha != facts.base_sha:
        return Decision(
            Action.SKIP_MAIN_ADVANCED,
            f"main advanced from {facts.base_sha} to {facts.remote_main_sha}; "
            "the newer merge has its own queued release.",
        )

    if not facts.push_attempted:
        return Decision(Action.ATTEMPT_PUBLISH, "Remote state permits a release tag reservation.")

    if facts.updated_tag_sha == facts.release_sha:
        if not facts.updated_marker_matches:
            return Decision(
                Action.FAIL,
                f"{facts.release_tag} reached the expected commit without this run's "
                "publication marker.",
            )
    if facts.updated_main_sha != facts.base_sha:
        return Decision(
            Action.SKIP_MAIN_ADVANCED,
            f"main advanced from {facts.base_sha} to {facts.updated_main_sha} during tag reservation; "
            "the newer merge has its own queued release.",
        )
    if facts.updated_tag_sha == facts.release_sha:
        return Decision(
            Action.PUBLISHED if facts.push_succeeded else Action.RECOVER,
            f"Verified tag {facts.release_tag} at {facts.release_sha} after the push.",
            published=True,
        )

    return Decision(
        Action.FAIL,
        f"Could not verify the release tag while main remained at {facts.base_sha}.",
    )


def should_deploy_pages(release_tag: str, latest_release_tag: str) -> bool:
    """Only the newest semantic release may replace the Pages installer."""

    return release_tag == latest_release_tag


def run_git(
    root: Path,
    *args: str,
    check: bool = True,
    extra_config: tuple[str, ...] = (),
) -> subprocess.CompletedProcess[str]:
    command = ["git", "-C", str(root)]
    for value in extra_config:
        command.extend(("-c", value))
    command.extend(args)
    result = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown git error"
        raise PublicationError(detail)
    return result


def resolve_commit(root: Path, ref: str) -> str | None:
    result = run_git(root, "rev-parse", "--verify", f"{ref}^{{commit}}", check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def marker_matches(root: Path, release_tag: str, release_run_id: str) -> bool:
    result = run_git(
        root,
        "for-each-ref",
        "--format=%(contents)",
        f"refs/tags/{release_tag}",
    )
    marker = f"Release-Run-ID: {release_run_id}"
    return marker in result.stdout.splitlines()


def is_ancestor(root: Path, ancestor: str, descendant: str) -> bool:
    result = run_git(
        root,
        "merge-base",
        "--is-ancestor",
        ancestor,
        descendant,
        check=False,
    )
    if result.returncode not in {0, 1}:
        raise PublicationError(result.stderr.strip() or "git merge-base failed")
    return result.returncode == 0


def read_release_refs(root: Path, remote: str, tag: str, run_id: str) -> RemoteReleaseRefs:
    run_git(root, "fetch", "--force", remote, "main")
    run_git(root, "fetch", "--force", "--tags", remote)
    # One advertisement captures main, the tag object, its peeled target and
    # competing semantic tags together. Never use a local-only tag as proof.
    advertisement = run_git(root, "ls-remote", remote, "refs/heads/main", "refs/tags/*")
    refs = dict(line.split("\t")[::-1] for line in advertisement.stdout.splitlines())
    main = refs.get("refs/heads/main")
    if not main:
        raise PublicationError("remote main is unavailable")
    tag_ref = f"refs/tags/{tag}"
    tag_object = refs.get(tag_ref)
    peeled = refs.get(f"{tag_ref}^{{}}")
    local_tag = run_git(root, "rev-parse", "--verify", tag_ref, check=False).stdout.strip()
    matches = bool(peeled and tag_object == local_tag and marker_matches(root, tag, run_id))
    tags = [ref.removeprefix("refs/tags/v") for ref in refs if ref.startswith("refs/tags/v")]
    versions = [prepare_release.Version.parse(value) for value in tags if prepare_release.SEMVER_RE.fullmatch(value)]
    latest = f"v{max(versions)}" if versions else ""
    return RemoteReleaseRefs(main, peeled or tag_object, matches, latest)


def public_github_release(tag: str) -> bool:
    repository = os.environ.get("GITHUB_REPOSITORY")
    if not repository:
        return False
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not token:
        raise PublicationError("missing GitHub credentials for Release lookup")
    api = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    request = Request(
        f"{api}/repos/{repository}/releases/tags/{tag}",
        headers={"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json"},
    )
    try:
        with urlopen(request, timeout=15) as response:
            data = json.load(response)
    except HTTPError as exc:
        if exc.code == 404:
            return False
        raise PublicationError(f"GitHub Release lookup failed with HTTP {exc.code}") from exc
    if data.get("tag_name") != tag or type(data.get("draft")) is not bool:
        raise PublicationError("GitHub Release lookup returned an unexpected tag or status")
    return not data["draft"]


def write_outputs(path: Path | None, values: dict[str, str]) -> None:
    if path is None:
        return
    with path.open("a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")


def publication_inputs_are_valid(
    release_sha: str,
    base_sha: str,
    release_tag: str,
    release_run_id: str,
) -> None:
    if not release_sha or not base_sha:
        raise PublicationError("release and base SHAs are required")
    if release_sha != base_sha:
        raise PublicationError("release SHA must be the qualified main SHA")
    if not release_tag.startswith("v") or not prepare_release.SEMVER_RE.fullmatch(
        release_tag[1:]
    ):
        raise PublicationError(
            f"invalid release tag {release_tag!r}; expected vMAJOR.MINOR.PATCH"
        )
    if not prepare_release.RUN_ID_RE.fullmatch(release_run_id):
        raise PublicationError(f"invalid GitHub Actions run ID: {release_run_id!r}")


def report_eligibility(
    decision: Decision, refs: RemoteReleaseRefs, tag: str, sha: str, output_path: Path | None,
    already_public: bool | None = None,
) -> PublicationResult:
    if decision.action is Action.FAIL:
        raise PublicationError(decision.diagnostic)
    print(decision.diagnostic if decision.published else f"::notice::{decision.diagnostic}")
    deploy = decision.published and refs.main == sha and should_deploy_pages(tag, refs.latest_tag)
    values = {"published": str(decision.published).lower(), "deploy_pages": str(deploy).lower()}
    if decision.published:
        values["sha"] = sha
    if already_public is not None:
        values["already_public"] = str(already_public).lower()
    write_outputs(output_path, values)
    return PublicationResult(decision.action, decision.published, deploy, sha)


def publish_release(
    root: Path,
    *,
    release_sha: str,
    base_sha: str,
    release_tag: str,
    release_run_id: str,
    github_token: str = "",
    remote: str = "origin",
    output_path: Path | None = None,
    has_public_release: Callable[[str], bool] = lambda _tag: False,
    check_only: bool = False,
) -> PublicationResult:
    """Reserve an append-only tag and independently confirm publication eligibility."""

    root = root.resolve()
    publication_inputs_are_valid(release_sha, base_sha, release_tag, release_run_id)

    head_sha = run_git(root, "rev-parse", "HEAD").stdout.strip()
    dirty = bool(run_git(root, "status", "--porcelain").stdout.strip())
    if head_sha != release_sha or dirty:
        decision = decide_publication(
            PublicationFacts(
                release_sha=release_sha,
                base_sha=base_sha,
                release_tag=release_tag,
                head_sha=head_sha,
                dirty=dirty,
                remote_main_sha="",
            )
        )
        raise PublicationError(decision.diagnostic)

    refs = read_release_refs(root, remote, release_tag, release_run_id)
    already_public = (
        refs.tag_commit == release_sha and refs.marker_matches
        and (check_only or refs.main != release_sha) and has_public_release(release_tag)
    )
    # A public-release lookup can take time. Check refs again before deciding
    # to create or deploy, and require the same owned tag on that fresh view.
    if check_only:
        refs = read_release_refs(root, remote, release_tag, release_run_id)
    pre_push = decide_publication(
        PublicationFacts(
            release_sha=release_sha,
            base_sha=base_sha,
            release_tag=release_tag,
            head_sha=head_sha,
            dirty=False,
            remote_main_sha=refs.main,
            existing_tag_sha=refs.tag_commit,
            existing_marker_matches=refs.marker_matches,
            existing_release_public=already_public and is_ancestor(root, release_sha, refs.main),
        )
    )
    if pre_push.action is not Action.ATTEMPT_PUBLISH:
        return report_eligibility(
            pre_push, refs, release_tag, release_sha, output_path, already_public if check_only else None,
        )
    if check_only:
        raise PublicationError(f"Could not verify reservation for {release_tag}.")

    local_target = resolve_commit(root, f"refs/tags/{release_tag}")
    if local_target is None:
        run_git(
            root,
            "tag",
            "-a",
            release_tag,
            release_sha,
            "-m",
            f"Release {release_tag}",
            "-m",
            f"Release-Run-ID: {release_run_id}",
        )

    elif local_target != release_sha or not marker_matches(root, release_tag, release_run_id):
        raise PublicationError(f"Conflicting local reservation for {release_tag}.")

    push_config: tuple[str, ...] = ()
    if github_token:
        auth = base64.b64encode(f"x-access-token:{github_token}".encode("utf-8")).decode("ascii")
        push_config = (f"http.https://github.com/.extraheader=AUTHORIZATION: basic {auth}",)
    tag_ref = f"refs/tags/{release_tag}"
    push = run_git(root, "push", remote, f"{tag_ref}:{tag_ref}", check=False, extra_config=push_config)

    # An unchanged main ref would be omitted by git push even with --atomic.
    # Recheck both refs after positive and ambiguous push results.
    updated = read_release_refs(root, remote, release_tag, release_run_id)
    terminal = decide_publication(
        PublicationFacts(
            release_sha=release_sha,
            base_sha=base_sha,
            release_tag=release_tag,
            head_sha=head_sha,
            dirty=False,
            remote_main_sha=refs.main,
            push_attempted=True,
            push_succeeded=push.returncode == 0,
            updated_main_sha=updated.main,
            updated_tag_sha=updated.tag_commit,
            updated_marker_matches=updated.marker_matches,
        )
    )
    return report_eligibility(terminal, updated, release_tag, release_sha, output_path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--release-sha", default=os.environ.get("RELEASE_SHA", ""))
    parser.add_argument("--base-sha", default=os.environ.get("BASE_SHA", ""))
    parser.add_argument("--release-tag", default=os.environ.get("RELEASE_TAG", ""))
    parser.add_argument(
        "--release-run-id", default=os.environ.get("RELEASE_RUN_ID", "")
    )
    parser.add_argument("--remote", default="origin")
    parser.add_argument("--check-eligibility", action="store_true")
    parser.add_argument(
        "--github-output",
        type=Path,
        default=Path(os.environ["GITHUB_OUTPUT"])
        if os.environ.get("GITHUB_OUTPUT")
        else None,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        publish_release(
            args.root,
            release_sha=args.release_sha,
            base_sha=args.base_sha,
            release_tag=args.release_tag,
            release_run_id=args.release_run_id,
            github_token=os.environ.get("GITHUB_TOKEN", ""),
            remote=args.remote,
            output_path=args.github_output,
            has_public_release=public_github_release,
            check_only=args.check_eligibility,
        )
    except (OSError, ValueError, PublicationError) as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
