#!/usr/bin/env python3
"""Run-on-change regression tests for public privacy-boundary hooks."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PRE_COMMIT = ROOT / ".githooks" / "pre-commit"
PRE_PUSH = ROOT / ".githooks" / "pre-push"
COMMIT_MSG = ROOT / ".githooks" / "commit-msg"
COMMIT_MSG_CHECK = ROOT / "scripts" / "check_commit_msg.py"
METADATA_CHECK = ROOT / "scripts" / "check_public_commit_metadata.py"
SNAPSHOT_CHECK = ROOT / "scripts" / "check_public_snapshot_privacy.py"
ZERO = "0" * 40


def run(
    arguments: list[str],
    *,
    cwd: Path,
    input_text: str | None = None,
    env: dict[str, str] | None = None,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        cwd=cwd,
        input=input_text,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        env=env,
        timeout=timeout,
    )


def git(repo: Path, *arguments: str) -> str:
    completed = run(["git", *arguments], cwd=repo)
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed.stdout.strip()


def make_repo(base: Path) -> Path:
    repo = base / "repo"
    (repo / ".githooks").mkdir(parents=True)
    (repo / "scripts").mkdir()
    shutil.copy2(PRE_COMMIT, repo / ".githooks" / "pre-commit")
    shutil.copy2(PRE_PUSH, repo / ".githooks" / "pre-push")
    shutil.copy2(COMMIT_MSG, repo / ".githooks" / "commit-msg")
    shutil.copy2(COMMIT_MSG_CHECK, repo / "scripts" / COMMIT_MSG_CHECK.name)
    shutil.copy2(METADATA_CHECK, repo / "scripts" / METADATA_CHECK.name)
    shutil.copy2(SNAPSHOT_CHECK, repo / "scripts" / SNAPSHOT_CHECK.name)
    git(repo, "init", "-q")
    git(repo, "config", "user.name", "v1simple")
    git(repo, "config", "user.email", "noreply@example.invalid")
    (repo / "tracked.txt").write_text("fixture\n", encoding="utf-8")
    git(repo, "add", "tracked.txt")
    git(repo, "commit", "-q", "-m", "fixture")
    return repo


def invoke_pre_push(
    repo: Path,
    *,
    local_ref: str,
    local_sha: str,
    remote_ref: str,
    remote_sha: str = ZERO,
) -> subprocess.CompletedProcess[str]:
    return run(
        [str(repo / ".githooks" / "pre-push"), "origin", "unused"],
        cwd=repo,
        input_text=f"{local_ref} {local_sha} {remote_ref} {remote_sha}\n",
    )


def invoke_commit_msg(repo: Path, message: str) -> subprocess.CompletedProcess[str]:
    message_file = repo / "COMMIT_EDITMSG"
    message_file.write_text(message, encoding="utf-8")
    return run([str(repo / ".githooks" / "commit-msg"), str(message_file)], cwd=repo)


def test_commit_msg_accepts_safe_conventional_message() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        completed = invoke_commit_msg(repo, "ci(release): scan complete publication history\n")
        assert completed.returncode == 0, completed.stderr


def test_commit_msg_blocks_private_text_before_validator_can_echo_it() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        private_email = "message" + "@" + "corp.com"
        completed = invoke_commit_msg(
            repo,
            f"ci(release): protect message metadata\n\nReviewed-by: {private_email}\n",
        )
        assert completed.returncode != 0
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_pre_commit_accepts_public_identity() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        completed = run([str(repo / ".githooks" / "pre-commit")], cwd=repo)
        assert completed.returncode == 0, completed.stderr


def test_pre_commit_blocks_personal_identity_without_echoing_it() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        private_email = "blocked@personal.test"
        git(repo, "config", "user.email", private_email)
        completed = run([str(repo / ".githooks" / "pre-commit")], cwd=repo)
        assert completed.returncode != 0
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_pre_commit_blocks_unsafe_staged_content_without_echoing_it() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        private_email = "staged" + "@" + "corp.com"
        (repo / "profile.txt").write_text(private_email, encoding="utf-8")
        git(repo, "add", "profile.txt")
        completed = run([str(repo / ".githooks" / "pre-commit")], cwd=repo)
        assert completed.returncode != 0
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_pre_commit_blocks_force_added_replay_data_without_echoing_it() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        private_value = "private-replay-value-must-not-be-echoed"
        capture = repo / "tools" / "v1replay" / "captures" / "input.json"
        capture.parent.mkdir(parents=True)
        capture.write_text(private_value, encoding="utf-8")
        git(repo, "add", "-f", str(capture.relative_to(repo)))
        completed = run([str(repo / ".githooks" / "pre-commit")], cwd=repo)
        assert completed.returncode != 0
        assert private_value not in completed.stdout
        assert private_value not in completed.stderr


def test_pre_push_accepts_safe_main_history() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        head = git(repo, "rev-parse", "HEAD")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
        )
        assert completed.returncode == 0, completed.stderr


def test_pre_push_accepts_a_safe_existing_remote_range() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        remote_head = git(repo, "rev-parse", "HEAD")
        (repo / "safe.txt").write_text("safe update\n", encoding="utf-8")
        git(repo, "add", "safe.txt")
        git(repo, "commit", "-q", "-m", "safe update")
        head = git(repo, "rev-parse", "HEAD")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
            remote_sha=remote_head,
        )
        assert completed.returncode == 0, completed.stderr


def test_pre_push_blocks_deleted_intermediate_content_in_existing_range() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        remote_head = git(repo, "rev-parse", "HEAD")
        private_email = "intermediate" + "@" + "corp.com"
        (repo / "temporary.txt").write_text(private_email, encoding="utf-8")
        git(repo, "add", "temporary.txt")
        git(repo, "commit", "-q", "-m", "temporary content")
        (repo / "temporary.txt").unlink()
        git(repo, "add", "-u")
        git(repo, "commit", "-q", "-m", "remove temporary content")
        head = git(repo, "rev-parse", "HEAD")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
            remote_sha=remote_head,
        )
        assert completed.returncode != 0
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_pre_push_allows_only_semantic_release_tags() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        head = git(repo, "rev-parse", "HEAD")
        private_ref = "refs/tags/reviewer-" + "hidden" + "@" + "corp.com"
        blocked = invoke_pre_push(
            repo,
            local_ref=private_ref,
            local_sha=head,
            remote_ref=private_ref,
        )
        assert blocked.returncode != 0
        assert "outside the public ref allowlist" in blocked.stderr
        assert "hidden" not in blocked.stderr

        git(repo, "tag", "-a", "v1.2.3", "-m", "safe release")
        tag_object = git(repo, "rev-parse", "refs/tags/v1.2.3")
        allowed = invoke_pre_push(
            repo,
            local_ref="refs/tags/v1.2.3",
            local_sha=tag_object,
            remote_ref="refs/tags/v1.2.3",
        )
        assert allowed.returncode == 0, allowed.stderr


def test_pre_push_blocks_absolute_hooks_path() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        absolute_hooks_path = str(repo / ".githooks")
        git(repo, "config", "core.hooksPath", absolute_hooks_path)
        head = git(repo, "rev-parse", "HEAD")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
        )
        assert completed.returncode != 0
        assert "core.hooksPath is an absolute path" in completed.stderr
        assert absolute_hooks_path not in completed.stderr


def test_pre_push_blocks_failed_scanner_parity_without_echoing_details() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        parity = repo / "scripts" / "test_scanner_parity.py"
        parity.write_text(
            'print("private-parity-detail")\nraise SystemExit(1)\n',
            encoding="utf-8",
        )
        head = git(repo, "rev-parse", "HEAD")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
        )
        assert completed.returncode != 0
        assert "pinned canonical digest" in completed.stderr
        assert "private-parity-detail" not in completed.stdout
        assert "private-parity-detail" not in completed.stderr


def test_pre_push_blocks_personal_history_without_echoing_it() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        private_email = "history@personal.test"
        git(repo, "config", "user.email", private_email)
        (repo / "tracked.txt").write_text("private history\n", encoding="utf-8")
        git(repo, "commit", "-qam", "private fixture")
        head = git(repo, "rev-parse", "HEAD")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
        )
        assert completed.returncode != 0
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_pre_push_blocks_non_public_branch() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        head = git(repo, "rev-parse", "HEAD")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/dev/private",
            local_sha=head,
            remote_ref="refs/heads/dev/private",
        )
        assert completed.returncode != 0
        assert "outside the public ref allowlist" in completed.stderr


def test_pre_push_blocks_unsafe_committed_content_without_echoing_it() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        private_email = "committed" + "@" + "corp.com"
        (repo / "profile.txt").write_text(private_email, encoding="utf-8")
        git(repo, "add", "profile.txt")
        git(repo, "commit", "-q", "-m", "private content fixture")
        head = git(repo, "rev-parse", "HEAD")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
        )
        assert completed.returncode != 0
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_pre_push_blocks_committed_replay_data_without_echoing_it() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        private_value = "private-replay-value-must-not-be-echoed"
        capture = repo / "tools" / "v1replay" / "captures" / "input.json"
        capture.parent.mkdir(parents=True)
        capture.write_text(private_value, encoding="utf-8")
        git(repo, "add", "-f", str(capture.relative_to(repo)))
        git(repo, "commit", "-q", "-m", "private replay fixture")
        head = git(repo, "rev-parse", "HEAD")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
        )
        assert completed.returncode != 0
        assert private_value not in completed.stdout
        assert private_value not in completed.stderr


def main() -> int:
    tests = (
        test_commit_msg_accepts_safe_conventional_message,
        test_commit_msg_blocks_private_text_before_validator_can_echo_it,
        test_pre_commit_accepts_public_identity,
        test_pre_commit_blocks_personal_identity_without_echoing_it,
        test_pre_commit_blocks_unsafe_staged_content_without_echoing_it,
        test_pre_commit_blocks_force_added_replay_data_without_echoing_it,
        test_pre_push_accepts_safe_main_history,
        test_pre_push_accepts_a_safe_existing_remote_range,
        test_pre_push_blocks_deleted_intermediate_content_in_existing_range,
        test_pre_push_allows_only_semantic_release_tags,
        test_pre_push_blocks_absolute_hooks_path,
        test_pre_push_blocks_failed_scanner_parity_without_echoing_details,
        test_pre_push_blocks_personal_history_without_echoing_it,
        test_pre_push_blocks_non_public_branch,
        test_pre_push_blocks_unsafe_committed_content_without_echoing_it,
        test_pre_push_blocks_committed_replay_data_without_echoing_it,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} public privacy hook regression tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
