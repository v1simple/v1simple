#!/usr/bin/env python3
"""Regression tests for the exact-commit local CI gate marker."""

from pathlib import Path
import tempfile

from test_public_privacy_hooks import git, invoke_pre_push, make_repo


def invoke_with_marker(marker: str | None):
    temporary = tempfile.TemporaryDirectory(prefix="pre-push-gate-marker-")
    repo = make_repo(Path(temporary.name))
    head = git(repo, "rev-parse", "HEAD")
    completed = invoke_pre_push(
        repo,
        local_ref="refs/heads/main",
        local_sha=head,
        remote_ref="refs/heads/main",
        gate_marker=marker,
    )
    return temporary, completed


def test_missing_marker_is_rejected() -> None:
    temporary, completed = invoke_with_marker(None)
    with temporary:
        assert completed.returncode != 0
        assert "has not passed at the commit being pushed" in completed.stderr


def test_different_commit_marker_is_rejected() -> None:
    temporary, completed = invoke_with_marker("1" * 40)
    with temporary:
        assert completed.returncode != 0
        assert "passed at a different commit" in completed.stderr


def test_malformed_marker_is_rejected() -> None:
    temporary, completed = invoke_with_marker("not-a-commit")
    with temporary:
        assert completed.returncode != 0
        assert "marker is malformed" in completed.stderr


def test_exact_commit_marker_is_accepted() -> None:
    temporary, completed = invoke_with_marker("MATCHING")
    with temporary:
        assert completed.returncode == 0, completed.stderr


def main() -> int:
    tests = (
        test_missing_marker_is_rejected,
        test_different_commit_marker_is_rejected,
        test_malformed_marker_is_rejected,
        test_exact_commit_marker_is_accepted,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} pre-push gate marker regression tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
