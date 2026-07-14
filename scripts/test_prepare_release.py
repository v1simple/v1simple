#!/usr/bin/env python3
"""Regression tests for the one-click release version preparer."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("prepare_release.py")
CHANGELOG_TEMPLATE = """# Changelog

## [Unreleased]

### Changed

- A tested change waiting for release.

## [1.0.1] - 2026-07-13

- Previous release.

[Unreleased]: https://github.com/v1simple/v1simple/compare/v1.0.1...HEAD
[1.0.1]: https://github.com/v1simple/v1simple/releases/tag/v1.0.1
"""


class TempReleaseRepo:
    def __init__(self, version: str = "1.0.1") -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name)
        (self.root / "include").mkdir()
        self.write_version(version)
        (self.root / "CHANGELOG.md").write_text(CHANGELOG_TEMPLATE, encoding="utf-8")
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release-test@example.invalid")

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

    def write_version(self, version: str) -> None:
        (self.root / "include" / "config.h").write_text(
            f'#pragma once\n#define FIRMWARE_VERSION "{version}"\n',
            encoding="utf-8",
        )

    def commit_all(self, message: str) -> str:
        self.git("add", "include/config.h", "CHANGELOG.md")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def add_change(self, message: str = "new release content") -> str:
        path = self.root / "change.txt"
        path.write_text(message + "\n", encoding="utf-8")
        self.git("add", "change.txt")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def prepare(
        self,
        bump: str,
        *,
        resume_tag: str | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, str]]:
        output_path = self.root / ".git" / "github-output.txt"
        output_path.unlink(missing_ok=True)
        command = [
            sys.executable,
            str(SCRIPT),
            "--root",
            str(self.root),
            "--bump",
            bump,
            "--date",
            "2026-07-14",
            "--repository",
            "v1simple/v1simple",
            "--github-output",
            str(output_path),
        ]
        if resume_tag is not None:
            command.extend(("--resume-tag", resume_tag))
        result = subprocess.run(
            command,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={key: value for key, value in os.environ.items() if key != "GITHUB_OUTPUT"},
        )
        values: dict[str, str] = {}
        if output_path.exists():
            for line in output_path.read_text(encoding="utf-8").splitlines():
                key, value = line.split("=", 1)
                values[key] = value
        return result, values

    def lookup_run(self, run_id: str) -> tuple[subprocess.CompletedProcess[str], dict[str, str]]:
        output_path = self.root / ".git" / "lookup-output.txt"
        output_path.unlink(missing_ok=True)
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--root",
                str(self.root),
                "--lookup-run-id",
                run_id,
                "--github-output",
                str(output_path),
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={key: value for key, value in os.environ.items() if key != "GITHUB_OUTPUT"},
        )
        values: dict[str, str] = {}
        if output_path.exists():
            for line in output_path.read_text(encoding="utf-8").splitlines():
                key, value = line.split("=", 1)
                values[key] = value
        return result, values

    def latest_tag(self) -> tuple[subprocess.CompletedProcess[str], dict[str, str]]:
        output_path = self.root / ".git" / "latest-tag-output.txt"
        output_path.unlink(missing_ok=True)
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--root",
                str(self.root),
                "--latest-tag",
                "--github-output",
                str(output_path),
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={key: value for key, value in os.environ.items() if key != "GITHUB_OUTPUT"},
        )
        values: dict[str, str] = {}
        if output_path.exists():
            for line in output_path.read_text(encoding="utf-8").splitlines():
                key, value = line.split("=", 1)
                values[key] = value
        return result, values


class PrepareReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TempReleaseRepo()

    def tearDown(self) -> None:
        self.repo.close()

    def tag_baseline_and_advance(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        self.repo.add_change()

    def test_patch_bump_updates_config_changelog_and_links(self) -> None:
        self.tag_baseline_and_advance()

        result, values = self.repo.prepare("patch")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values["version"], "1.0.2")
        self.assertEqual(values["tag"], "v1.0.2")
        self.assertEqual(values["previous_tag"], "v1.0.1")
        self.assertEqual(values["mode"], "new")
        self.assertEqual(values["files_changed"], "true")
        config = (self.repo.root / "include" / "config.h").read_text(encoding="utf-8")
        changelog = (self.repo.root / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertIn('#define FIRMWARE_VERSION "1.0.2"', config)
        self.assertIn("## [1.0.2] - 2026-07-14", changelog)
        self.assertLess(changelog.index("## [1.0.2]"), changelog.index("### Changed"))
        self.assertIn(
            "[Unreleased]: https://github.com/v1simple/v1simple/compare/v1.0.2...HEAD",
            changelog,
        )
        self.assertIn(
            "[1.0.2]: https://github.com/v1simple/v1simple/releases/tag/v1.0.2",
            changelog,
        )

    def test_minor_and_major_bumps_reset_lower_components(self) -> None:
        for bump, expected in (("minor", "1.1.0"), ("major", "2.0.0")):
            with self.subTest(bump=bump):
                repo = TempReleaseRepo()
                try:
                    repo.commit_all("baseline")
                    repo.git("tag", "v1.0.1")
                    repo.add_change()
                    result, values = repo.prepare(bump)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(values["version"], expected)
                finally:
                    repo.close()

    def test_fresh_dispatch_from_tagged_head_prepares_selected_bump(self) -> None:
        self.repo.write_version("1.0.2")
        changelog = (self.repo.root / "CHANGELOG.md").read_text(encoding="utf-8")
        changelog = changelog.replace(
            "## [Unreleased]\n",
            "## [Unreleased]\n\n## [1.0.2] - 2026-07-14\n",
            1,
        )
        (self.repo.root / "CHANGELOG.md").write_text(changelog, encoding="utf-8")
        self.repo.commit_all("prepared release")
        self.repo.git("tag", "v1.0.2")

        result, values = self.repo.prepare("patch", resume_tag="")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values["version"], "1.0.3")
        self.assertEqual(values["tag"], "v1.0.3")
        self.assertEqual(values["mode"], "new")
        self.assertEqual(values["files_changed"], "true")

    def test_tagged_head_is_an_idempotent_rerun_with_explicit_resume_tag(self) -> None:
        self.repo.write_version("1.0.2")
        changelog = (self.repo.root / "CHANGELOG.md").read_text(encoding="utf-8")
        changelog = changelog.replace(
            "## [Unreleased]\n",
            "## [Unreleased]\n\n## [1.0.2] - 2026-07-14\n",
            1,
        )
        (self.repo.root / "CHANGELOG.md").write_text(changelog, encoding="utf-8")
        self.repo.commit_all("prepared release")
        self.repo.git("tag", "v1.0.2")
        before = self.repo.git("status", "--porcelain")

        result, values = self.repo.prepare("major", resume_tag="v1.0.2")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values["version"], "1.0.2")
        self.assertEqual(values["mode"], "rerun")
        self.assertEqual(values["files_changed"], "false")
        self.assertEqual(self.repo.git("status", "--porcelain"), before)

    def test_manually_elevated_version_does_not_override_selected_bump(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        self.repo.write_version("9.0.0")
        self.repo.commit_all("manual version edit")

        result, values = self.repo.prepare("patch")

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(values)
        self.assertIn("must match latest release tag v1.0.1", result.stderr)
        config = (self.repo.root / "include" / "config.h").read_text(encoding="utf-8")
        self.assertIn('#define FIRMWARE_VERSION "9.0.0"', config)

    def test_run_lookup_reuses_annotated_release_after_main_advances(self) -> None:
        self.tag_baseline_and_advance()
        result, values = self.repo.prepare("patch")
        self.assertEqual(result.returncode, 0, result.stderr)
        release_sha = self.repo.commit_all("prepare v1.0.2")
        self.repo.git(
            "tag",
            "-a",
            "v1.0.2",
            "-m",
            "Release v1.0.2",
            "-m",
            "Release-Run-ID: 29336081336",
        )
        advanced_main_sha = self.repo.add_change("main advanced after publication")
        self.repo.git("tag", "v1.0.3", advanced_main_sha)

        lookup, resume = self.repo.lookup_run("29336081336")

        self.assertEqual(lookup.returncode, 0, lookup.stderr)
        self.assertEqual(resume["resume_tag"], "v1.0.2")
        self.assertEqual(resume["resume_sha"], release_sha)
        self.repo.git("checkout", "--detach", resume["resume_sha"])

        rerun, rerun_values = self.repo.prepare(
            "major", resume_tag=resume["resume_tag"]
        )

        self.assertEqual(rerun.returncode, 0, rerun.stderr)
        self.assertEqual(rerun_values["tag"], "v1.0.2")
        self.assertEqual(rerun_values["mode"], "rerun")
        self.assertEqual(rerun_values["files_changed"], "false")
        self.assertEqual(self.repo.git("rev-parse", "HEAD"), release_sha)
        self.assertEqual(self.repo.git("rev-parse", "main"), advanced_main_sha)

    def test_latest_tag_must_be_in_head_history(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        self.repo.git("switch", "-q", "-c", "other")
        self.repo.write_version("1.1.0")
        self.repo.commit_all("other release")
        self.repo.git("tag", "v1.1.0")
        self.repo.git("switch", "-q", "main")
        self.repo.add_change("main diverged")

        result, _ = self.repo.prepare("patch")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not an ancestor", result.stderr)

    def test_latest_tag_reports_highest_strict_semver(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.2")
        self.repo.git("tag", "v1.10.0")
        self.repo.git("tag", "vnot-a-release")

        result, values = self.repo.latest_tag()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "v1.10.0")
        self.assertEqual(values["latest_tag"], "v1.10.0")

    def test_empty_unreleased_section_gets_a_release_summary(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        changelog = (self.repo.root / "CHANGELOG.md").read_text(encoding="utf-8")
        changelog = changelog.replace(
            "\n### Changed\n\n- A tested change waiting for release.\n",
            "",
            1,
        )
        (self.repo.root / "CHANGELOG.md").write_text(changelog, encoding="utf-8")
        self.repo.commit_all("empty unreleased section")

        result, values = self.repo.prepare("patch")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values["version"], "1.0.2")
        updated = (self.repo.root / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertIn("Changes are summarized in the generated GitHub release notes.", updated)

    def test_dirty_working_tree_is_rejected_without_overwriting_it(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        original = '#pragma once\n#define FIRMWARE_VERSION "dirty-user-value"\n'
        (self.repo.root / "include" / "config.h").write_text(original, encoding="utf-8")

        result, values = self.repo.prepare("patch")

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(values)
        self.assertIn("clean working tree", result.stderr)
        self.assertEqual(
            (self.repo.root / "include" / "config.h").read_text(encoding="utf-8"),
            original,
        )

    def test_invalid_firmware_version_fails_before_writing(self) -> None:
        self.repo.write_version("1.0")
        self.repo.commit_all("invalid version")

        result, values = self.repo.prepare("patch")

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(values)
        self.assertIn("expected MAJOR.MINOR.PATCH", result.stderr)


if __name__ == "__main__":
    unittest.main()
