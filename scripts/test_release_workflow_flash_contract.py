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
        contract.check_version_and_publication(errors)
        contract.check_flash_and_package(errors)
        self.assertEqual(errors, [])

    def test_rejects_release_without_build_version_injection(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        required = "V1_RELEASE_VERSION:"
        self.assertIn(required, release_text)
        release_text = release_text.replace(required, "REMOVED_RELEASE_VERSION:", 1)

        with tempfile.TemporaryDirectory(prefix="release_version_injection_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_version_and_publication(errors)

        self.assertTrue(any(required in error for error in errors), errors)

    def test_release_sha_is_the_exact_tested_base(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        required = "RELEASE_SHA: ${{ steps.base.outputs.sha }}"
        self.assertIn(required, release_text)
        release_text = release_text.replace(
            required,
            "RELEASE_SHA: ${{ steps.version.outputs.tag }}",
            1,
        )

        with tempfile.TemporaryDirectory(prefix="release_tested_sha_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_version_and_publication(errors)

        self.assertTrue(any(required in error for error in errors), errors)

    def test_rejects_missing_public_release_tagger_identity(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        required = "GIT_COMMITTER_NAME: v1simple"
        self.assertIn(required, release_text)
        release_text = release_text.replace(required, "REMOVED_TAGGER_NAME: v1simple", 1)

        with tempfile.TemporaryDirectory(prefix="release_tagger_identity_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_version_and_publication(errors)

        self.assertTrue(any(required in error for error in errors), errors)

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

        self.assertTrue(any("build, verify" in error for error in errors), errors)

    def test_rejects_packaging_before_embedded_version_check(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        version_check = "python3 scripts/check_release_firmware_version.py"
        merge = "Merge firmware for ESP Web Tools"
        self.assertLess(release_text.index(version_check), release_text.index(merge))
        release_text = release_text.replace(version_check, "VERSION_CHECK_PLACEHOLDER", 1)
        release_text = release_text.replace(merge, version_check, 1)
        release_text = release_text.replace("VERSION_CHECK_PLACEHOLDER", merge, 1)

        with tempfile.TemporaryDirectory(prefix="release_version_order_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_production_build(errors)

        self.assertTrue(any("verify the embedded version" in error for error in errors), errors)

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
