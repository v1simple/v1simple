#!/usr/bin/env python3
"""Regression tests for exact tracked-source CI qualification."""

from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile

import check_tracked_source_state as checker


ROOT = Path(__file__).resolve().parent.parent


def git(repo: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repo), *arguments],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed.stdout.strip()


def make_repo(path: Path) -> Path:
    path.mkdir()
    git(path, "init", "-q")
    git(path, "config", "user.name", "v1simple")
    git(path, "config", "user.email", "noreply@example.invalid")
    (path / "tracked.txt").write_text("committed\n", encoding="utf-8")
    git(path, "add", "tracked.txt")
    git(path, "commit", "-q", "-m", "fixture")
    return path


def test_clean_tree_passes() -> None:
    with tempfile.TemporaryDirectory(prefix="tracked-source-") as raw:
        repo = make_repo(Path(raw) / "repo")
        assert checker.tracked_source_is_clean(repo)


def test_untracked_and_ignored_outputs_pass() -> None:
    with tempfile.TemporaryDirectory(prefix="tracked-source-") as raw:
        repo = make_repo(Path(raw) / "repo")
        (repo / ".gitignore").write_text("ignored/\n", encoding="utf-8")
        git(repo, "add", ".gitignore")
        git(repo, "commit", "-q", "-m", "ignore generated output")
        (repo / "untracked.txt").write_text("generated\n", encoding="utf-8")
        (repo / "ignored").mkdir()
        (repo / "ignored" / "output.txt").write_text("generated\n", encoding="utf-8")
        assert checker.tracked_source_is_clean(repo)


def test_staged_and_unstaged_tracked_changes_fail() -> None:
    with tempfile.TemporaryDirectory(prefix="tracked-source-") as raw:
        repo = make_repo(Path(raw) / "repo")
        tracked = repo / "tracked.txt"
        tracked.write_text("unstaged\n", encoding="utf-8")
        assert not checker.tracked_source_is_clean(repo)
        git(repo, "add", "tracked.txt")
        assert not checker.tracked_source_is_clean(repo)


def test_tracked_submodule_changes_fail_but_untracked_content_passes() -> None:
    with tempfile.TemporaryDirectory(prefix="tracked-source-") as raw:
        base = Path(raw)
        child = make_repo(base / "child")
        parent = make_repo(base / "parent")
        git(
            parent,
            "-c",
            "protocol.file.allow=always",
            "submodule",
            "add",
            "-q",
            str(child),
            "dependency",
        )
        git(parent, "commit", "-q", "-am", "add submodule")

        dependency = parent / "dependency"
        (dependency / "untracked.txt").write_text("generated\n", encoding="utf-8")
        assert checker.tracked_source_is_clean(parent)
        (dependency / "tracked.txt").write_text("modified\n", encoding="utf-8")
        assert not checker.tracked_source_is_clean(parent)


def test_ci_gate_tests_dirty_trees_but_qualifies_only_an_unchanged_clean_commit() -> None:
    gate = (ROOT / "scripts" / "ci-test.sh").read_text(encoding="utf-8")
    assert 'QUALIFICATION_SHA="$(git rev-parse HEAD)"' in gate
    assert "testing the current working tree" in gate
    assert '[[ -n "$QUALIFICATION_SHA"' in gate
    assert '"$(git rev-parse HEAD)" == "$QUALIFICATION_SHA"' in gate
    marker = 'printf \'%s\\n\' "$QUALIFICATION_SHA" > "$ROOT_DIR/.artifacts/ci-gate-passed.sha"'
    assert marker in gate


def main() -> int:
    tests = (
        test_clean_tree_passes,
        test_untracked_and_ignored_outputs_pass,
        test_staged_and_unstaged_tracked_changes_fail,
        test_tracked_submodule_changes_fail_but_untracked_content_passes,
        test_ci_gate_tests_dirty_trees_but_qualifies_only_an_unchanged_clean_commit,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} tracked source state regression tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
