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
        objcopy = root / "objcopy"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")
        objcopy.write_bytes(b"tool")

        def linked_1024(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            del kwargs
            if "-t" in command:
                return subprocess.CompletedProcess(
                    command,
                    0,
                    stdout=(
                        "420b9c78 l F .flash.text 00000090 esp_ipc_init\n"
                        "40387d50 g F .iram0.text 0000008d xTaskCreatePinnedToCore\n"
                        "3c100000 g O .rodata 00000004 v1simple_webserver_exact_body_contract\n"
                    ),
                )
            if "-h" in command:
                return subprocess.CompletedProcess(
                    command, 0,
                    stdout=(" 11 .flash.text 0018b828 42000020 42000020 000a6020 2**2\n"
                            " 18 .rodata 00000004 3c100000 3c100000 00000000 2**2\n"),
                )
            if "--dump-section" in command:
                output = Path(next(item.split("=", 1)[1] for item in command if "=" in item))
                output.write_bytes((0x56314232).to_bytes(4, "little"))
                return subprocess.CompletedProcess(command, 0, stdout="")
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=(
                    "420b9cdb:\t50cc10 slli a12, a12, 10\n"
                    "420b9cde:\tbd03 mov.n a11, a3\n"
                    "420b9ce0:\t81c221 l32r a8, 0x420823e8 (40387d50 <xTaskCreatePinnedToCore>)\n"
                    "420b9ce3:\te00800 callx8 a8\n"
                ),
            )

        with mock.patch.object(verifier.subprocess, "run", side_effect=linked_1024):
            try:
                verifier.verify_linked_elf(elf, objdump)
            except verifier.ContractError as exc:
                assert "does not materialize a 2048-byte stack" in str(exc)
            else:
                raise AssertionError("1024-byte linked stack unexpectedly passed")


def raw_linked_fixture_result(
    command: list[str], task_address: str = "40387d50", **kwargs: object
) -> subprocess.CompletedProcess[str]:
    del kwargs
    if "--disassemble=esp_ipc_init" in command:
        return subprocess.CompletedProcess(
            command,
            0,
            stdout="420b9c78 <esp_ipc_init>:\n420b9c78:\tb1008136\n",
        )
    if "-t" in command:
        return subprocess.CompletedProcess(
            command,
            0,
            stdout=(
                "420b9c78 l     F .flash.text 00000090 esp_ipc_init\n"
                "40387d50 g     F .iram0.text 0000008d xTaskCreatePinnedToCore\n"
                "3c100000 g     O .rodata 00000004 v1simple_webserver_exact_body_contract\n"
            ),
        )
    if "-h" in command:
        return subprocess.CompletedProcess(
            command,
            0,
            stdout=(" 11 .flash.text 0018b828 42000020 42000020 000a6020 2**2\n"
                    " 18 .rodata 00000004 3c100000 3c100000 00000000 2**2\n"),
        )
    if "--dump-section" in command:
        section, output_name = next(item.split("=", 1) for item in command if "=" in item)
        output = Path(output_name)
        if section == ".rodata":
            output.write_bytes((0x56314232).to_bytes(4, "little"))
        else:
            output.write_bytes(b"\0" * 0x90)
        return subprocess.CompletedProcess(command, 0, stdout="")
    if "-b" in command and "binary" in command:
        return subprocess.CompletedProcess(
            command,
            0,
            stdout=(
                "420b9cdb:\t50cc11        slli a12, a12, 11\n"
                "420b9cde:\tbd03          mov.n a11, a3\n"
                f"420b9ce0:\t81c221        l32r a8, 0x420823e8 (0x{task_address})\n"
                "420b9ce3:\te00800        callx8 a8\n"
            ),
        )
    raise AssertionError(f"unexpected command: {command}")


