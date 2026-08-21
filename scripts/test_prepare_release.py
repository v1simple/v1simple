#!/usr/bin/env python3
"""Regression tests for release version selection and artifact identity."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import check_release_firmware_version


SCRIPT = Path(__file__).with_name("prepare_release.py")


class TempReleaseRepo:
    def __init__(self, version: str = "1.0.1") -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name)
        (self.root / "include").mkdir()
        self.write_version(version)
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
            "#pragma once\n"
            "#ifndef FIRMWARE_VERSION\n"
            f'#define FIRMWARE_VERSION "{version}"\n'
            "#endif\n",
            encoding="utf-8",
        )

    def commit_all(self, message: str) -> str:
        self.git("add", "-A")
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
            "--prepare",
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

    def query(
        self,
        option: str,
        value: str | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, str]]:
        output_path = self.root / ".git" / "query-output.txt"
        output_path.unlink(missing_ok=True)
        command = [
            sys.executable,
            str(SCRIPT),
            "--root",
            str(self.root),
            option,
        ]
        if value is not None:
            command.append(value)
        command.extend(("--github-output", str(output_path)))
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
                key, output_value = line.split("=", 1)
                values[key] = output_value
        return result, values


class PrepareReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TempReleaseRepo()

    def tearDown(self) -> None:
        self.repo.close()

    def tag_baseline_and_advance(self) -> str:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        return self.repo.add_change()

    def test_patch_selection_is_read_only(self) -> None:
        head = self.tag_baseline_and_advance()
        status = self.repo.git("status", "--porcelain")

        result, values = self.repo.prepare()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values["version"], "1.0.2")
        self.assertEqual(values["tag"], "v1.0.2")
        self.assertEqual(values["source_version"], "1.0.1")
        self.assertEqual(values["previous_tag"], "v1.0.1")
        self.assertEqual(values["mode"], "new")
        self.assertEqual(values["bump"], "patch")
        self.assertEqual(self.repo.git("rev-parse", "HEAD"), head)
        self.assertEqual(self.repo.git("status", "--porcelain"), status)

    def test_stale_source_version_does_not_block_automatic_patches(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        released = self.repo.add_change("released patch")
        self.repo.git("tag", "v1.0.2", released)
        self.repo.add_change("next patch")

        result, values = self.repo.prepare()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values["version"], "1.0.3")
        self.assertEqual(values["source_version"], "1.0.1")
        self.assertEqual(values["bump"], "patch")

    def test_reviewed_source_version_selects_next_minor_or_major(self) -> None:
        for source, expected_bump in (("1.1.0", "minor"), ("2.0.0", "major")):
            with self.subTest(source=source):
                repo = TempReleaseRepo()
                try:
                    repo.commit_all("baseline")
                    repo.git("tag", "v1.0.1")
                    repo.write_version(source)
                    repo.commit_all("reviewed version selection")

                    result, values = repo.prepare()

                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(values["version"], source)
                    self.assertEqual(values["bump"], expected_bump)
                finally:
                    repo.close()

    def test_arbitrary_source_version_jump_is_rejected(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        self.repo.write_version("1.0.9")
        self.repo.commit_all("invalid version jump")

        result, values = self.repo.prepare()

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(values)
        self.assertIn("not the next patch, minor, or major", result.stderr)

    def test_first_release_uses_source_version(self) -> None:
        self.repo.commit_all("initial source")

        result, values = self.repo.prepare()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values["version"], "1.0.1")
        self.assertEqual(values["mode"], "initial")
        self.assertEqual(values["bump"], "initial")

    def test_run_lookup_resumes_annotated_release_after_main_advances(self) -> None:
        self.tag_baseline_and_advance()
        release_sha = self.repo.git("rev-parse", "HEAD")
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

        lookup, resume = self.repo.query("--lookup-run-id", "29336081336")

        self.assertEqual(lookup.returncode, 0, lookup.stderr)
        self.assertEqual(resume["resume_tag"], "v1.0.2")
        self.assertEqual(resume["resume_sha"], release_sha)
        self.repo.git("checkout", "--detach", resume["resume_sha"])

        rerun, rerun_values = self.repo.prepare(resume_tag=resume["resume_tag"])

        self.assertEqual(rerun.returncode, 0, rerun.stderr)
        self.assertEqual(rerun_values["tag"], "v1.0.2")
        self.assertEqual(rerun_values["mode"], "rerun")
        self.assertEqual(rerun_values["bump"], "rerun")
        self.assertEqual(self.repo.git("rev-parse", "HEAD"), release_sha)
        self.assertEqual(self.repo.git("rev-parse", "main"), advanced_main_sha)

    def test_resume_tag_must_resolve_to_checked_out_commit(self) -> None:
        self.tag_baseline_and_advance()

        result, values = self.repo.prepare(resume_tag="v1.0.1")

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(values)
        self.assertIn("expected HEAD", result.stderr)

    def test_latest_tag_must_be_in_head_history(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        self.repo.git("switch", "-q", "-c", "other")
        self.repo.write_version("1.1.0")
        self.repo.commit_all("other release")
        self.repo.git("tag", "v1.1.0")
        self.repo.git("switch", "-q", "main")
        self.repo.add_change("main diverged")

        result, _ = self.repo.prepare()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not an ancestor", result.stderr)

    def test_latest_tag_reports_highest_strict_semver(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.2")
        self.repo.git("tag", "v1.10.0")
        self.repo.git("tag", "vnot-a-release")

        result, values = self.repo.query("--latest-tag")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "v1.10.0")
        self.assertEqual(values["latest_tag"], "v1.10.0")

    def test_dirty_working_tree_is_rejected_without_overwriting_it(self) -> None:
        self.repo.commit_all("baseline")
        self.repo.git("tag", "v1.0.1")
        original = '#define FIRMWARE_VERSION "dirty-user-value"\n'
        (self.repo.root / "include" / "config.h").write_text(original, encoding="utf-8")

        result, values = self.repo.prepare()

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(values)
        self.assertIn("clean working tree", result.stderr)
        self.assertEqual(
            (self.repo.root / "include" / "config.h").read_text(encoding="utf-8"),
            original,
        )

    def test_invalid_firmware_version_fails_before_output(self) -> None:
        self.repo.write_version("1.0")
        self.repo.commit_all("invalid version")

        result, values = self.repo.prepare()

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(values)
        self.assertIn("expected MAJOR.MINOR.PATCH", result.stderr)


class ReleaseFirmwareVersionTests(unittest.TestCase):
    def test_selected_version_is_present_without_stale_source_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            firmware = Path(temporary) / "firmware.bin"
            firmware.write_bytes(b"prefix\0" + b"1.0.2\0" + b"suffix\0")

            errors = check_release_firmware_version.check_firmware_version(
                firmware,
                "1.0.2",
                "1.0.1",
            )

        self.assertEqual(errors, [])

    def test_missing_selected_version_and_stale_source_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            firmware = Path(temporary) / "firmware.bin"
            firmware.write_bytes(b"prefix\0" + b"1.0.1\0" + b"suffix\0")

            errors = check_release_firmware_version.check_firmware_version(
                firmware,
                "1.0.2",
                "1.0.1",
            )

        self.assertTrue(any("does not contain" in error for error in errors), errors)
        self.assertTrue(any("still contains" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main(verbosity=2)
