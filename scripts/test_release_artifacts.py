#!/usr/bin/env python3
"""Output-based regression tests for release packaging."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import check_web_installer_page as installer
import package_release_artifacts as package
import write_release_manifests as manifests


class ReleaseArtifactTests(unittest.TestCase):
    def make_build(self, root: Path) -> Path:
        build = root / "build"
        build.mkdir()
        partitions = manifests.read_partitions(manifests.DEFAULT_PARTITIONS)
        for name, content in {
            manifests.BOOTLOADER_IMAGE: b"bootloader",
            manifests.PARTITION_TABLE_IMAGE: b"partition-table",
            manifests.APP_IMAGE: b"firmware",
            "littlefs.bin": b"l" * partitions["storage"].size,
        }.items():
            (build / name).write_bytes(content)
        return build

    def write_merged_image(self, release: Path) -> None:
        partitions = manifests.read_partitions(manifests.DEFAULT_PARTITIONS)
        end = partitions["storage"].end
        merged = bytearray(b"\xff" * end)
        for name, offset in (
            (manifests.BOOTLOADER_IMAGE, 0),
            (manifests.PARTITION_TABLE_IMAGE, manifests.PARTITION_TABLE_OFFSET),
            (manifests.APP_IMAGE, partitions["app"].offset),
            ("littlefs.bin", partitions["storage"].offset),
        ):
            content = (release / name).read_bytes()
            merged[offset : offset + len(content)] = content
        (release / manifests.FRESH_IMAGE).write_bytes(merged)

    def emulate_external_tools(self, release: Path, calls: list[list[str]]):
        def run(command: list[str]) -> None:
            calls.append(command)
            if Path(command[0]).name == "esptool":
                self.write_merged_image(release)
                return
            if command[1].endswith("check_release_image_info.py"):
                (release / "merged-firmware-image-info.txt").write_text(
                    "validated image\n", encoding="utf-8"
                )
                return
            if command[1].endswith("check_web_installer_page.py"):
                with mock.patch.object(installer.sys, "argv", command[1:]):
                    self.assertEqual(installer.main(), 0)
                return
            self.fail(f"unexpected release command: {command}")

        return run

    def test_package_contains_validated_release_and_installer_outputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_package_") as temporary:
            root = Path(temporary)
            build = self.make_build(root)
            release = root / "release"
            calls: list[list[str]] = []

            with (
                mock.patch.object(package.shutil, "which", return_value="/fixture/esptool"),
                mock.patch.object(
                    package,
                    "run_checked",
                    self.emulate_external_tools(release, calls),
                ),
            ):
                package.package_release(
                    build_dir=build,
                    release_dir=release,
                    version="v2.0.3",
                    environment="waveshare-349",
                    partitions_path=manifests.DEFAULT_PARTITIONS,
                )

            manifests.validate_release_images(
                release,
                manifests.read_partitions(manifests.DEFAULT_PARTITIONS),
            )
            update = json.loads(
                (release / manifests.UPDATE_MANIFEST).read_text(encoding="utf-8")
            )
            fresh = json.loads(
                (release / manifests.FRESH_MANIFEST).read_text(encoding="utf-8")
            )
            self.assertEqual(update["version"], "v2.0.3")
            self.assertEqual(update["builds"][0]["parts"][0]["path"], manifests.UPDATE_IMAGE)
            self.assertEqual(fresh["builds"][0]["parts"], [{"path": manifests.FRESH_IMAGE, "offset": 0}])
            self.assertTrue((release / "pages" / manifests.UPDATE_IMAGE).is_file())
            self.assertTrue((release / "pages" / "licenses" / "ArduinoJson-LICENSE.txt").is_file())
            self.assertEqual(len(calls), 3)

    def test_package_refuses_mixed_or_missing_inputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_refusal_") as temporary:
            root = Path(temporary)
            build = self.make_build(root)
            release = root / "release"
            release.mkdir()
            (release / "old.bin").write_bytes(b"old")
            with self.assertRaisesRegex(ValueError, "not empty"):
                package.package_release(
                    build_dir=build,
                    release_dir=release,
                    version="v2.0.3",
                    environment="waveshare-349",
                    partitions_path=manifests.DEFAULT_PARTITIONS,
                )

            release.joinpath("old.bin").unlink()
            build.joinpath(manifests.APP_IMAGE).unlink()
            with self.assertRaisesRegex(ValueError, "missing or empty"):
                package.package_release(
                    build_dir=build,
                    release_dir=release,
                    version="v2.0.3",
                    environment="waveshare-349",
                    partitions_path=manifests.DEFAULT_PARTITIONS,
                )

    def test_manifest_rejects_nonidentical_or_oversized_update(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_manifest_") as temporary:
            root = Path(temporary)
            build = self.make_build(root)
            release = root / "release"
            release.mkdir()
            partitions = manifests.read_partitions(manifests.DEFAULT_PARTITIONS)
            app = partitions["app"]
            for name in package.RELEASE_IMAGES:
                (release / name).write_bytes((build / name).read_bytes())
            self.write_merged_image(release)
            (release / manifests.UPDATE_IMAGE).write_bytes(b"different")
            with self.assertRaisesRegex(ValueError, "not the exact production"):
                manifests.validate_release_images(release, partitions)

            (release / manifests.UPDATE_IMAGE).write_bytes(b"firmware")
            with (release / manifests.UPDATE_IMAGE).open("wb") as handle:
                handle.truncate(app.size + 1)
            with self.assertRaisesRegex(ValueError, "exceeds app partition"):
                manifests.validate_release_images(release, partitions)

    def test_installer_rejects_unpinned_module_without_integrity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="installer_dependency_") as temporary:
            site = Path(temporary)
            template = (installer.ROOT / "web-installer" / "index.html").read_text(
                encoding="utf-8"
            )
            expected_url, expected_integrity, expected_crossorigin = installer.EXPECTED_INSTALL_SCRIPT
            template = template.replace(
                expected_url,
                "https://unpkg.com/esp-web-tools@latest/dist/web/install-button.js?module",
            )
            template = template.replace(f'\n    integrity="{expected_integrity}"', "")
            template = template.replace(
                f'\n    crossorigin="{expected_crossorigin}"', "", 1
            )
            (site / "index.html").write_text(template, encoding="utf-8")
            branding = site / "../interface/static/branding/v1simple-logo-transparent.png"
            branding.parent.mkdir(parents=True, exist_ok=True)
            branding.write_bytes(b"branding")

            with mock.patch.object(
                installer.sys,
                "argv",
                ["checker", "--site-dir", str(site), "--template-only"],
            ):
                self.assertEqual(installer.main(), 1)

    def test_installer_rejects_unpinned_analytics_without_integrity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="installer_analytics_") as temporary:
            site = Path(temporary)
            template = (installer.ROOT / "web-installer" / "index.html").read_text(
                encoding="utf-8"
            )
            endpoint, settings, expected_url, expected_integrity, _ = (
                installer.EXPECTED_ANALYTICS_SCRIPT
            )
            self.assertIn(endpoint, template)
            self.assertIn(settings, template)
            template = template.replace(
                expected_url,
                "https://gc.zgo.at/count.js",
            )
            template = template.replace(f'\n    integrity="{expected_integrity}"', "")
            (site / "index.html").write_text(template, encoding="utf-8")
            branding = site / "../interface/static/branding/v1simple-logo-transparent.png"
            branding.parent.mkdir(parents=True, exist_ok=True)
            branding.write_bytes(b"branding")

            with mock.patch.object(
                installer.sys,
                "argv",
                ["checker", "--site-dir", str(site), "--template-only"],
            ):
                self.assertEqual(installer.main(), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
