#!/usr/bin/env python3
"""Regression tests for exact release-tool version gates."""

from __future__ import annotations

import subprocess
import sys
import unittest
from unittest import mock

import check_esptool_version
import check_platformio_core_version


class BuildToolVersionTests(unittest.TestCase):
    @staticmethod
    def completed(output: str) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(args=[], returncode=0, stdout=output)

    def run_checker(self, module: object, output: str) -> int:
        with (
            mock.patch.object(sys, "argv", ["checker"]),
            mock.patch.object(
                module.subprocess,
                "run",
                return_value=self.completed(output),
            ),
        ):
            return module.main()

    def test_platformio_exact_pin_passes(self) -> None:
        self.assertEqual(
            self.run_checker(check_platformio_core_version, "PlatformIO Core, version 6.1.19"),
            0,
        )

    def test_platformio_newer_version_does_not_pass(self) -> None:
        self.assertEqual(
            self.run_checker(check_platformio_core_version, "PlatformIO Core, version 6.1.20"),
            1,
        )

    def test_platformio_prerelease_does_not_pass_as_exact(self) -> None:
        self.assertEqual(
            self.run_checker(
                check_platformio_core_version,
                "PlatformIO Core, version 6.1.19-dev",
            ),
            1,
        )

    def test_esptool_exact_pin_passes(self) -> None:
        self.assertEqual(self.run_checker(check_esptool_version, "esptool v5.3.0\n5.3.0"), 0)

    def test_esptool_newer_version_does_not_pass(self) -> None:
        self.assertEqual(self.run_checker(check_esptool_version, "esptool v5.4.0\n5.4.0"), 1)

    def test_esptool_prerelease_does_not_pass_as_exact(self) -> None:
        self.assertEqual(
            self.run_checker(check_esptool_version, "esptool v5.3.0-dev"),
            1,
        )

    def test_missing_esptool_module_does_not_pass(self) -> None:
        with (
            mock.patch.object(sys, "argv", ["checker"]),
            mock.patch.object(
                check_esptool_version.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(
                    args=[],
                    returncode=1,
                    stdout="No module named esptool",
                ),
            ),
        ):
            self.assertEqual(check_esptool_version.main(), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
