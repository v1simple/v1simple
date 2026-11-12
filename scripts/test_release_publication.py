#!/usr/bin/env python3
"""Regression tests for release publication and recovery decisions."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import release_publication as publication


SHA_A = "a" * 40
SHA_B = "b" * 40


def facts(**changes: object) -> publication.PublicationFacts:
    values: dict[str, object] = {
        "release_sha": SHA_A,
        "base_sha": SHA_A,
        "release_tag": "v1.2.3",
        "head_sha": SHA_A,
        "dirty": False,
        "remote_main_sha": SHA_A,
    }
    values.update(changes)
    return publication.PublicationFacts(**values)  # type: ignore[arg-type]


class PublicationDecisionTests(unittest.TestCase):
    def test_decision_table(self) -> None:
        cases = (
            (
                "new publication",
                facts(),
                publication.Action.ATTEMPT_PUBLISH,
                False,
            ),
            (
                "same-run retry",
                facts(existing_tag_sha=SHA_A, existing_marker_matches=True),
                publication.Action.REUSE,
                True,
            ),
            (
                "conflicting tag",
                facts(existing_tag_sha=SHA_B, existing_marker_matches=True),
                publication.Action.FAIL,
                False,
            ),
            (
                "wrong tag marker",
                facts(existing_tag_sha=SHA_A, existing_marker_matches=False),
                publication.Action.FAIL,
                False,
            ),
            (
                "main advanced before push",
                facts(remote_main_sha=SHA_B),
                publication.Action.SKIP_MAIN_ADVANCED,
                False,
            ),
            (
                "push succeeded",
                facts(push_attempted=True, push_succeeded=True, updated_main_sha=SHA_A,
                      updated_tag_sha=SHA_A, updated_marker_matches=True),
                publication.Action.PUBLISHED,
                True,
            ),
            (
                "ambiguous push recovered",
                facts(
                    push_attempted=True,
                    push_succeeded=False,
                    updated_main_sha=SHA_A,
                    updated_tag_sha=SHA_A,
                    updated_marker_matches=True,
                ),
                publication.Action.RECOVER,
                True,
            ),
            (
                "ambiguous push wrong marker",
                facts(
                    push_attempted=True,
                    push_succeeded=False,
                    updated_main_sha=SHA_A,
                    updated_tag_sha=SHA_A,
                    updated_marker_matches=False,
                ),
                publication.Action.FAIL,
                False,
            ),
            (
                "main advanced during failed push",
                facts(
                    push_attempted=True,
                    push_succeeded=False,
                    updated_main_sha=SHA_B,
                ),
                publication.Action.SKIP_MAIN_ADVANCED,
                False,
            ),
            (
                "genuine push failure",
                facts(
                    push_attempted=True,
                    push_succeeded=False,
                    updated_main_sha=SHA_A,
                ),
                publication.Action.FAIL,
                False,
            ),
            (
                "head mismatch",
                facts(head_sha=SHA_B),
                publication.Action.FAIL,
                False,
            ),
            (
                "dirty checkout",
                facts(dirty=True),
                publication.Action.FAIL,
                False,
            ),
        )

        for name, state, expected_action, expected_published in cases:
            with self.subTest(name=name):
                decision = publication.decide_publication(state)
                self.assertEqual(decision.action, expected_action)
                self.assertEqual(decision.published, expected_published)

    def test_only_latest_release_deploys_pages(self) -> None:
        self.assertTrue(publication.should_deploy_pages("v1.2.3", "v1.2.3"))
        self.assertFalse(publication.should_deploy_pages("v1.2.2", "v1.2.3"))


class TempPublicationRepo:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        base = Path(self.temporary.name)
        self.remote = base / "remote.git"
        self.root = base / "work"
        self.git_at(base, "init", "--bare", "-q", str(self.remote))
        self.git_at(base, "init", "-q", "-b", "main", str(self.root))
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release-test@example.invalid")
        (self.root / "source.txt").write_text("baseline\n", encoding="utf-8")
        self.git("add", "source.txt")
        self.git("commit", "-q", "-m", "baseline")
        self.git("remote", "add", "origin", str(self.remote))
        self.git("push", "-q", "-u", "origin", "main")

    def close(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def git_at(root: Path, *args: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(root), *args],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return result.stdout.strip()

    def git(self, *args: str) -> str:
        return self.git_at(self.root, *args)

    @property
    def head(self) -> str:
        return self.git("rev-parse", "HEAD")

    def publish(
        self,
        tag: str = "v1.0.1",
        run_id: str = "12345",
        output_path: Path | None = None,
    ) -> publication.PublicationResult:
        return publication.publish_release(
            self.root,
            release_sha=self.head,
            base_sha=self.head,
            release_tag=tag,
            release_run_id=run_id,
            output_path=output_path,
        )


class PublicationIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TempPublicationRepo()

    def tearDown(self) -> None:
        self.repo.close()

    def test_first_publication_and_same_run_retry_are_idempotent(self) -> None:
        release_sha = self.repo.head
        first_output = self.repo.root.parent / "first-output.txt"
        retry_output = self.repo.root.parent / "retry-output.txt"

        first = self.repo.publish(output_path=first_output)
        second = self.repo.publish(output_path=retry_output)

        self.assertEqual(first.action, publication.Action.PUBLISHED)
        self.assertEqual(second.action, publication.Action.REUSE)
        self.assertTrue(first.published and first.deploy_pages)
        self.assertTrue(second.published and second.deploy_pages)
        self.assertEqual(
            self.repo.git("rev-parse", "v1.0.1^{commit}"), release_sha
        )
        self.assertEqual(
            self.repo.git("ls-remote", "origin", "refs/heads/main").split()[0],
            release_sha,
        )
        contents = self.repo.git(
            "for-each-ref", "--format=%(contents)", "refs/tags/v1.0.1"
        )
        self.assertIn("Release-Run-ID: 12345", contents.splitlines())
        expected_outputs = {
            "deploy_pages=true",
            f"sha={release_sha}",
            "published=true",
        }
        self.assertEqual(set(first_output.read_text().splitlines()), expected_outputs)
        self.assertEqual(set(retry_output.read_text().splitlines()), expected_outputs)

    def test_older_same_run_retry_repairs_without_replacing_pages(self) -> None:
        release_sha = self.repo.head
        self.repo.publish()
        clone = self.repo.root.parent / "newer"
        self.repo.git_at(
            self.repo.root.parent,
            "clone",
            "-q",
            "-b",
            "main",
            str(self.repo.remote),
            str(clone),
        )
        self.repo.git_at(clone, "config", "user.name", "Newer Release Test")
        self.repo.git_at(clone, "config", "user.email", "newer@example.invalid")
        (clone / "newer.txt").write_text("newer\n", encoding="utf-8")
        self.repo.git_at(clone, "add", "newer.txt")
        self.repo.git_at(clone, "commit", "-q", "-m", "newer release")
        self.repo.git_at(
            clone,
            "tag",
            "-a",
            "v1.0.2",
            "-m",
            "Release v1.0.2",
            "-m",
            "Release-Run-ID: 67890",
        )
        self.repo.git_at(clone, "push", "-q", "--atomic", "origin", "main", "v1.0.2")
        output = self.repo.root.parent / "older-retry-output.txt"

        result = publication.publish_release(
            self.repo.root,
            release_sha=release_sha,
            base_sha=release_sha,
            release_tag="v1.0.1",
            release_run_id="12345",
            output_path=output,
            has_public_release=lambda tag: tag == "v1.0.1",
        )

        self.assertEqual(result.action, publication.Action.REUSE)
        self.assertTrue(result.published)
        self.assertFalse(result.deploy_pages)
        self.assertEqual(
            set(output.read_text().splitlines()),
            {"deploy_pages=false", f"sha={release_sha}", "published=true"},
        )

    def test_conflicting_marker_is_refused_without_ref_changes(self) -> None:
        release_sha = self.repo.head
        self.repo.git(
            "tag",
            "-a",
            "v1.0.1",
            release_sha,
            "-m",
            "Release v1.0.1",
            "-m",
            "Release-Run-ID: 99999",
        )
        self.repo.git("push", "-q", "origin", "refs/tags/v1.0.1")
        refs_before = self.repo.git("show-ref", "--heads", "--tags")

        with self.assertRaisesRegex(publication.PublicationError, "does not carry"):
            self.repo.publish()

        self.assertEqual(self.repo.git("show-ref", "--heads", "--tags"), refs_before)

    def advance_remote_commit(self) -> str:
        (self.repo.root / "advance.txt").write_text("advance\n", encoding="utf-8")
        self.repo.git("add", "advance.txt")
        self.repo.git("commit", "-qm", "advance main")
        advanced = self.repo.head
        self.repo.git("push", "-q", "origin", "main")
        self.repo.git("reset", "--hard", "HEAD^")
        return advanced

    def test_main_advance_after_advertisement_skips_release_and_retry(self) -> None:
        release_sha = self.repo.head
        advanced = self.advance_remote_commit()
        # Simulate a concurrent fast-forward after push's initial advertisement
        # using the disposable remote directly from Git's pre-push hook.
        self.repo.git_at(self.repo.remote, "update-ref", "refs/heads/main", release_sha)
        hook = self.repo.root / ".git/hooks/pre-push"
        hook.write_text(
            "#!/bin/sh\n"
            f'git -C "$2" update-ref refs/heads/main {advanced} {release_sha}\n',
            encoding="utf-8",
        )
        hook.chmod(0o755)
        first = self.repo.publish()
        hook.unlink()
        self.assertEqual(first.action, publication.Action.SKIP_MAIN_ADVANCED)
        self.assertFalse(first.published or first.deploy_pages)
        self.assertEqual(self.repo.git_at(self.repo.remote, "rev-parse", "refs/heads/main"), advanced)
        self.assertEqual(self.repo.git_at(self.repo.remote, "rev-parse", "v1.0.1^{commit}"), release_sha)
        retry = self.repo.publish()
        self.assertEqual(retry.action, publication.Action.SKIP_MAIN_ADVANCED)
        self.assertFalse(retry.published or retry.deploy_pages)

    def test_main_advance_while_checking_release_status_suppresses_publication(self) -> None:
        self.repo.publish()
        release_sha = self.repo.head

        def advance_during_lookup(_tag: str) -> bool:
            self.advance_remote_commit()
            return False

        result = publication.publish_release(
            self.repo.root, release_sha=release_sha, base_sha=release_sha,
            release_tag="v1.0.1", release_run_id="12345", check_only=True,
            has_public_release=advance_during_lookup,
        )
        self.assertFalse(result.published or result.deploy_pages)

    def test_lost_push_result_recovers_matching_tag(self) -> None:
        real_git = publication.run_git

        def lose_push_reply(root: Path, *args: str, **kwargs):
            response = real_git(root, *args, **kwargs)
            if args[0] == "push":
                return subprocess.CompletedProcess(args, 1, response.stdout, "lost reply")
            return response

        with mock.patch.object(publication, "run_git", side_effect=lose_push_reply):
            result = self.repo.publish()
        self.assertEqual(result.action, publication.Action.RECOVER)
        self.assertTrue(result.published and result.deploy_pages)

    def test_advanced_main_skips_without_creating_release_tag(self) -> None:
        release_sha = self.repo.head
        output = self.repo.root.parent / "advanced-output.txt"
        clone = self.repo.root.parent / "other"
        self.repo.git_at(
            self.repo.root.parent,
            "clone",
            "-q",
            "-b",
            "main",
            str(self.repo.remote),
            str(clone),
        )
        self.repo.git_at(clone, "config", "user.name", "Other Test")
        self.repo.git_at(clone, "config", "user.email", "other@example.invalid")
        (clone / "advance.txt").write_text("advance\n", encoding="utf-8")
        self.repo.git_at(clone, "add", "advance.txt")
        self.repo.git_at(clone, "commit", "-q", "-m", "advance main")
        self.repo.git_at(clone, "push", "-q", "origin", "main")

        result = publication.publish_release(
            self.repo.root,
            release_sha=release_sha,
            base_sha=release_sha,
            release_tag="v1.0.1",
            release_run_id="12345",
            output_path=output,
        )

        self.assertEqual(result.action, publication.Action.SKIP_MAIN_ADVANCED)
        self.assertFalse(result.published)
        self.assertIsNone(publication.resolve_commit(self.repo.root, "v1.0.1"))
        self.assertEqual(
            set(output.read_text().splitlines()),
            {"published=false", "deploy_pages=false"},
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
