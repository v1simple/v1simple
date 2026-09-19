#!/usr/bin/env python3
"""Publish one already-built release without hiding policy in workflow YAML.

``prepare_release.py`` owns version and tag selection. This module owns the
publication decision after a tag has been selected: exact checkout identity,
same-run retries, main-advance handling, atomic publication, ambiguous push
recovery, and Pages eligibility.
"""

from __future__ import annotations

import argparse
import base64
import os
import subprocess
import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

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
    push_attempted: bool = False
    push_succeeded: bool | None = None
    updated_main_sha: str | None = None
    updated_tag_sha: str | None = None
    release_is_ancestor_of_updated_main: bool = False
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
        return Decision(Action.ATTEMPT_PUBLISH, "Remote state permits atomic publication.")

    if facts.push_succeeded:
        return Decision(
            Action.PUBLISHED,
            f"Published {facts.release_tag} at {facts.release_sha}.",
            published=True,
        )

    updated_main = facts.updated_main_sha or ""
    if (
        facts.updated_tag_sha == facts.release_sha
        and facts.release_is_ancestor_of_updated_main
    ):
        if not facts.updated_marker_matches:
            return Decision(
                Action.FAIL,
                f"{facts.release_tag} reached the expected commit without this run's "
                "publication marker.",
            )
        return Decision(
            Action.RECOVER,
            "Recovered successful atomic publication after the client lost its "
            f"response; main is at {updated_main}.",
            published=True,
        )

    if updated_main != facts.base_sha:
        return Decision(
            Action.SKIP_MAIN_ADVANCED,
            f"main advanced from {facts.base_sha} to {updated_main} during publication; "
            "the newer merge has its own queued release.",
        )

    return Decision(
        Action.FAIL,
        f"Atomic release publication failed while main remained at {facts.base_sha}.",
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


def remote_annotated_tag_sha(root: Path, remote: str, release_tag: str) -> str | None:
    result = run_git(
        root,
        "ls-remote",
        "--tags",
        remote,
        f"refs/tags/{release_tag}^{{}}",
    )
    first = result.stdout.splitlines()
    return first[0].split()[0] if first else None


def fetch_release_refs(root: Path, remote: str) -> None:
    run_git(root, "fetch", "--force", remote, "main")
    run_git(root, "fetch", "--force", "--tags", remote)


def latest_release_tag(root: Path) -> str:
    tags = prepare_release.strict_version_tags(root)
    return tags[-1].name if tags else ""


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
    if not release_tag.startswith("v") or not prepare_release.SEMVER_RE.fullmatch(
        release_tag[1:]
    ):
        raise PublicationError(
            f"invalid release tag {release_tag!r}; expected vMAJOR.MINOR.PATCH"
        )
    if not prepare_release.RUN_ID_RE.fullmatch(release_run_id):
        raise PublicationError(f"invalid GitHub Actions run ID: {release_run_id!r}")


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
) -> PublicationResult:
    """Gather repository facts, perform at most one atomic push, and report outputs."""

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

    fetch_release_refs(root, remote)
    remote_main_sha = run_git(root, "rev-parse", f"{remote}/main").stdout.strip()
    existing_tag_sha = resolve_commit(root, release_tag)
    pre_push = decide_publication(
        PublicationFacts(
            release_sha=release_sha,
            base_sha=base_sha,
            release_tag=release_tag,
            head_sha=head_sha,
            dirty=False,
            remote_main_sha=remote_main_sha,
            existing_tag_sha=existing_tag_sha,
            existing_marker_matches=(
                marker_matches(root, release_tag, release_run_id)
                if existing_tag_sha is not None
                else False
            ),
        )
    )

    if pre_push.action is Action.FAIL:
        raise PublicationError(pre_push.diagnostic)
    if pre_push.action is Action.SKIP_MAIN_ADVANCED:
        print(f"::notice::{pre_push.diagnostic}")
        write_outputs(output_path, {"published": "false", "deploy_pages": "false"})
        return PublicationResult(pre_push.action, False, False, release_sha)

    terminal = pre_push
    if pre_push.action is Action.REUSE:
        print(pre_push.diagnostic)
    else:
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

        push_config: tuple[str, ...] = ()
        if github_token:
            auth = base64.b64encode(
                f"x-access-token:{github_token}".encode("utf-8")
            ).decode("ascii")
            push_config = (
                f"http.https://github.com/.extraheader=AUTHORIZATION: basic {auth}",
            )
        push = run_git(
            root,
            "push",
            "--atomic",
            remote,
            "HEAD:refs/heads/main",
            f"refs/tags/{release_tag}:refs/tags/{release_tag}",
            check=False,
            extra_config=push_config,
        )
        if push.returncode == 0:
            terminal = decide_publication(
                PublicationFacts(
                    release_sha=release_sha,
                    base_sha=base_sha,
                    release_tag=release_tag,
                    head_sha=head_sha,
                    dirty=False,
                    remote_main_sha=remote_main_sha,
                    push_attempted=True,
                    push_succeeded=True,
                )
            )
        else:
            fetch_release_refs(root, remote)
            updated_main_sha = run_git(
                root, "rev-parse", f"{remote}/main"
            ).stdout.strip()
            updated_tag_sha = remote_annotated_tag_sha(root, remote, release_tag)
            terminal = decide_publication(
                PublicationFacts(
                    release_sha=release_sha,
                    base_sha=base_sha,
                    release_tag=release_tag,
                    head_sha=head_sha,
                    dirty=False,
                    remote_main_sha=remote_main_sha,
                    push_attempted=True,
                    push_succeeded=False,
                    updated_main_sha=updated_main_sha,
                    updated_tag_sha=updated_tag_sha,
                    release_is_ancestor_of_updated_main=is_ancestor(
                        root, release_sha, f"{remote}/main"
                    ),
                    updated_marker_matches=(
                        marker_matches(root, release_tag, release_run_id)
                        if updated_tag_sha is not None
                        else False
                    ),
                )
            )

        if terminal.action is Action.FAIL:
            raise PublicationError(terminal.diagnostic)
        if terminal.action is Action.SKIP_MAIN_ADVANCED:
            print(f"::notice::{terminal.diagnostic}")
            write_outputs(output_path, {"published": "false", "deploy_pages": "false"})
            return PublicationResult(terminal.action, False, False, release_sha)
        prefix = "::notice::" if terminal.action is Action.RECOVER else ""
        print(f"{prefix}{terminal.diagnostic}")

    latest_tag = latest_release_tag(root)
    deploy_pages = should_deploy_pages(release_tag, latest_tag)
    if not deploy_pages:
        print(
            f"::notice::Repairing {release_tag} assets without replacing the newer "
            f"{latest_tag} Pages installer."
        )
    write_outputs(
        output_path,
        {
            "deploy_pages": "true" if deploy_pages else "false",
            "sha": release_sha,
            "published": "true",
        },
    )
    return PublicationResult(terminal.action, True, deploy_pages, release_sha)


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
        )
    except (OSError, PublicationError) as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
