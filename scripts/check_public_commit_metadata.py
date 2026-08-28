#!/usr/bin/env python3
"""Reject private identities in commit and annotated-tag metadata."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


ALLOWED_EMAILS = frozenset(
    {
        "noreply@example.invalid",
        "noreply@github.com",
    }
)
ALLOWED_SUFFIXES = ("@users.noreply.github.com",)
ALLOWED_NAMES = frozenset(
    {
        "contributor",
        "github",
        "github-actions[bot]",
        "v1simple",
    }
)
IDENTITY_PATTERN = re.compile(r"^(.*?)\s*<([^<>]+)>")
OBJECT_ID_PATTERN = re.compile(r"^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$")


def is_public_safe_email(value: str) -> bool:
    normalized = value.strip().lower()
    return normalized in ALLOWED_EMAILS or normalized.endswith(ALLOWED_SUFFIXES)


def is_public_safe_name(value: str) -> bool:
    return value.strip().casefold() in ALLOWED_NAMES


def run_git(repo: Path, *arguments: str) -> str:
    environment = os.environ.copy()
    environment["GIT_NO_REPLACE_OBJECTS"] = "1"
    environment["GIT_NO_LAZY_FETCH"] = "1"
    completed = subprocess.run(
        ["git", "-C", str(repo), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        encoding="utf-8",
        env=environment,
    )
    if completed.returncode != 0:
        raise RuntimeError("Git metadata inspection failed")
    return completed.stdout


def resolve_object(repo: Path, revision: str) -> str:
    value = run_git(repo, "rev-parse", "--verify", "--end-of-options", revision).strip()
    if not OBJECT_ID_PATTERN.fullmatch(value):
        raise RuntimeError("Git returned an invalid object identifier")
    return value.lower()


def git_object_type(repo: Path, object_id: str) -> str:
    value = run_git(repo, "cat-file", "-t", object_id).strip()
    if value not in {"blob", "commit", "tag", "tree"}:
        raise RuntimeError("Git returned an invalid object type")
    return value


def configured_identity(repo: Path, variable: str) -> tuple[str, str]:
    output = run_git(repo, "var", variable).strip()
    match = IDENTITY_PATTERN.search(output)
    if not match:
        raise RuntimeError(f"Git returned malformed {variable} metadata")
    return match.group(1).strip(), match.group(2)


def identity_violations(repo: Path) -> list[str]:
    errors: list[str] = []
    for role, variable in (
        ("author", "GIT_AUTHOR_IDENT"),
        ("committer", "GIT_COMMITTER_IDENT"),
    ):
        name, email = configured_identity(repo, variable)
        if not is_public_safe_name(name):
            errors.append(f"configured {role} name is not privacy-safe")
        if not is_public_safe_email(email):
            errors.append(f"configured {role} email is not privacy-safe")
    return errors


def commit_metadata(repo: Path, revision: str) -> list[tuple[str, str, str, str, str]]:
    output = run_git(
        repo,
        "log",
        revision,
        "--format=%H%x00%an%x00%ae%x00%cn%x00%ce%x00",
    )
    fields = output.split("\x00")
    if fields and fields[-1].strip() == "":
        fields.pop()
    if len(fields) % 5 != 0:
        raise RuntimeError("Git returned malformed commit metadata")
    return [
        (
            fields[index].lstrip("\n"),
            fields[index + 1],
            fields[index + 2],
            fields[index + 3],
            fields[index + 4],
        )
        for index in range(0, len(fields), 5)
    ]


def annotated_tag_ref_objects(repo: Path) -> list[str]:
    output = run_git(
        repo,
        "for-each-ref",
        "refs/tags",
        "--format=%(objectname)%00%(objecttype)%00",
    )
    fields = output.split("\x00")
    if fields and fields[-1].strip() == "":
        fields.pop()
    if len(fields) % 2 != 0:
        raise RuntimeError("Git returned malformed tag metadata")
    return [
        fields[index].lstrip("\n")
        for index in range(0, len(fields), 2)
        if fields[index + 1] == "tag"
    ]


def annotated_tag_object_metadata(repo: Path, object_id: str) -> tuple[str, str, str]:
    output = run_git(repo, "cat-file", "tag", object_id)
    tagger_line = next(
        (
            line.removeprefix("tagger ")
            for line in output.splitlines()
            if line.startswith("tagger ")
        ),
        None,
    )
    if tagger_line is None:
        raise RuntimeError("Annotated tag has no tagger identity")
    match = IDENTITY_PATTERN.search(tagger_line)
    if not match:
        raise RuntimeError("Annotated tag has malformed tagger identity")
    return object_id, match.group(1).strip(), match.group(2)


def annotated_tag_target(repo: Path, object_id: str) -> str:
    output = run_git(repo, "cat-file", "tag", object_id)
    target_line = next(
        (line.removeprefix("object ") for line in output.splitlines() if line.startswith("object ")),
        None,
    )
    if target_line is None or not OBJECT_ID_PATTERN.fullmatch(target_line):
        raise RuntimeError("Annotated tag has a malformed target object")
    return target_line.lower()


def annotated_tag_chain(repo: Path, revision: str) -> tuple[list[str], str, str]:
    """Return every tag object before the final peeled target, rejecting cycles."""

    object_id = resolve_object(repo, revision)
    tags: list[str] = []
    seen: set[str] = set()
    while git_object_type(repo, object_id) == "tag":
        if object_id in seen:
            raise RuntimeError("Annotated tag chain contains a cycle")
        seen.add(object_id)
        tags.append(object_id)
        object_id = annotated_tag_target(repo, object_id)
    return tags, object_id, git_object_type(repo, object_id)


def is_shallow_repository(repo: Path) -> bool:
    value = run_git(repo, "rev-parse", "--is-shallow-repository").strip()
    if value not in {"true", "false"}:
        raise RuntimeError("Git returned an invalid shallow-repository state")
    return value == "true"


def append_commit_violations(
    errors: list[str],
    repo: Path,
    revision: str,
) -> None:
    for commit, author_name, author_email, committer_name, committer_email in commit_metadata(
        repo, revision
    ):
        if not is_public_safe_name(author_name):
            errors.append(f"{commit}: author name is not privacy-safe")
        if not is_public_safe_email(author_email):
            errors.append(f"{commit}: author email is not privacy-safe")
        if not is_public_safe_name(committer_name):
            errors.append(f"{commit}: committer name is not privacy-safe")
        if not is_public_safe_email(committer_email):
            errors.append(f"{commit}: committer email is not privacy-safe")


def append_tag_violations(
    errors: list[str],
    metadata: tuple[str, str, str],
) -> None:
    tag_object, tagger_name, tagger_email = metadata
    if not is_public_safe_name(tagger_name):
        errors.append(f"{tag_object}: annotated-tag name is not privacy-safe")
    if not is_public_safe_email(tagger_email):
        errors.append(f"{tag_object}: annotated-tag email is not privacy-safe")


def violations(repo: Path, revision: str = "HEAD") -> list[str]:
    if is_shallow_repository(repo):
        return ["repository is shallow; full commit and tag history is required"]
    errors: list[str] = []
    append_commit_violations(errors, repo, revision)
    seen_tags: set[str] = set()
    for tag_ref_object in annotated_tag_ref_objects(repo):
        tags, _target, _target_type = annotated_tag_chain(repo, tag_ref_object)
        for tag_object in tags:
            if tag_object not in seen_tags:
                append_tag_violations(
                    errors, annotated_tag_object_metadata(repo, tag_object)
                )
                seen_tags.add(tag_object)
    return errors


def object_violations(repo: Path, revisions: list[str]) -> list[str]:
    """Inspect exact proposed ref targets, including not-yet-visible tag objects."""
    if is_shallow_repository(repo):
        return ["repository is shallow; full commit and tag history is required"]
    errors: list[str] = []
    seen_commits: set[str] = set()
    seen_tags: set[str] = set()
    for revision in revisions:
        tags, target_id, target_kind = annotated_tag_chain(repo, revision)
        for tag_object in tags:
            if tag_object not in seen_tags:
                append_tag_violations(
                    errors, annotated_tag_object_metadata(repo, tag_object)
                )
                seen_tags.add(tag_object)
        if target_kind != "commit":
            errors.append(f"{target_id}: reference target does not peel to a commit")
            continue
        if target_id not in seen_commits:
            append_commit_violations(errors, repo, target_id)
            seen_commits.add(target_id)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--revision", default="HEAD")
    parser.add_argument(
        "--identity-only",
        action="store_true",
        help="check the resolved author and committer identity without inspecting history",
    )
    parser.add_argument(
        "--object",
        action="append",
        default=[],
        help="check an exact proposed commit or annotated-tag object (repeatable)",
    )
    args = parser.parse_args()

    if args.identity_only and args.object:
        parser.error("--identity-only and --object are mutually exclusive")

    try:
        repo = args.repo.resolve()
        if args.identity_only:
            errors = identity_violations(repo)
        elif args.object:
            errors = object_violations(repo, args.object)
        else:
            errors = violations(repo, args.revision)
    except RuntimeError as exc:
        print(f"[public-commit-metadata] ERROR: {exc}", file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(f"[public-commit-metadata] ERROR: {error}", file=sys.stderr)
        return 1
    checked = (
        "configured identity"
        if args.identity_only
        else "proposed object metadata"
        if args.object
        else "public metadata"
    )
    print(f"[public-commit-metadata] {checked} is privacy-safe")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
