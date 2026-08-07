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
        "dependabot[bot]",
        "github",
        "github-actions[bot]",
        "v1simple",
    }
)
IDENTITY_PATTERN = re.compile(r"^(.*?)\s*<([^<>]+)>")


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


def annotated_tag_metadata(repo: Path) -> list[tuple[str, str, str]]:
    output = run_git(
        repo,
        "for-each-ref",
        "refs/tags",
        "--format=%(objectname)%00%(objecttype)%00%(taggername)%00%(taggeremail)%00",
    )
    fields = output.split("\x00")
    if fields and fields[-1].strip() == "":
        fields.pop()
    if len(fields) % 4 != 0:
        raise RuntimeError("Git returned malformed tag metadata")
    return [
        (
            fields[index].lstrip("\n"),
            fields[index + 2],
            fields[index + 3].strip("<>"),
        )
        for index in range(0, len(fields), 4)
        if fields[index + 1] == "tag"
    ]


def is_shallow_repository(repo: Path) -> bool:
    value = run_git(repo, "rev-parse", "--is-shallow-repository").strip()
    if value not in {"true", "false"}:
        raise RuntimeError("Git returned an invalid shallow-repository state")
    return value == "true"


def violations(repo: Path, revision: str = "HEAD") -> list[str]:
    if is_shallow_repository(repo):
        return ["repository is shallow; full commit and tag history is required"]
    errors: list[str] = []
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
    for tag_object, tagger_name, tagger_email in annotated_tag_metadata(repo):
        if not is_public_safe_name(tagger_name):
            errors.append(f"{tag_object}: annotated-tag name is not privacy-safe")
        if not is_public_safe_email(tagger_email):
            errors.append(f"{tag_object}: annotated-tag email is not privacy-safe")
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
    args = parser.parse_args()

    try:
        repo = args.repo.resolve()
        errors = identity_violations(repo) if args.identity_only else violations(repo, args.revision)
    except RuntimeError as exc:
        print(f"[public-commit-metadata] ERROR: {exc}", file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(f"[public-commit-metadata] ERROR: {error}", file=sys.stderr)
        return 1
    checked = "configured identity" if args.identity_only else "public metadata"
    print(f"[public-commit-metadata] {checked} is privacy-safe")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
