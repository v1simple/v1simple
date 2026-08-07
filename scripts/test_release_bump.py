#!/usr/bin/env python3
"""Regression tests for the next-release bump selector."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("release-bump")


class TempReleaseRepo:
    def __init__(self, version: str = "1.0.1", bump: str = "patch") -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name)
        (self.root / "include").mkdir()
        (self.root / "include" / "config.h").write_text(
            f'#pragma once\n#define FIRMWARE_VERSION "{version}"\n',
            encoding="utf-8",
        )
        (self.root / ".release-bump").write_text(bump + "\n", encoding="utf-8")
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release-test@example.invalid")
        self.git("add", "include/config.h", ".release-bump")
        self.git("commit", "-q", "-m", "baseline")
        self.git("tag", f"v{version}")

    def close(self) -> None:
        self._temporary.cleanup()

    def git(self, *args: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(self.root), *args],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return result.stdout.strip()

    def run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(SCRIPT), *args, "--root", str(self.root)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )


class ReleaseBumpTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TempReleaseRepo()

    def tearDown(self) -> None:
        self.repo.close()

    def test_default_command_shows_current_bump_and_next_release(self) -> None:
        result = self.repo.run()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "Release bump: patch\nNext release: v1.0.2\n")
        self.assertEqual((self.repo.root / ".release-bump").read_text(), "patch\n")

    def test_minor_selection_commits_only_policy_and_previews_reset_version(self) -> None:
        before = self.repo.git("rev-parse", "HEAD")

        result = self.repo.run("minor")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Release bump: patch -> minor", result.stdout)
        self.assertIn("Next release: v1.1.0", result.stdout)
        self.assertIn("Created commit", result.stdout)
        self.assertIn("workflow resets the bump to patch", result.stdout)
        self.assertEqual((self.repo.root / ".release-bump").read_text(), "minor\n")
        self.assertEqual(
            (self.repo.root / "include" / "config.h").read_text(),
            '#pragma once\n#define FIRMWARE_VERSION "1.0.1"\n',
        )
        self.assertEqual(self.repo.git("status", "--short"), "")
        self.assertNotEqual(self.repo.git("rev-parse", "HEAD"), before)
        self.assertEqual(
            self.repo.git("log", "-1", "--format=%s"),
            "ci(release): select v1.1.0",
        )

    def test_major_selection_commits_next_major_release(self) -> None:
        result = self.repo.run("major")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Next release: v2.0.0", result.stdout)
        self.assertEqual((self.repo.root / ".release-bump").read_text(), "major\n")
        self.assertEqual(
            self.repo.git("log", "-1", "--format=%s"),
            "ci(release): select v2.0.0",
        )

    def test_reselecting_current_bump_does_not_report_a_transition(self) -> None:
        before = self.repo.git("rev-parse", "HEAD")

        result = self.repo.run("patch")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Release bump: patch (already selected)", result.stdout)
        self.assertIn("No release-selection commit was needed", result.stdout)
        self.assertEqual(self.repo.git("status", "--short"), "")
        self.assertEqual(self.repo.git("rev-parse", "HEAD"), before)

    def test_pending_policy_only_selection_is_committed(self) -> None:
        (self.repo.root / ".release-bump").write_text("minor\n", encoding="utf-8")

        result = self.repo.run("minor")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Release bump: minor (pending commit)", result.stdout)
        self.assertIn("Created commit", result.stdout)
        self.assertEqual(self.repo.git("status", "--short"), "")
        self.assertEqual(
            self.repo.git("log", "-1", "--format=%s"),
            "ci(release): select v1.1.0",
        )

    def test_selecting_committed_policy_cancels_pending_selection(self) -> None:
        before = self.repo.git("rev-parse", "HEAD")
        (self.repo.root / ".release-bump").write_text("minor\n", encoding="utf-8")

        result = self.repo.run("patch")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Release bump: minor -> patch", result.stdout)
        self.assertIn("No release-selection commit was needed", result.stdout)
        self.assertEqual(self.repo.git("status", "--short"), "")
        self.assertEqual(self.repo.git("rev-parse", "HEAD"), before)

    def test_unrelated_change_is_refused_without_changing_policy(self) -> None:
        before = self.repo.git("rev-parse", "HEAD")
        (self.repo.root / "notes.txt").write_text("unfinished\n", encoding="utf-8")

        result = self.repo.run("minor")

        self.assertEqual(result.returncode, 1)
        self.assertIn("commit or stash changes outside .release-bump", result.stderr)
        self.assertEqual((self.repo.root / ".release-bump").read_text(), "patch\n")
        self.assertEqual(self.repo.git("rev-parse", "HEAD"), before)

    def test_invalid_policy_fails_without_overwriting_it(self) -> None:
        policy = self.repo.root / ".release-bump"
        policy.write_text("preview\n", encoding="utf-8")

        result = self.repo.run("minor")

        self.assertEqual(result.returncode, 1)
        self.assertIn("must contain exactly one of", result.stderr)
        self.assertEqual(policy.read_text(), "preview\n")

    def test_manual_firmware_version_change_fails_without_changing_policy(self) -> None:
        config = self.repo.root / "include" / "config.h"
        config.write_text(
            '#pragma once\n#define FIRMWARE_VERSION "9.0.0"\n',
            encoding="utf-8",
        )

        result = self.repo.run()

        self.assertEqual(result.returncode, 1)
        self.assertIn("must match latest release tag v1.0.1", result.stderr)
        self.assertEqual((self.repo.root / ".release-bump").read_text(), "patch\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
