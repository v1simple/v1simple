#!/usr/bin/env python3
"""Focused regressions for the app-only upload layout hook."""

from __future__ import annotations

import ast
from contextlib import redirect_stdout
import io
from pathlib import Path
import unittest


SOURCE = Path(__file__).with_name("force_app_upload_offset.py")


def load_testable_symbols() -> dict[str, object]:
    """Load the hook's pure policy functions without importing PlatformIO/SCons."""

    tree = ast.parse(SOURCE.read_text(encoding="utf-8"), filename=str(SOURCE))
    admitted_assignments = {
        "APP_OFFSET",
        "BAD_DEFAULT_OFFSETS",
        "BOOT_APP0_CMD_RE",
    }
    admitted_functions = {
        "force_before_upload",
        "force_flash_layout",
        "strip_boot_app0_flags",
        "patch_upload_command",
    }
    body: list[ast.stmt] = [
        node
        for node in tree.body
        if (
            isinstance(node, ast.Import)
            and any(alias.name in {"re"} for alias in node.names)
        )
        or (
            isinstance(node, (ast.Assign, ast.AnnAssign))
            and any(
                isinstance(target, ast.Name)
                and target.id in admitted_assignments
                for target in (
                    node.targets
                    if isinstance(node, ast.Assign)
                    else [node.target]
                )
            )
        )
        or (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in admitted_functions
        )
    ]
    namespace: dict[str, object] = {}
    exec(compile(ast.Module(body=body, type_ignores=[]), str(SOURCE), "exec"), namespace)
    return namespace


class FakeBoardConfig:
    """Minimal PlatformIO board configuration used by the upload pre-action."""

    def __init__(self) -> None:
        self.values: dict[str, object] = {}

    def update(self, key: str, value: object) -> None:
        self.values[key] = value


class FakeSConsEnvironment(dict):
    """Exercise the hook against the symbolic state exposed by PlatformIO."""

    BOOTLOADER_PAIR = ("0x0000", "/build/bootloader.bin")
    PARTITIONS_PAIR = ("0x8000", "/build/partitions.bin")
    BOOT_APP0_PAIR = ("0xe000", "/framework/tools/partitions/boot_app0.bin")

    def __init__(self) -> None:
        super().__init__(
            PIOENV="waveshare-349",
            UPLOADERFLAGS=[
                "--chip",
                "esp32s3",
                "--port",
                '"$UPLOAD_PORT"',
                "--baud",
                "$UPLOAD_SPEED",
                "--before",
                "default-reset",
                "--after",
                "hard-reset",
                "write-flash",
                "-z",
                "--flash-mode",
                "${__get_board_flash_mode(__env__)}",
                "--flash-freq",
                "${__get_board_f_image(__env__)}",
                "--flash-size",
                "detect",
                *self.BOOTLOADER_PAIR,
                *self.PARTITIONS_PAIR,
                *self.BOOT_APP0_PAIR,
            ],
            UPLOADCMD=(
                "$UPLOADER $UPLOADERFLAGS $ESP32_APP_OFFSET $SOURCE"
            ),
        )
        self.board = FakeBoardConfig()

    def BoardConfig(self) -> FakeBoardConfig:  # pylint: disable=invalid-name
        return self.board

    def Replace(self, **values: object) -> None:  # pylint: disable=invalid-name
        self.update(values)

    def subst(self, value: str) -> str:
        if value != "$PIOENV":
            raise AssertionError(f"unexpected substitution request: {value}")
        return str(self["PIOENV"])


class ForceAppUploadOffsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.symbols = load_testable_symbols()

    def test_upload_command_retains_layout_without_expanding_symbolic_flags(self) -> None:
        patch_command = self.symbols["patch_upload_command"]
        command = (
            "$UPLOADER $UPLOADERFLAGS "
            "0xe000 boot_app0.bin 0x10000 firmware.bin"
        )
        patched = patch_command(command)
        self.assertIn("$UPLOADERFLAGS", patched)
        self.assertIn("0x20000 firmware.bin", patched)
        self.assertNotIn("boot_app0.bin", patched)
        self.assertNotIn("0x10000", patched)

    def test_pre_upload_hook_retains_symbolic_layout_and_marker(self) -> None:
        force_before_upload = self.symbols["force_before_upload"]
        app_offset = self.symbols["APP_OFFSET"]
        marker = (
            "[UploadOffset] pre-upload app image offset forced to 0x20000; "
            "boot_app0 skipped"
        )
        build_env = FakeSConsEnvironment()
        original_flags = list(build_env["UPLOADERFLAGS"])
        original_upload_command = build_env["UPLOADCMD"]
        output = io.StringIO()

        with redirect_stdout(output):
            force_before_upload([], [], build_env)

        flags = build_env["UPLOADERFLAGS"]
        expected_flags = list(original_flags)
        boot_app0_index = expected_flags.index(
            FakeSConsEnvironment.BOOT_APP0_PAIR[0]
        )
        self.assertEqual(
            tuple(expected_flags[boot_app0_index : boot_app0_index + 2]),
            FakeSConsEnvironment.BOOT_APP0_PAIR,
        )
        del expected_flags[boot_app0_index : boot_app0_index + 2]
        self.assertEqual(flags, expected_flags)
        after_indices = [
            index for index, value in enumerate(flags) if value == "--after"
        ]
        self.assertEqual(after_indices, [8])
        self.assertEqual(flags[after_indices[0] + 1], "hard-reset")
        self.assertEqual(flags.count(app_offset), 0)
        self.assertEqual(
            tuple(flags[-4:-2]),
            FakeSConsEnvironment.BOOTLOADER_PAIR,
        )
        self.assertEqual(
            tuple(flags[-2:]),
            FakeSConsEnvironment.PARTITIONS_PAIR,
        )
        self.assertNotIn(FakeSConsEnvironment.BOOT_APP0_PAIR[0], flags)
        self.assertNotIn(FakeSConsEnvironment.BOOT_APP0_PAIR[1], flags)
        self.assertFalse(
            any(str(flag).endswith("boot_app0.bin") for flag in flags)
        )
        self.assertEqual(build_env["UPLOADCMD"], original_upload_command)
        self.assertEqual(
            build_env["UPLOADCMD"],
            "$UPLOADER $UPLOADERFLAGS $ESP32_APP_OFFSET $SOURCE",
        )
        upload_tokens = build_env["UPLOADCMD"].split()
        app_source_pairs = [
            tuple(upload_tokens[index : index + 2])
            for index in range(len(upload_tokens) - 1)
            if upload_tokens[index] == "$ESP32_APP_OFFSET"
        ]
        self.assertEqual(app_source_pairs, [("$ESP32_APP_OFFSET", "$SOURCE")])
        self.assertEqual(upload_tokens.count("$ESP32_APP_OFFSET"), 1)
        self.assertEqual(upload_tokens.count("$SOURCE"), 1)
        self.assertEqual(build_env.board.values["upload.offset_address"], app_offset)
        self.assertNotIn("upload.after_reset", build_env.board.values)
        self.assertEqual(build_env["ESP32_APP_OFFSET"], app_offset)
        self.assertEqual(
            build_env["INTEGRATION_EXTRA_DATA"]["application_offset"],
            app_offset,
        )
        self.assertEqual(output.getvalue().splitlines(), [marker])


if __name__ == "__main__":
    unittest.main()
