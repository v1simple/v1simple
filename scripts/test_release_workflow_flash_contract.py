#!/usr/bin/env python3
"""Regression tests for release build and artifact validation."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import check_release_workflow_flash_contract as contract
import check_web_installer_page as installer
import write_release_manifests as manifests


class ReleaseArtifactContractTests(unittest.TestCase):
    @staticmethod
    def write_valid_release_images(release_dir: Path) -> dict[str, manifests.Partition]:
        partitions = manifests.read_partitions(contract.PARTITIONS)
        components = {
            manifests.BOOTLOADER_IMAGE: (0, b"bootloader"),
            manifests.PARTITION_TABLE_IMAGE: (
                manifests.PARTITION_TABLE_OFFSET,
                b"partition-table",
            ),
            manifests.APP_IMAGE: (partitions["app"].offset, b"firmware"),
            "littlefs.bin": (partitions["storage"].offset, b"littlefs"),
        }
        merged_size = max(offset + len(payload) for offset, payload in components.values())
        merged = bytearray(b"\xff" * merged_size)
        for name, (offset, payload) in components.items():
            (release_dir / name).write_bytes(payload)
            merged[offset : offset + len(payload)] = payload
        (release_dir / manifests.FRESH_IMAGE).write_bytes(merged)
        (release_dir / manifests.UPDATE_IMAGE).write_bytes(
            (release_dir / manifests.APP_IMAGE).read_bytes()
        )
        return partitions

    def test_live_release_artifact_contract_passes(self) -> None:
        errors: list[str] = []
        contract.check_production_build(errors)
        contract.check_version_and_publication(errors)
        contract.check_toolchain_provenance(errors)
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

    def test_rejects_unpinned_installer_dependency_without_sri(self) -> None:
        index_text = (contract.ROOT / "web-installer" / "index.html").read_text(
            encoding="utf-8"
        )
        expected_url, expected_integrity, expected_crossorigin = installer.EXPECTED_INSTALL_SCRIPT
        self.assertIn(f'src="{expected_url}"', index_text)
        mutated = index_text.replace(
            expected_url,
            "https://unpkg.com/esp-web-tools@latest/dist/web/install-button.js?module",
            1,
        )
        mutated = mutated.replace(f'\n    integrity="{expected_integrity}"', "", 1)
        mutated = mutated.replace(f'\n    crossorigin="{expected_crossorigin}"', "", 1)

        with tempfile.TemporaryDirectory(prefix="installer_dependency_") as temporary:
            site_dir = Path(temporary) / "web-installer"
            site_dir.mkdir()
            (site_dir / "index.html").write_text(mutated, encoding="utf-8")
            branding = Path(temporary) / "interface/static/branding/v1simple-logo-transparent.png"
            branding.parent.mkdir(parents=True)
            branding.write_bytes(b"branding")
            with mock.patch.object(
                installer.sys,
                "argv",
                ["checker", "--site-dir", str(site_dir), "--template-only"],
            ):
                self.assertEqual(installer.main(), 1)

    def test_release_offsets_match_partition_table(self) -> None:
        self.assertEqual(
            contract.workflow_image_offset("firmware.bin"),
            contract.partition_offset("app"),
        )
        self.assertEqual(
            contract.workflow_image_offset("littlefs.bin"),
            contract.partition_offset("storage"),
        )

    def test_ci_and_release_use_exact_same_image_tool_versions(self) -> None:
        errors: list[str] = []
        contract.check_toolchain_provenance(errors)
        self.assertEqual(errors, [])

    def test_rejects_release_platformio_version_range(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        release_text = release_text.replace(
            '"platformio==6.1.19"',
            '"platformio>=6.1.19,<7"',
            1,
        )
        with tempfile.TemporaryDirectory(prefix="release_tool_range_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_toolchain_provenance(errors)
        self.assertTrue(any("exact platformio pin" in error for error in errors), errors)

    def test_rejects_release_esptool_version_range(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        release_text = release_text.replace(
            '"esptool==5.3.0"',
            '"esptool>=5.3.0"',
            1,
        )
        with tempfile.TemporaryDirectory(prefix="release_esptool_range_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_toolchain_provenance(errors)
        self.assertTrue(any("exact esptool pin" in error for error in errors), errors)

    def test_rejects_merge_without_immediate_exact_esptool_check(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        merge_step = release_text.index("- name: Merge firmware for ESP Web Tools")
        required = "python3 scripts/check_esptool_version.py --python python3"
        check_index = release_text.index(required, merge_step)
        release_text = release_text[:check_index] + release_text[check_index + len(required) :]
        with tempfile.TemporaryDirectory(prefix="release_merge_tool_check_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_toolchain_provenance(errors)
        self.assertTrue(any("merge step" in error for error in errors), errors)

    def test_generated_update_manifest_is_app_only_and_fresh_is_complete(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_manifests_") as temporary:
            release_dir = Path(temporary)
            partitions = self.write_valid_release_images(release_dir)

            manifests.write_manifests(release_dir, "v2.0.3", contract.PARTITIONS)

            update = json.loads(
                (release_dir / manifests.UPDATE_MANIFEST).read_text(encoding="utf-8")
            )
            fresh = json.loads(
                (release_dir / manifests.FRESH_MANIFEST).read_text(encoding="utf-8")
            )
            self.assertEqual(update["version"], "v2.0.3")
            self.assertEqual(fresh["version"], "v2.0.3")
            self.assertEqual(
                update["builds"][0]["parts"],
                [{"path": manifests.UPDATE_IMAGE, "offset": partitions["app"].offset}],
            )
            self.assertEqual(
                fresh["builds"][0]["parts"],
                [{"path": manifests.FRESH_IMAGE, "offset": 0}],
            )

            update_start = update["builds"][0]["parts"][0]["offset"]
            update_end = update_start + (release_dir / manifests.UPDATE_IMAGE).stat().st_size
            self.assertGreaterEqual(update_start, 0x9000)
            self.assertFalse(
                manifests.overlaps(update_start, update_end, partitions["nvs"])
            )
            self.assertFalse(
                manifests.overlaps(update_start, update_end, partitions["storage"])
            )

    def test_rejects_update_image_that_reaches_littlefs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_update_overlap_") as temporary:
            release_dir = Path(temporary)
            partitions = self.write_valid_release_images(release_dir)
            with (release_dir / manifests.UPDATE_IMAGE).open("wb") as handle:
                handle.truncate(partitions["app"].size + 1)
            with self.assertRaisesRegex(ValueError, "exceeds app partition"):
                manifests.write_manifests(release_dir, "v2.0.3", contract.PARTITIONS)

    def test_rejects_update_image_that_is_not_the_production_app(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_update_identity_") as temporary:
            release_dir = Path(temporary)
            self.write_valid_release_images(release_dir)
            (release_dir / manifests.UPDATE_IMAGE).write_bytes(b"different firmware")
            with self.assertRaisesRegex(ValueError, "exact production firmware.bin"):
                manifests.write_manifests(release_dir, "v2.0.3", contract.PARTITIONS)

    def test_rejects_fresh_image_missing_a_component(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_fresh_component_") as temporary:
            release_dir = Path(temporary)
            partitions = self.write_valid_release_images(release_dir)
            merged = bytearray((release_dir / manifests.FRESH_IMAGE).read_bytes())
            storage = partitions["storage"]
            merged[storage.offset : storage.offset + 8] = b"\xff" * 8
            (release_dir / manifests.FRESH_IMAGE).write_bytes(merged)
            with self.assertRaisesRegex(ValueError, "littlefs.bin"):
                manifests.write_manifests(release_dir, "v2.0.3", contract.PARTITIONS)


if __name__ == "__main__":
    unittest.main(verbosity=2)
