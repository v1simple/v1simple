#!/usr/bin/env python3
"""Regression tests for the public commit-metadata privacy guard."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

import check_public_commit_metadata as checker


def git(repo: Path, *arguments: str, environment: dict[str, str] | None = None) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repo), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    return completed.stdout.decode("utf-8").strip()


def make_repo(base: Path) -> Path:
    repo = base / "repo"
    repo.mkdir()
    git(repo, "init", "-q")
    git(repo, "config", "user.name", "v1simple")
    git(repo, "config", "user.email", "noreply@example.invalid")
    (repo / "tracked.txt").write_text("fixture\n", encoding="utf-8")
    git(repo, "add", "tracked.txt")
    git(repo, "commit", "-q", "-m", "fixture")
    return repo


def test_allows_project_and_github_noreply_identities() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_AUTHOR_NAME": "contributor",
                "GIT_AUTHOR_EMAIL": "12345+contributor@users.noreply.github.com",
                "GIT_COMMITTER_NAME": "GitHub",
                "GIT_COMMITTER_EMAIL": "noreply@github.com",
            }
        )
        (repo / "tracked.txt").write_text("updated\n", encoding="utf-8")
        git(repo, "commit", "-qam", "noreply fixture", environment=environment)
        assert checker.violations(repo) == []


def test_rejects_personal_commit_email_without_echoing_it() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        private_email = "person@personal.test"
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_AUTHOR_NAME": "private author",
                "GIT_AUTHOR_EMAIL": private_email,
                "GIT_COMMITTER_NAME": "private committer",
                "GIT_COMMITTER_EMAIL": private_email,
            }
        )
        (repo / "tracked.txt").write_text("private metadata\n", encoding="utf-8")
        git(repo, "commit", "-qam", "private fixture", environment=environment)
        errors = checker.violations(repo)
        assert len(errors) == 4
        assert all(private_email not in error for error in errors)
        assert all("private author" not in error for error in errors)
        assert all("private committer" not in error for error in errors)


def test_rejects_personal_annotated_tag_email() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_COMMITTER_NAME": "private tagger",
                "GIT_COMMITTER_EMAIL": "tagger@personal.test",
            }
        )
        git(repo, "tag", "-a", "private-tag", "-m", "fixture", environment=environment)
        errors = checker.violations(repo)
        assert len(errors) == 2
        assert errors[0].endswith(": annotated-tag name is not privacy-safe")
        assert errors[1].endswith(": annotated-tag email is not privacy-safe")
        assert all("private tagger" not in error for error in errors)
        assert all("private-tag" not in error for error in errors)


def test_rejects_personal_names_even_with_safe_email_addresses() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        private_name = "Private Person"
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_AUTHOR_NAME": private_name,
                "GIT_AUTHOR_EMAIL": "noreply@example.invalid",
                "GIT_COMMITTER_NAME": private_name,
                "GIT_COMMITTER_EMAIL": "noreply@example.invalid",
            }
        )
        git(repo, "commit", "--allow-empty", "-q", "-m", "private name", environment=environment)
        errors = checker.violations(repo)
        assert len(errors) == 2
        assert errors[0].endswith(": author name is not privacy-safe")
        assert errors[1].endswith(": committer name is not privacy-safe")
        assert all(private_name not in error for error in errors)


def test_cli_failure_does_not_print_personal_address() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        private_email = "hidden@personal.test"
        git(repo, "config", "user.email", private_email)
        (repo / "tracked.txt").write_text("private metadata\n", encoding="utf-8")
        git(repo, "commit", "-qam", "private fixture")
        completed = subprocess.run(
            [sys.executable, str(Path(checker.__file__)), "--repo", str(repo)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        assert completed.returncode == 1
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_rejects_shallow_history_before_claiming_full_metadata_coverage() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        base = Path(raw)
        source = make_repo(base)
        (source / "tracked.txt").write_text("second\n", encoding="utf-8")
        git(source, "commit", "-qam", "second fixture")
        shallow = base / "shallow"
        subprocess.run(
            [
                "git",
                "clone",
                "-q",
                "--depth",
                "1",
                source.as_uri(),
                str(shallow),
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert checker.violations(shallow) == [
            "repository is shallow; full commit and tag history is required"
        ]


def test_configured_identity_must_be_privacy_safe() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        assert checker.identity_violations(repo) == []
        git(repo, "config", "user.email", "configured@personal.test")
        errors = checker.identity_violations(repo)
        assert errors == [
            "configured author email is not privacy-safe",
            "configured committer email is not privacy-safe",
        ]
        assert all("configured@personal.test" not in error for error in errors)


def test_identity_cli_failure_does_not_print_personal_address() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        private_email = "configured-hidden@personal.test"
        git(repo, "config", "user.email", private_email)
        completed = subprocess.run(
            [
                sys.executable,
                str(Path(checker.__file__)),
                "--repo",
                str(repo),
                "--identity-only",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        assert completed.returncode == 1
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_exact_unreferenced_commit_object_is_checked() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        private_email = "unreferenced-commit@personal.test"
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_AUTHOR_NAME": "private author",
                "GIT_AUTHOR_EMAIL": private_email,
                "GIT_COMMITTER_NAME": "private committer",
                "GIT_COMMITTER_EMAIL": private_email,
            }
        )
        git(
            repo,
            "commit",
            "--allow-empty",
            "-q",
            "-m",
            "private object",
            environment=environment,
        )
        object_id = git(repo, "rev-parse", "HEAD")
        git(repo, "reset", "--hard", "-q", "HEAD^")
        errors = checker.object_violations(repo, [object_id])
        assert len(errors) == 4
        assert all(private_email not in error for error in errors)


def test_exact_unreferenced_annotated_tag_object_is_checked() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        private_email = "unreferenced-tag@personal.test"
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_COMMITTER_NAME": "private tagger",
                "GIT_COMMITTER_EMAIL": private_email,
            }
        )
        git(repo, "tag", "-a", "temporary-tag", "-m", "fixture", environment=environment)
        object_id = git(repo, "rev-parse", "refs/tags/temporary-tag")
        git(repo, "tag", "-d", "temporary-tag")
        errors = checker.object_violations(repo, [object_id])
        assert len(errors) == 2
        assert errors[0].endswith(": annotated-tag name is not privacy-safe")
        assert errors[1].endswith(": annotated-tag email is not privacy-safe")
        assert all(private_email not in error for error in errors)


def test_safe_outer_tag_does_not_hide_unsafe_inner_tag_identity() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        private_email = "nested-tagger@personal.test"
        unsafe_environment = os.environ.copy()
        unsafe_environment.update(
            {
                "GIT_COMMITTER_NAME": "private tagger",
                "GIT_COMMITTER_EMAIL": private_email,
            }
        )
        git(
            repo,
            "tag",
            "-a",
            "inner-private",
            "-m",
            "safe fixture",
            environment=unsafe_environment,
        )
        inner = git(repo, "rev-parse", "refs/tags/inner-private")
        git(repo, "tag", "-d", "inner-private")
        git(repo, "tag", "-a", "outer-safe", inner, "-m", "safe outer fixture")

        errors = checker.violations(repo)
        assert len(errors) == 2
        assert all(error.startswith(inner) for error in errors)
        assert all(private_email not in error for error in errors)


def test_safe_nested_tags_and_ordinary_commits_pass() -> None:
    with tempfile.TemporaryDirectory(prefix="public-metadata-") as raw:
        repo = make_repo(Path(raw))
        git(repo, "tag", "-a", "inner-safe", "-m", "safe inner fixture")
        inner = git(repo, "rev-parse", "refs/tags/inner-safe")
        git(repo, "tag", "-d", "inner-safe")
        git(repo, "tag", "-a", "outer-safe", inner, "-m", "safe outer fixture")

        outer = git(repo, "rev-parse", "refs/tags/outer-safe")
        assert checker.violations(repo) == []
        assert checker.object_violations(repo, [outer]) == []
        assert checker.object_violations(repo, ["HEAD"]) == []


def test_annotated_tag_cycle_is_rejected_defensively() -> None:
    first = "a" * 40
    second = "b" * 40
    originals = (
        checker.resolve_object,
        checker.git_object_type,
        checker.annotated_tag_target,
    )
    checker.resolve_object = lambda _repo, _revision: first
    checker.git_object_type = lambda _repo, _object_id: "tag"
    checker.annotated_tag_target = (  # type: ignore[assignment]
        lambda _repo, object_id: second if object_id == first else first
    )
    try:
        try:
            checker.annotated_tag_chain(Path("."), "fixture")
        except RuntimeError as exc:
            assert "cycle" in str(exc)
        else:
            raise AssertionError("annotated tag cycle was accepted")
    finally:
        (
            checker.resolve_object,
            checker.git_object_type,
            checker.annotated_tag_target,
        ) = originals


def main() -> int:
    tests = (
        test_allows_project_and_github_noreply_identities,
        test_rejects_personal_commit_email_without_echoing_it,
        test_rejects_personal_annotated_tag_email,
        test_rejects_personal_names_even_with_safe_email_addresses,
        test_cli_failure_does_not_print_personal_address,
        test_rejects_shallow_history_before_claiming_full_metadata_coverage,
        test_configured_identity_must_be_privacy_safe,
        test_identity_cli_failure_does_not_print_personal_address,
        test_exact_unreferenced_commit_object_is_checked,
        test_exact_unreferenced_annotated_tag_object_is_checked,
        test_safe_outer_tag_does_not_hide_unsafe_inner_tag_identity,
        test_safe_nested_tags_and_ordinary_commits_pass,
        test_annotated_tag_cycle_is_rejected_defensively,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} public commit-metadata regression tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
