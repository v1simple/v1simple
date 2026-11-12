#!/usr/bin/env python3
"""Regression tests for fail-fast Linux validation prerequisites."""

from __future__ import annotations

import unittest
from unittest.mock import patch

import check_linux_validation_environment as checker


def valid_snapshot(profile: str = "ci", **changes: object) -> checker.EnvironmentSnapshot:
    values: dict[str, object] = {
        "profile": profile,
        "source_commit": "a" * 40,
        "source_clean": True,
        "image_identity": "fixture",
        "os_id": "ubuntu",
        "os_version": "24.04",
        "architecture": "x86_64",
        "python_version": "3.12.12",
        "node_version": '22.23.3',
        "npm_version": '10.9.9',
        "pioarduino_version": '6.2.0',
        "esptool_version": '5.4.0',
        "esp_idf_size_version": '2.2.1',
        "ruff_version": '0.16.0',
        "shellcheck_version": '0.11.0',
        "swift_version": '6.3.3',
        "ffmpeg_version": "7.1.1",
        "libpcre3_available": True,
    }
    values.update(changes)
    return checker.EnvironmentSnapshot(**values)  # type: ignore[arg-type]


class LinuxValidationEnvironmentTests(unittest.TestCase):
    def test_image_identity_prefers_exact_container_then_hosted_runner(self) -> None:
        self.assertEqual(
            checker.image_identity(
                {
                    "V1_CLEAN_LINUX_IMAGE": "ubuntu@sha256:abc",
                    "ImageOS": "ubuntu24",
                    "ImageVersion": "20260918.1",
                }
            ),
            "ubuntu@sha256:abc",
        )
        self.assertEqual(
            checker.image_identity(
                {"ImageOS": "ubuntu24", "ImageVersion": "20260918.1"}
            ),
            "ubuntu24:20260918.1",
        )
        self.assertEqual(checker.image_identity({}), "github-hosted-unidentified")

    def test_complete_ci_contract_passes(self) -> None:
        self.assertEqual(checker.validate_snapshot(valid_snapshot()), [])

    def test_missing_cppcheck_runtime_fails_by_name(self) -> None:
        errors = checker.validate_snapshot(valid_snapshot(libpcre3_available=False))
        self.assertIn(
            "libpcre3 runtime is missing for the pinned cppcheck binary", errors
        )

    def test_missing_swift_fails_before_protocol_contract(self) -> None:
        errors = checker.validate_snapshot(valid_snapshot(swift_version=""))
        self.assertIn("swift_version is missing; required 6.3.3", errors)

    def test_memory_size_dependency_is_required_by_ci_and_release(self) -> None:
        for profile in ("ci", "release"):
            for version in ("", "2.2.0"):
                with self.subTest(profile=profile, version=version):
                    errors = checker.validate_snapshot(valid_snapshot(profile, esp_idf_size_version=version))
                    self.assertIn(f"esp_idf_size_version is {version or 'missing'}; required 2.2.1", errors)

    def test_memory_size_probe_imports_cli_with_validation_interpreter(self) -> None:
        interpreter = "/fixture/validation/bin/python"
        command = [interpreter, "-c",
                   "import esp_idf_size; import esp_idf_size.main; print(esp_idf_size.__version__)"]
        with patch.object(checker.sys, "executable", interpreter), \
                patch.object(checker, "command_output", side_effect=lambda args: "2.2.1" if args == command else "") as probe, \
                patch.object(checker, "tracked_source_is_clean", return_value=True), \
                patch.object(checker.check_platformio_core_version, "installed_pioarduino_version", return_value=(6, 2, 0)):
            snapshot = checker.collect_snapshot("ci", checker.ROOT)
        probe.assert_any_call(command)
        self.assertEqual(snapshot.esp_idf_size_version, "2.2.1")

    def test_release_profile_preserves_smaller_dependency_surface(self) -> None:
        snapshot = valid_snapshot(
            "release",
            ruff_version="",
            shellcheck_version="",
            swift_version="",
            ffmpeg_version="",
            libpcre3_available=False,
        )
        self.assertEqual(checker.validate_snapshot(snapshot), [])

    def test_source_or_runtime_drift_fails_before_gate(self) -> None:
        errors = checker.validate_snapshot(
            valid_snapshot(source_clean=False, node_version="22.23.1")
        )
        self.assertTrue(any("tracked source" in error for error in errors), errors)
        self.assertTrue(any("node_version" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main(verbosity=2)
