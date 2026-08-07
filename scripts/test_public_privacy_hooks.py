#!/usr/bin/env python3
"""Run-on-change regression tests for public privacy-boundary hooks."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PRE_COMMIT = ROOT / ".githooks" / "pre-commit"
PREPARE_COMMIT_MSG = ROOT / ".githooks" / "prepare-commit-msg"
PRE_PUSH = ROOT / ".githooks" / "pre-push"
COMMIT_MSG = ROOT / ".githooks" / "commit-msg"
REFERENCE_TRANSACTION = ROOT / ".githooks" / "reference-transaction"
COMMIT_MSG_CHECK = ROOT / "scripts" / "check_commit_msg.py"
METADATA_CHECK = ROOT / "scripts" / "check_public_commit_metadata.py"
SNAPSHOT_CHECK = ROOT / "scripts" / "check_public_snapshot_privacy.py"
SNAPSHOT_TEST = ROOT / "scripts" / "test_check_public_snapshot_privacy.py"
PARITY_CHECK = ROOT / "scripts" / "test_scanner_parity.py"
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
    shutil.copy2(PREPARE_COMMIT_MSG, repo / ".githooks" / "prepare-commit-msg")
    shutil.copy2(PRE_PUSH, repo / ".githooks" / "pre-push")
    shutil.copy2(COMMIT_MSG, repo / ".githooks" / "commit-msg")
    shutil.copy2(REFERENCE_TRANSACTION, repo / ".githooks" / "reference-transaction")
    shutil.copy2(COMMIT_MSG_CHECK, repo / "scripts" / COMMIT_MSG_CHECK.name)
    shutil.copy2(METADATA_CHECK, repo / "scripts" / METADATA_CHECK.name)
    shutil.copy2(SNAPSHOT_CHECK, repo / "scripts" / SNAPSHOT_CHECK.name)
    shutil.copy2(SNAPSHOT_TEST, repo / "scripts" / SNAPSHOT_TEST.name)
    shutil.copy2(PARITY_CHECK, repo / "scripts" / PARITY_CHECK.name)
    git(repo, "init", "-q")
    git(repo, "config", "user.name", "v1simple")
    git(repo, "config", "user.email", "noreply@example.invalid")
    (repo / "tracked.txt").write_text("fixture\n", encoding="utf-8")
    git(repo, "add", "tracked.txt")
    git(repo, "commit", "-q", "-m", "fixture")
    return repo


def enable_tracked_hooks(repo: Path) -> None:
    git(repo, "config", "core.hooksPath", ".githooks")


def object_ids(repo: Path, object_kind: str) -> set[str]:
    output = git(
        repo,
        "cat-file",
        "--batch-all-objects",
        "--batch-check=%(objectname) %(objecttype)",
    )
    return {
        object_id
        for object_id, observed_kind in (line.split() for line in output.splitlines())
        if observed_kind == object_kind
    }


def invoke_pre_push(
    repo: Path,
    *,
    local_ref: str,
    local_sha: str,
    remote_ref: str,
    remote_sha: str = ZERO,
    configure_hooks: bool = True,
) -> subprocess.CompletedProcess[str]:
    if configure_hooks:
        git(repo, "config", "core.hooksPath", ".githooks")
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


def test_reference_transaction_allows_safe_no_verify_commit() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        enable_tracked_hooks(repo)
        (repo / "safe.txt").write_text("safe update\n", encoding="utf-8")
        git(repo, "add", "safe.txt")
        completed = run(
            ["git", "commit", "--no-verify", "-m", "test(privacy): safe fixture"],
            cwd=repo,
        )
        assert completed.returncode == 0, completed.stderr


def test_no_verify_staged_pii_is_blocked_before_commit_object() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        enable_tracked_hooks(repo)
        original_head = git(repo, "rev-parse", "HEAD")
        original_commits = object_ids(repo, "commit")
        private_email = "blocked-reference" + "@" + "corp.com"
        (repo / "private.txt").write_text(private_email, encoding="utf-8")
        git(repo, "add", "private.txt")
        completed = run(
            ["git", "commit", "--no-verify", "-m", "test(privacy): blocked content"],
            cwd=repo,
        )
        assert completed.returncode != 0
        assert git(repo, "rev-parse", "HEAD") == original_head
        assert object_ids(repo, "commit") == original_commits
        assert "privacy preparation gate blocked this commit" in completed.stderr
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_no_verify_identity_override_is_blocked_before_commit_object() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        enable_tracked_hooks(repo)
        original_head = git(repo, "rev-parse", "HEAD")
        original_commits = object_ids(repo, "commit")
        private_email = "blocked-identity@personal.test"
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_AUTHOR_NAME": "private author",
                "GIT_AUTHOR_EMAIL": private_email,
                "GIT_COMMITTER_NAME": "private committer",
                "GIT_COMMITTER_EMAIL": private_email,
            }
        )
        completed = run(
            [
                "git",
                "commit",
                "--allow-empty",
                "--no-verify",
                "-m",
                "test(privacy): blocked identity",
            ],
            cwd=repo,
            env=environment,
        )
        assert completed.returncode != 0
        assert git(repo, "rev-parse", "HEAD") == original_head
        assert object_ids(repo, "commit") == original_commits
        assert "privacy preparation gate blocked this commit" in completed.stderr
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_no_verify_message_pii_is_blocked_before_commit_object() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        enable_tracked_hooks(repo)
        original_head = git(repo, "rev-parse", "HEAD")
        original_commits = object_ids(repo, "commit")
        private_email = "blocked-message" + "@" + "corp.com"
        completed = run(
            [
                "git",
                "commit",
                "--allow-empty",
                "--no-verify",
                "-m",
                f"test(privacy): blocked message\n\nReviewer: {private_email}",
            ],
            cwd=repo,
        )
        assert completed.returncode != 0
        assert git(repo, "rev-parse", "HEAD") == original_head
        assert object_ids(repo, "commit") == original_commits
        assert "privacy preparation gate blocked this commit" in completed.stderr
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_reference_transaction_blocks_private_annotated_tag_identity() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        enable_tracked_hooks(repo)
        private_email = "blocked-tagger@personal.test"
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_COMMITTER_NAME": "private tagger",
                "GIT_COMMITTER_EMAIL": private_email,
            }
        )
        completed = run(
            ["git", "tag", "-a", "v9.9.9", "-m", "safe release fixture"],
            cwd=repo,
            env=environment,
        )
        assert completed.returncode != 0
        assert (
            run(
                ["git", "rev-parse", "--verify", "refs/tags/v9.9.9"],
                cwd=repo,
            ).returncode
            != 0
        )
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_reference_transaction_blocks_direct_ref_to_unsafe_commit() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        enable_tracked_hooks(repo)
        private_email = "blocked-update-ref" + "@" + "corp.com"
        (repo / "private.txt").write_text(private_email, encoding="utf-8")
        git(repo, "add", "private.txt")
        tree = git(repo, "write-tree")
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_AUTHOR_NAME": "v1simple",
                "GIT_AUTHOR_EMAIL": "noreply@example.invalid",
                "GIT_COMMITTER_NAME": "v1simple",
                "GIT_COMMITTER_EMAIL": "noreply@example.invalid",
            }
        )
        commit = run(
            ["git", "commit-tree", tree, "-p", "HEAD", "-m", "test(privacy): direct ref"],
            cwd=repo,
            env=environment,
        )
        assert commit.returncode == 0, commit.stderr
        object_id = commit.stdout.strip()
        completed = run(["git", "update-ref", "refs/heads/unsafe", object_id], cwd=repo)
        assert completed.returncode != 0
        assert (
            run(
                ["git", "rev-parse", "--verify", "refs/heads/unsafe"],
                cwd=repo,
            ).returncode
            != 0
        )
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_reference_transaction_blocks_direct_ref_to_unsafe_message() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        enable_tracked_hooks(repo)
        private_email = "blocked-direct-message" + "@" + "corp.com"
        tree = git(repo, "write-tree")
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_AUTHOR_NAME": "v1simple",
                "GIT_AUTHOR_EMAIL": "noreply@example.invalid",
                "GIT_COMMITTER_NAME": "v1simple",
                "GIT_COMMITTER_EMAIL": "noreply@example.invalid",
            }
        )
        commit = run(
            [
                "git",
                "commit-tree",
                tree,
                "-p",
                "HEAD",
                "-m",
                f"test(privacy): direct message {private_email}",
            ],
            cwd=repo,
            env=environment,
        )
        assert commit.returncode == 0, commit.stderr
        completed = run(
            ["git", "update-ref", "refs/heads/unsafe-message", commit.stdout.strip()],
            cwd=repo,
        )
        assert completed.returncode != 0
        assert (
            run(
                ["git", "rev-parse", "--verify", "refs/heads/unsafe-message"],
                cwd=repo,
            ).returncode
            != 0
        )
        assert private_email not in completed.stdout
        assert private_email not in completed.stderr


def test_prepare_commit_msg_fails_closed_without_scanner() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        enable_tracked_hooks(repo)
        original_head = git(repo, "rev-parse", "HEAD")
        (repo / "scripts" / SNAPSHOT_CHECK.name).unlink()
        completed = run(
            ["git", "commit", "--allow-empty", "--no-verify", "-m", "test(privacy): fail closed"],
            cwd=repo,
        )
        assert completed.returncode != 0
        assert git(repo, "rev-parse", "HEAD") == original_head


def test_reference_transaction_fails_closed_without_scanner() -> None:
    with tempfile.TemporaryDirectory(prefix="privacy-hooks-") as raw:
        repo = make_repo(Path(raw))
        enable_tracked_hooks(repo)
        tree = git(repo, "write-tree")
        commit = git(repo, "commit-tree", tree, "-p", "HEAD", "-m", "safe fixture")
        (repo / "scripts" / SNAPSHOT_CHECK.name).unlink()
        completed = run(
            ["git", "update-ref", "refs/heads/fail-closed", commit],
            cwd=repo,
        )
        assert completed.returncode != 0
        assert (
            run(
                ["git", "rev-parse", "--verify", "refs/heads/fail-closed"],
                cwd=repo,
            ).returncode
            != 0
        )


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


def test_pre_push_requires_exact_local_hooks_path() -> None:
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
            configure_hooks=False,
        )
        assert completed.returncode != 0
        assert "core.hooksPath is not the required repository-relative path" in completed.stderr
        assert absolute_hooks_path not in completed.stderr

        git(repo, "config", "--unset", "core.hooksPath")
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
            configure_hooks=False,
        )
        assert completed.returncode != 0
        assert "core.hooksPath is not the required repository-relative path" in completed.stderr


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

        parity.unlink()
        completed = invoke_pre_push(
            repo,
            local_ref="refs/heads/main",
            local_sha=head,
            remote_ref="refs/heads/main",
        )
        assert completed.returncode != 0
        assert "pinned canonical digest" in completed.stderr


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
        test_reference_transaction_allows_safe_no_verify_commit,
        test_no_verify_staged_pii_is_blocked_before_commit_object,
        test_no_verify_identity_override_is_blocked_before_commit_object,
        test_no_verify_message_pii_is_blocked_before_commit_object,
        test_reference_transaction_blocks_private_annotated_tag_identity,
        test_reference_transaction_blocks_direct_ref_to_unsafe_commit,
        test_reference_transaction_blocks_direct_ref_to_unsafe_message,
        test_prepare_commit_msg_fails_closed_without_scanner,
        test_reference_transaction_fails_closed_without_scanner,
        test_pre_push_accepts_safe_main_history,
        test_pre_push_accepts_a_safe_existing_remote_range,
        test_pre_push_blocks_deleted_intermediate_content_in_existing_range,
        test_pre_push_allows_only_semantic_release_tags,
        test_pre_push_requires_exact_local_hooks_path,
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
