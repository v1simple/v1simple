#!/usr/bin/env python3
"""Regression tests for the fail-closed ESP32-S3 framework contract."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path
from unittest import mock

import verify_esp32s3_framework as verifier


def fixture(root: Path, ipc_stack: int = 2048, platform_version: str = "55.03.311") -> tuple[Path, Path, Path]:
    platform = root / "platform"
    arduino = root / "arduino"
    libs = root / "libs"
    selected = libs / "esp32s3" / "qio_opi"
    (selected / "include").mkdir(parents=True)
    platform.mkdir()
    arduino.mkdir()
    (platform / "platform.json").write_text(f'{{"version":"{platform_version}"}}', encoding="utf-8")
    (arduino / "package.json").write_text('{"version":"3.3.11"}', encoding="utf-8")
    (libs / "package.json").write_text('{"version":"5.5.5+sha.b774170ff46"}', encoding="utf-8")
    (selected / "include" / "sdkconfig.h").write_text(
        f"#define CONFIG_ESP_IPC_TASK_STACK_SIZE {ipc_stack}\n"
        "#define CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY 1\n"
        "#define CONFIG_HEAP_POISONING_LIGHT 1\n",
        encoding="utf-8",
    )
    (selected / "libesp_system.a").write_bytes(b"archive")
    (libs / "esp32s3" / "versions.txt").write_text("versions\n", encoding="utf-8")
    return platform, arduino, libs


def test_effective_sdkconfig_must_be_2048() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        paths = fixture(Path(raw), ipc_stack=1024)
        with mock.patch.object(verifier, "require_hash", return_value="qualified"):
            try:
                verifier.verify_framework(*paths, "qio_opi")
            except verifier.ContractError as exc:
                assert "does not contain exactly" in str(exc)
            else:
                raise AssertionError("1024-byte sdkconfig unexpectedly passed")


def test_framework_version_mismatch_fails_before_build() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        paths = fixture(Path(raw), platform_version="55.03.39")
        with mock.patch.object(verifier, "require_hash", return_value="qualified"):
            try:
                verifier.verify_framework(*paths, "qio_opi")
            except verifier.ContractError as exc:
                assert "platform_version mismatch" in str(exc)
            else:
                raise AssertionError("unexpected platform identity passed")


def test_linked_elf_must_materialize_2048_stack_argument() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        root = Path(raw)
        elf = root / "firmware.elf"
        objdump = root / "objdump"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")
        bad = "00000000 <esp_ipc_init>:\n  call8 0000 <xTaskCreatePinnedToCore>\n  slli a12, a12, 10\n"
        completed = subprocess.CompletedProcess([], 0, stdout=bad)
        with mock.patch.object(verifier.subprocess, "run", return_value=completed):
            try:
                verifier.verify_linked_elf(elf, objdump)
            except verifier.ContractError as exc:
                assert "does not pass a 2048-byte stack" in str(exc)
            else:
                raise AssertionError("1024-byte linked stack unexpectedly passed")


def main() -> None:
    tests = (
        test_effective_sdkconfig_must_be_2048,
        test_framework_version_mismatch_fails_before_build,
        test_linked_elf_must_materialize_2048_stack_argument,
    )
    for test in tests:
        test()
    print(f"PASS {len(tests)} ESP32-S3 framework contract regression tests")


if __name__ == "__main__":
    main()
