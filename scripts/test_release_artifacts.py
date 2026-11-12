#!/usr/bin/env python3
"""Output-based regression tests for release packaging."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

import check_web_installer_page as installer
from build_context import FlashPolicy
import package_release_artifacts as package
import write_release_manifests as manifests
import stage_release_source as source


FIRMWARE = b"firmware\0Firmware: 2.0.3\0"


class ReleaseArtifactTests(unittest.TestCase):
    def make_build(self, root: Path) -> Path:
        build = root / "build"
        build.mkdir()
        partitions = manifests.read_partitions(manifests.DEFAULT_PARTITIONS)
        for name, content in {
            manifests.BOOTLOADER_IMAGE: b"bootloader",
            manifests.PARTITION_TABLE_IMAGE: b"partition-table",
            manifests.APP_IMAGE: FIRMWARE,
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
            if command[1:3] == ["scripts/generate_web_bundle.py", "verify-firmware"]:
                self.assertEqual(command[3], str(release / manifests.APP_IMAGE))
                self.assertTrue((release / manifests.APP_IMAGE).is_file())
                return
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
                    source, "stage_source",
                    side_effect=lambda *_args: (release / source.ARCHIVE_NAME).write_bytes(b"source"),
                ),
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
                    flash=FlashPolicy("dio", "80m", "16MB"),
                    arduino_source_dir=root / "framework",
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
            self.assertEqual(update["builds"][0]["parts"][0]["size"], len(FIRMWARE))
            self.assertEqual(update["builds"][0]["parts"][0]["sha256"], hashlib.sha256(FIRMWARE).hexdigest())
            self.assertEqual(fresh["builds"][0]["parts"], [{"path": manifests.FRESH_IMAGE, "offset": 0}])
            self.assertTrue((release / "pages" / manifests.UPDATE_IMAGE).is_file())
            self.assertEqual(
                (release / "pages" / "preserve-install.mjs").read_bytes(),
                (installer.ROOT / "web-installer" / "preserve-install.mjs").read_bytes(),
            )
            self.assertTrue((release / "pages" / "licenses" / "ArduinoJson-LICENSE.txt").is_file())
            self.assertEqual((release / "LGPL-2.1.txt").read_bytes(), (release / "pages/licenses/LGPL-2.1.txt").read_bytes())
            self.assertEqual((release / source.ARCHIVE_NAME).read_bytes(), (release / "pages" / source.ARCHIVE_NAME).read_bytes())
            self.assertEqual(
                (release / "pages" / "first-use.html").read_bytes(),
                (package.ROOT / "interface" / "static" / "first-use.html").read_bytes(),
            )
            installer_html = (release / "pages" / "index.html").read_text(encoding="utf-8")
            self.assertIn('href="first-use.html"', installer_html)
            self.assertNotIn('../interface/static/first-use.html', installer_html)
            self.assertEqual(calls[0][1:3], ["scripts/generate_web_bundle.py", "verify-firmware"])
            self.assertEqual(len(calls), 4)

    def test_package_stops_before_merge_when_firmware_web_bundle_is_stale(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_stale_web_") as temporary:
            root = Path(temporary)
            build = self.make_build(root)
            release = root / "release"
            calls: list[list[str]] = []

            def reject_stale_bundle(command: list[str]) -> None:
                calls.append(command)
                self.assertEqual(command[1:3], ["scripts/generate_web_bundle.py", "verify-firmware"])
                self.assertEqual(command[3], str(release / manifests.APP_IMAGE))
                raise RuntimeError("firmware does not contain the current web bundle")

            with mock.patch.object(package, "run_checked", reject_stale_bundle):
                with self.assertRaisesRegex(RuntimeError, "current web bundle"):
                    package.package_release(
                        build_dir=build,
                        release_dir=release,
                        version="v2.0.3",
                        environment="waveshare-349",
                        partitions_path=manifests.DEFAULT_PARTITIONS,
                        flash=FlashPolicy("dio", "80m", "16MB"),
                        arduino_source_dir=root / "framework",
                    )

            self.assertEqual(len(calls), 1)
            self.assertFalse((release / manifests.FRESH_IMAGE).exists())
            self.assertFalse((release / manifests.UPDATE_MANIFEST).exists())
            self.assertFalse((release / "pages").exists())

    def test_installer_rejects_missing_or_mismatched_update_identity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="installer_update_identity_") as temporary:
            site = Path(temporary)
            image = b"firmware"
            (site / manifests.UPDATE_IMAGE).write_bytes(image)
            valid = {"size": len(image), "sha256": hashlib.sha256(image).hexdigest()}
            for changed in (
                {"size": None}, {"size": 0}, {"size": True}, {"size": 0xD80001},
                {"size": len(image) + 1}, {"sha256": None}, {"sha256": "invalid"},
                {"sha256": "0" * 64},
            ):
                with self.subTest(changed=changed):
                    document = manifests.manifest("v2.0.3", manifests.UPDATE_IMAGE, 0x20000)
                    document["builds"][0]["parts"][0].update({**valid, **changed})
                    (site / manifests.UPDATE_MANIFEST).write_text(json.dumps(document), encoding="utf-8")
                    errors, _ = installer.validate_manifest(
                        site, manifests.UPDATE_MANIFEST, require_assets=True, expected_version="v2.0.3"
                    )
                    self.assertTrue(errors, "invalid update identity was accepted")

            document["builds"][0]["parts"][0].update(valid)
            (site / manifests.UPDATE_MANIFEST).write_text(json.dumps(document), encoding="utf-8")
            (site / manifests.UPDATE_IMAGE).write_bytes(b"modified")
            errors, _ = installer.validate_manifest(
                site, manifests.UPDATE_MANIFEST, require_assets=True, expected_version="v2.0.3"
            )
            self.assertTrue(any("SHA-256 does not match" in error for error in errors))

    def test_standalone_package_cli_needs_no_platformio(self) -> None:
        arguments = ["package_release_artifacts.py", "--version", "v2.0.3", "--build-dir", "/fixture/build",
                     "--partitions", "/fixture/geometry.csv", "--release-dir", "/fixture/release",
                     "--arduino-source-dir", "/fixture/arduino",
                     "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "16MB"]
        with (mock.patch.object(package.sys, "argv", arguments),
              mock.patch.object(package, "resolve", side_effect=AssertionError("must remain package-only")),
              mock.patch.object(package, "package_release") as build):
            self.assertEqual(package.main(), 0)
        self.assertEqual(build.call_args.kwargs["flash"], FlashPolicy("dio", "80m", "16MB"))
        self.assertEqual(build.call_args.kwargs["partitions_path"], Path("/fixture/geometry.csv"))
        self.assertEqual(build.call_args.kwargs["arduino_source_dir"], Path("/fixture/arduino"))

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
                    flash=FlashPolicy("dio", "80m", "16MB"),
                    arduino_source_dir=root / "framework",
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
                    flash=FlashPolicy("dio", "80m", "16MB"),
                    arduino_source_dir=root / "framework",
                )

    def test_mislabeled_image_cannot_be_packaged_or_given_manifests(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_wrong_version_") as temporary:
            root = Path(temporary)
            build = self.make_build(root)
            release = root / "release"
            with mock.patch.object(package, "run_checked") as command:
                with self.assertRaisesRegex(ValueError, "selected version '2.0.4'"):
                    package.package_release(
                        build_dir=build, release_dir=release, version="v2.0.4", environment="waveshare-349",
                        partitions_path=manifests.DEFAULT_PARTITIONS, flash=FlashPolicy("dio", "80m", "16MB"),
                        arduino_source_dir=root / "framework",
                    )
                command.assert_not_called()
            self.write_merged_image(release)
            with self.assertRaisesRegex(ValueError, "selected version '2.0.4'"):
                manifests.write_manifests(release, "v2.0.4", manifests.DEFAULT_PARTITIONS)
            self.assertFalse((release / manifests.UPDATE_MANIFEST).exists())

    def test_corresponding_source_checks_build_receipt_and_full_framework(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_source_") as temporary:
            root = Path(temporary)
            project = root / "project"
            for name, content in {
                ".gitignore": "local-note.txt\nsrc/captures/\n__pycache__/\n",
                "src/main.cpp": "int main() {}\n",
                "licenses/LGPL-2.1.txt": "verbatim license fixture\n",
                "docs/ARDUINO_ESP32_REBUILD.md": "build steps\n",
            }.items():
                file = project / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(content, encoding="utf-8")
            (project / "scripts").mkdir()
            for name in (
                "build_production_artifacts.sh", "stage_release_source.py",
                "check_release_firmware_version.py", "verify_esp32s3_framework.py",
            ):
                shutil.copyfile(source.ROOT / "scripts" / name, project / "scripts" / name)
            subprocess.run(["git", "init", "-q", str(project)], check=True)
            subprocess.run(["git", "-C", str(project), "add", "."], check=True)
            subprocess.run([
                "git", "-C", str(project), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                "commit", "-qm", "Fixture",
            ], check=True)
            sha = source.clean_source_sha(project)
            (project / "local-note.txt").write_text("local state\n", encoding="utf-8")

            framework = root / "framework"
            expected = {}
            for key, name in source.ARDUINO_INPUTS.items():
                file = framework / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(f"patched {name}\n", encoding="utf-8")
                expected[key] = hashlib.sha256(file.read_bytes()).hexdigest()
            extra = framework / "cores/esp32/main.cpp"
            extra.parent.mkdir(parents=True)
            extra.write_text("unmodified library source\n", encoding="utf-8")
            (framework / ".piopm").write_text("local install state\n", encoding="utf-8")
            build = self.make_build(root)
            (build / "framework_contract.json").write_text(json.dumps(expected), encoding="utf-8")
            release = root / "release"
            release.mkdir()
            package.copy_build_outputs(build, release)

            with mock.patch.object(source, "EXPECTED", expected):
                source.record_build(project, build, framework, sha, "v2.0.3")
                for name in ("src/extra.cpp", "src/captures/extra.cpp"):
                    with self.subTest(untracked_source=name):
                        rogue = project / name
                        rogue.parent.mkdir(parents=True, exist_ok=True)
                        rogue.write_text("void extra() {}\n", encoding="utf-8")
                        build_attempt = subprocess.run(
                            ["bash", str(project / "scripts/build_production_artifacts.sh")],
                            env={**os.environ, "V1_RELEASE_VERSION": "2.0.3", "PIO_CMD": "unavailable-fixture-pio"},
                            capture_output=True, text=True,
                        )
                        self.assertNotEqual(build_attempt.returncode, 0)
                        self.assertIn("untracked build inputs", build_attempt.stderr)
                        self.assertNotIn("unavailable-fixture-pio", build_attempt.stderr)
                        with self.assertRaisesRegex(ValueError, "untracked build inputs"):
                            source.record_build(project, build, framework, sha, "v2.0.3")
                        with self.assertRaisesRegex(ValueError, "untracked build inputs"):
                            source.stage_source(project, build, framework, release, "v2.0.3")
                        rogue.unlink()
                source.stage_source(project, build, framework, release, "v2.0.3")
                with tarfile.open(release / source.ARCHIVE_NAME) as archive:
                    names = archive.getnames()
                    self.assertNotIn("v1simple/local-note.txt", names)
                    self.assertNotIn(f"{source.FRAMEWORK_DIR}/.piopm", names)
                    self.assertIn("v1simple/src/main.cpp", names)
                    self.assertIn("README.md", names)
                    self.assertIn("LGPL-2.1.txt", names)
                    self.assertIn(f"{source.FRAMEWORK_DIR}/cores/esp32/main.cpp", names)
                    receipt = json.load(archive.extractfile(source.RECEIPT_NAME))
                    self.assertEqual(receipt["tag"], "v2.0.3")
                    self.assertEqual(receipt["source_sha"], sha)
                    self.assertEqual(receipt["firmware_sha256"], hashlib.sha256(FIRMWARE).hexdigest())

                extra.write_text("changed after the build\n", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "tree changed"):
                    source.stage_source(project, build, framework, release, "v2.0.3")
                with self.assertRaisesRegex(ValueError, "receipt disagrees"):
                    source.stage_source(project, build, framework, release, "v2.0.4")

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
            (site / "preserve-install.mjs").write_bytes(
                (installer.ROOT / "web-installer" / "preserve-install.mjs").read_bytes()
            )
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
            (site / "preserve-install.mjs").write_bytes(
                (installer.ROOT / "web-installer" / "preserve-install.mjs").read_bytes()
            )
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
