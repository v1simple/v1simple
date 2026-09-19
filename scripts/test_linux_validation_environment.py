#!/usr/bin/env python3
"""Regression tests for fail-fast Linux validation prerequisites."""

from __future__ import annotations

import unittest

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
        "node_version": checker.EXPECTED_NODE,
        "npm_version": checker.EXPECTED_NPM,
        "pioarduino_version": checker.EXPECTED_PIO,
        "esptool_version": checker.EXPECTED_ESPTOOL,
        "ruff_version": checker.EXPECTED_RUFF,
        "shellcheck_version": checker.EXPECTED_SHELLCHECK,
        "swift_version": checker.EXPECTED_SWIFT,
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
