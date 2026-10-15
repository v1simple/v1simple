#!/usr/bin/env python3
"""Regression tests for exact release-tool version gates."""

from __future__ import annotations

from contextlib import ExitStack
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

import check_esptool_version
import check_platformio_core_version


class PlatformIOInterpreterDiscoveryTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="pio discovery ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def launcher(self, interpreter: str, *, quoted: bool = True) -> Path:
        scripts = self.root / "selected installation" / "Scripts"
        scripts.mkdir(parents=True, exist_ok=True)
        launcher = scripts / "pio.exe"
        # pip's Windows layout is a binary launcher, an interpreter shebang,
        # then its ZIP entry point. The executable itself is not UTF-8 text.
        shebang = f'#!"{interpreter}"\n' if quoted else f"#!{interpreter}\n"
        launcher.write_bytes(b"MZ\x00\xff\x80\x00" + shebang.encode("utf-8"))
        with zipfile.ZipFile(launcher, "a") as archive:
            archive.writestr("__main__.py", "# PlatformIO entry point fixture\n")
        return launcher

    def test_windows_launcher_keeps_recorded_path_with_spaces_and_backslashes(self) -> None:
        for interpreter, quoted in (
            (r"C:\Toolchains\Selected Python\python.exe", True),
            (r"C:\Toolchains\Python\python.exe", False),
        ):
            with self.subTest(interpreter=interpreter):
                launcher = self.launcher(interpreter, quoted=quoted)
                self.assertEqual(check_platformio_core_version.pio_interpreter(str(launcher)), interpreter)

    def check_windows_installation(self, distribution: str | None) -> int:
        selected = str(self.root / "recorded installation" / "python.exe")
        launcher = self.launcher(selected)
        (launcher.parent / "python.exe").write_bytes(b"unrelated adjacent interpreter")
        unrelated = str(self.root / "unrelated installation" / "python.exe")
        calls: list[list[str]] = []

        def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append(command)
            if command == [str(launcher), "--version"]:
                return subprocess.CompletedProcess(command, 0, "PlatformIO Core, version 6.2.0")
            self.assertEqual(command[0], selected)
            self.assertEqual(command[1], "-c")
            self.assertIn("version('pioarduino')", command[2])
            if distribution is None:
                raise FileNotFoundError(selected)
            return subprocess.CompletedProcess(command, 0 if distribution else 1, distribution)

        with (
            mock.patch.object(sys, "argv", ["checker", "--pio", str(launcher)]),
            mock.patch.object(sys, "executable", unrelated),
            mock.patch.object(check_platformio_core_version.subprocess, "run", side_effect=run),
        ):
            result = check_platformio_core_version.main()
        self.assertEqual(len(calls), 2)
        return result

    def test_windows_launcher_checks_its_own_distribution(self) -> None:
        self.assertEqual(self.check_windows_installation("6.2.0"), 0)

    def test_wrong_windows_installation_does_not_use_current_python(self) -> None:
        self.assertEqual(self.check_windows_installation("6.2.1"), 1)

    def test_missing_windows_interpreter_does_not_use_current_python(self) -> None:
        self.assertEqual(self.check_windows_installation(None), 1)

    def test_missing_windows_distribution_does_not_use_current_python(self) -> None:
        self.assertEqual(self.check_windows_installation(""), 1)

    def test_malformed_windows_launchers_do_not_guess_an_interpreter(self) -> None:
        launcher = self.launcher("unused")
        (launcher.parent / "python.exe").write_bytes(b"unrelated interpreter")
        for contents in (
            b"MZ\x00\xff",
            b"MZ\x00\xff#!C:\\Python\\python.exe\nnot a ZIP entry point",
            b'MZ\x00\xff#!"unterminated\nPK\x03\x04',
            b"MZ\x00\xff#!\xff\nPK\x03\x04",
        ):
            with self.subTest(contents=contents):
                launcher.write_bytes(contents)
                self.assertIsNone(check_platformio_core_version.pio_interpreter(str(launcher)))

    def test_unix_direct_shebang_with_spaces_is_preserved(self) -> None:
        interpreter = str(self.root / "selected installation" / "bin" / "python")
        launcher = self.root / "pio"
        launcher.write_text(f'#!"{interpreter}"\nprint("entry point")\n', encoding="utf-8")
        self.assertEqual(check_platformio_core_version.pio_interpreter(str(launcher)), interpreter)

    def test_unix_env_shebang_uses_the_same_path_lookup_as_the_launcher(self) -> None:
        launcher = self.root / "pio"
        launcher.write_text("#!/usr/bin/env python3\n", encoding="utf-8")
        interpreter = str(self.root / "selected installation" / "bin" / "python3")
        with mock.patch.object(check_platformio_core_version.shutil, "which", return_value=interpreter) as which:
            self.assertEqual(check_platformio_core_version.pio_interpreter(str(launcher)), interpreter)
        which.assert_called_once_with("python3")

    def test_missing_launcher_is_unreadable(self) -> None:
        self.assertIsNone(check_platformio_core_version.pio_interpreter(str(self.root / "missing")))


class BuildToolVersionTests(unittest.TestCase):
    @staticmethod
    def completed(output: str) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(args=[], returncode=0, stdout=output)

    def run_checker(
        self,
        module: object,
        output: str,
        *,
        pioarduino_version: tuple[int, ...] | None = (6, 2, 0),
    ) -> int:
        patches = [
            mock.patch.object(sys, "argv", ["checker"]),
            mock.patch.object(
                module.subprocess,
                "run",
                return_value=self.completed(output),
            ),
        ]
        if module is check_platformio_core_version:
            patches.append(
                mock.patch.object(
                    check_platformio_core_version,
                    "installed_pioarduino_version",
                    return_value=pioarduino_version,
                )
            )
        with ExitStack() as stack:
            for patch in patches:
                stack.enter_context(patch)
            return module.main()

    def test_platformio_exact_pin_passes(self) -> None:
        self.assertEqual(
            self.run_checker(check_platformio_core_version, "PlatformIO Core, version 6.2.0"),
            0,
        )

    def test_platformio_newer_version_does_not_pass(self) -> None:
        self.assertEqual(
            self.run_checker(check_platformio_core_version, "PlatformIO Core, version 6.2.1"),
            1,
        )

    def test_platformio_prerelease_does_not_pass_as_exact(self) -> None:
        self.assertEqual(
            self.run_checker(
                check_platformio_core_version,
                "PlatformIO Core, version 6.2.0-dev",
            ),
            1,
        )

    def test_upstream_platformio_without_pioarduino_does_not_pass(self) -> None:
        self.assertEqual(
            self.run_checker(
                check_platformio_core_version,
                "PlatformIO Core, version 6.2.0",
                pioarduino_version=None,
            ),
            1,
        )

    def test_esptool_exact_pin_passes(self) -> None:
        self.assertEqual(self.run_checker(check_esptool_version, "esptool v5.4.0\n5.4.0"), 0)

    def test_esptool_newer_version_does_not_pass(self) -> None:
        self.assertEqual(self.run_checker(check_esptool_version, "esptool v5.5.0\n5.5.0"), 1)

    def test_esptool_prerelease_does_not_pass_as_exact(self) -> None:
        self.assertEqual(
            self.run_checker(check_esptool_version, "esptool v5.4.0-dev"),
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
