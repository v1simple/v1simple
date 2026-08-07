#!/usr/bin/env python3
"""Regression tests for release build and artifact validation."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import check_release_workflow_flash_contract as contract


class ReleaseArtifactContractTests(unittest.TestCase):
    def test_live_release_artifact_contract_passes(self) -> None:
        errors: list[str] = []
        contract.check_production_build(errors)
        contract.check_version_policy(errors)
        contract.check_flash_and_package(errors)
        self.assertEqual(errors, [])

    def test_rejects_release_bump_policy_that_cannot_reset_to_patch(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        required = 'if [ "$(cat .release-bump)" != "patch" ]; then'
        self.assertIn(required, release_text)
        release_text = release_text.replace(required, "if false; then", 1)

        with tempfile.TemporaryDirectory(prefix="release_bump_policy_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_version_policy(errors)

        self.assertTrue(any("release-bump" in error for error in errors), errors)

    def test_rejects_packaging_before_production_build(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        build = "./scripts/build_production_artifacts.sh"
        merge = "Merge firmware for ESP Web Tools"
        self.assertLess(release_text.index(build), release_text.index(merge))
        release_text = release_text.replace(build, "BUILD_PLACEHOLDER", 1)
        release_text = release_text.replace(merge, build, 1)
        release_text = release_text.replace("BUILD_PLACEHOLDER", merge, 1)

        with tempfile.TemporaryDirectory(prefix="release_build_order_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_production_build(errors)

        self.assertTrue(any("build, then package" in error for error in errors), errors)

    def test_rejects_workflow_that_does_not_run_artifact_checker_first(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        checker = "python3 scripts/check_release_workflow_flash_contract.py"
        build = "./scripts/build_production_artifacts.sh"
        self.assertLess(release_text.index(checker), release_text.index(build))
        release_text = release_text.replace(checker, "CHECKER_PLACEHOLDER", 1)
        release_text = release_text.replace(build, checker, 1)
        release_text = release_text.replace("CHECKER_PLACEHOLDER", build, 1)

        with tempfile.TemporaryDirectory(prefix="release_checker_order_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_production_build(errors)

        self.assertTrue(any("artifact contract, build" in error for error in errors), errors)

    def test_rejects_missing_littlefs_compatibility_check(self) -> None:
        production_text = contract.PRODUCTION_BUILD.read_text(encoding="utf-8")
        required = "scripts/check_littlefs_image_compatibility.py"
        self.assertIn(required, production_text)
        production_text = production_text.replace(required, "scripts/removed.py", 1)

        with tempfile.TemporaryDirectory(prefix="release_littlefs_") as temporary:
            candidate = Path(temporary) / "build_production_artifacts.sh"
            candidate.write_text(production_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "PRODUCTION_BUILD", candidate):
                contract.check_production_build(errors)

        self.assertTrue(any(required in error for error in errors), errors)

    def test_rejects_obsolete_exclusive_iram_warning(self) -> None:
        production_text = contract.PRODUCTION_BUILD.read_text(encoding="utf-8")
        required = "--warn-diram-zero"
        self.assertIn(required, production_text)
        production_text = production_text.replace(required, "--warn-iram-zero", 1)

        with tempfile.TemporaryDirectory(prefix="release_memory_headroom_") as temporary:
            candidate = Path(temporary) / "build_production_artifacts.sh"
            candidate.write_text(production_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "PRODUCTION_BUILD", candidate):
                contract.check_production_build(errors)

        self.assertTrue(any(required in error for error in errors), errors)

    def test_rejects_workflow_without_release_license_staging(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        required = "python3 scripts/stage_release_licenses.py"
        self.assertIn(required, release_text)
        release_text = release_text.replace(required, "python3 removed.py", 1)

        with tempfile.TemporaryDirectory(prefix="release_licenses_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_flash_and_package(errors)

        self.assertTrue(any(required in error for error in errors), errors)

    def test_release_offsets_match_partition_table(self) -> None:
        self.assertEqual(
            contract.workflow_image_offset("firmware.bin"),
            contract.partition_offset("app"),
        )
        self.assertEqual(
            contract.workflow_image_offset("littlefs.bin"),
            contract.partition_offset("storage"),
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