def test_linked_elf_raw_byte_fallback_proves_exact_symbol_call() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        root = Path(raw)
        elf = root / "firmware.elf"
        objdump = root / "xtensa-esp32s3-elf-objdump"
        objcopy = root / "xtensa-esp32s3-elf-objcopy"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")
        objcopy.write_bytes(b"tool")
        with mock.patch.object(verifier.subprocess, "run", side_effect=raw_linked_fixture_result):
            evidence = verifier.verify_linked_elf(elf, objdump)
        assert evidence["elf_ipc_stack_bytes"] == 2048


def test_linked_elf_raw_byte_fallback_rejects_wrong_call_target() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        root = Path(raw)
        elf = root / "firmware.elf"
        objdump = root / "xtensa-esp32s3-elf-objdump"
        objcopy = root / "xtensa-esp32s3-elf-objcopy"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")
        objcopy.write_bytes(b"tool")

        def wrong_target(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            del kwargs
            return raw_linked_fixture_result(command, task_address="40380000")

        with mock.patch.object(verifier.subprocess, "run", side_effect=wrong_target):
            try:
                verifier.verify_linked_elf(elf, objdump)
            except verifier.ContractError as exc:
                assert "does not materialize a 2048-byte stack" in str(exc)
            else:
                raise AssertionError("wrong linked IPC call target unexpectedly passed")


def test_linked_elf_raw_byte_fallback_rejects_1024_stack() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        root = Path(raw)
        elf = root / "firmware.elf"
        objdump = root / "xtensa-esp32s3-elf-objdump"
        objcopy = root / "xtensa-esp32s3-elf-objcopy"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")
        objcopy.write_bytes(b"tool")

        def raw_1024(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            result = raw_linked_fixture_result(command, **kwargs)
            if "-b" in command and "binary" in command:
                return subprocess.CompletedProcess(command, 0, stdout=result.stdout.replace("a12, a12, 11", "a12, a12, 10"))
            return result

        with mock.patch.object(verifier.subprocess, "run", side_effect=raw_1024):
            try:
                verifier.verify_linked_elf(elf, objdump)
            except verifier.ContractError as exc:
                assert "does not materialize a 2048-byte stack" in str(exc)
            else:
                raise AssertionError("raw linked 1024-byte stack unexpectedly passed")


def test_linked_elf_raw_byte_fallback_rejects_a12_clobber_before_call() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        root = Path(raw)
        elf = root / "firmware.elf"
        objdump = root / "xtensa-esp32s3-elf-objdump"
        objcopy = root / "xtensa-esp32s3-elf-objcopy"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")
        objcopy.write_bytes(b"tool")

        def clobbered(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            result = raw_linked_fixture_result(command, **kwargs)
            if "-b" in command and "binary" in command:
                text = result.stdout.replace(
                    "420b9ce0:",
                    "420b9cde:\t0c0c          movi.n a12, 0\n420b9ce0:",
                )
                return subprocess.CompletedProcess(command, 0, stdout=text)
            return result

        with mock.patch.object(verifier.subprocess, "run", side_effect=clobbered):
            try:
                verifier.verify_linked_elf(elf, objdump)
            except verifier.ContractError as exc:
                assert "does not materialize a 2048-byte stack" in str(exc)
            else:
                raise AssertionError("raw linked a12 clobber unexpectedly passed")


def test_linked_elf_raw_byte_fallback_rejects_missing_call() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        root = Path(raw)
        elf = root / "firmware.elf"
        objdump = root / "xtensa-esp32s3-elf-objdump"
        objcopy = root / "xtensa-esp32s3-elf-objcopy"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")
        objcopy.write_bytes(b"tool")

        def no_call(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            result = raw_linked_fixture_result(command, **kwargs)
            if "-b" in command and "binary" in command:
                return subprocess.CompletedProcess(command, 0, stdout=result.stdout.replace("callx8 a8", "nop.n"))
            return result

        with mock.patch.object(verifier.subprocess, "run", side_effect=no_call):
            try:
                verifier.verify_linked_elf(elf, objdump)
            except verifier.ContractError as exc:
                assert "does not materialize a 2048-byte stack" in str(exc)
            else:
                raise AssertionError("raw linked missing call unexpectedly passed")


def test_linked_elf_rejects_missing_exact_task_symbol_before_fallback() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        root = Path(raw)
        elf = root / "firmware.elf"
        objdump = root / "xtensa-esp32s3-elf-objdump"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")

        def missing_symbol(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            del kwargs
            assert "-t" in command
            return subprocess.CompletedProcess(
                command,
                0,
                stdout="420b9c78 l F .flash.text 00000090 esp_ipc_init\n",
            )

        with mock.patch.object(verifier.subprocess, "run", side_effect=missing_symbol):
            try:
                verifier.verify_linked_elf(elf, objdump)
            except verifier.ContractError as exc:
                assert "does not expose xTaskCreatePinnedToCore" in str(exc)
            else:
                raise AssertionError("missing linked task symbol unexpectedly passed")


def test_linked_elf_rejects_missing_webserver_body_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        root = Path(raw)
        elf = root / "firmware.elf"
        objdump = root / "xtensa-esp32s3-elf-objdump"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")

        def missing_contract(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            del kwargs
            assert "-t" in command
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=(
                    "420b9c78 l F .flash.text 00000090 esp_ipc_init\n"
                    "40387d50 g F .iram0.text 0000008d xTaskCreatePinnedToCore\n"
                ),
            )

        with mock.patch.object(verifier.subprocess, "run", side_effect=missing_contract):
            try:
                verifier.verify_linked_elf(elf, objdump)
            except verifier.ContractError as exc:
                assert "does not expose v1simple_webserver_exact_body_contract" in str(exc)
            else:
                raise AssertionError("stock linked WebServer body reader unexpectedly passed")


def test_linked_elf_rejects_wrong_webserver_body_contract_value() -> None:
    with tempfile.TemporaryDirectory(prefix="framework-contract-") as raw:
        root = Path(raw)
        elf = root / "firmware.elf"
        objdump = root / "xtensa-esp32s3-elf-objdump"
        objcopy = root / "xtensa-esp32s3-elf-objcopy"
        elf.write_bytes(b"elf")
        objdump.write_bytes(b"tool")
        objcopy.write_bytes(b"tool")

        def wrong_contract(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            result = raw_linked_fixture_result(command, **kwargs)
            if "--dump-section" in command and any(item.startswith(".rodata=") for item in command):
                output = Path(next(item.split("=", 1)[1] for item in command if item.startswith(".rodata=")))
                output.write_bytes((0xDEADBEEF).to_bytes(4, "little"))
                return subprocess.CompletedProcess(command, 0, stdout="")
            return result

        with mock.patch.object(verifier.subprocess, "run", side_effect=wrong_contract):
            try:
                verifier.verify_linked_elf(elf, objdump)
            except verifier.ContractError as exc:
                assert "marker value is incorrect" in str(exc)
            else:
                raise AssertionError("incorrect linked WebServer body contract unexpectedly passed")


def main() -> None:
    tests = (
        test_effective_sdkconfig_must_be_2048,
        test_framework_version_mismatch_fails_before_build,
        test_linked_elf_must_materialize_2048_stack_argument,
        test_linked_elf_raw_byte_fallback_proves_exact_symbol_call,
        test_linked_elf_raw_byte_fallback_rejects_wrong_call_target,
        test_linked_elf_raw_byte_fallback_rejects_1024_stack,
        test_linked_elf_raw_byte_fallback_rejects_a12_clobber_before_call,
        test_linked_elf_raw_byte_fallback_rejects_missing_call,
        test_linked_elf_rejects_missing_exact_task_symbol_before_fallback,
        test_linked_elf_rejects_missing_webserver_body_contract,
        test_linked_elf_rejects_wrong_webserver_body_contract_value,
    )
    for test in tests:
        test()
    print(f"PASS {len(tests)} ESP32-S3 framework contract regression tests")


if __name__ == "__main__":
    main()
