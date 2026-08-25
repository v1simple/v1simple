#!/usr/bin/env python3
"""Fail closed unless the qualified ESP32-S3 framework and linked IPC stack match."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


EXPECTED = {
    "platform_version": "55.03.311",
    "arduino_version": "3.3.11",
    "libs_version": "5.5.5+sha.b774170ff46",
    "platform_json_sha256": "cf752e458b9042e4b953f4dc7a0b6ddc1f9e87a02ff1f2ee67c4cfe5acc18f55",
    "arduino_package_sha256": "9e303d060e4506a3a6370b5297fc24cf2db02c0f9bf24408c97753fb63d6d170",
    "libs_package_sha256": "fb288b25fe5b9c883c5bdd30c96cc0542723b66161ae431d7c33d8ee2bfbffab",
    "sdkconfig_sha256": "9918badd7ca474090cc8bc86d1437163b4a793f58d4c6eaffba3923f1c5b0966",
    "esp_system_archive_sha256": "3fa34619defd0e4718dac29a93673652a3ad495b2d5551f381d1c78af92c9d38",
    "versions_sha256": "bb0ce8cff5cdfc1abb17666e4e37ec85d8fff148f4e19cba8e3a5d76c1d662ed",
    "ipc_stack_bytes": 2048,
}


class ContractError(RuntimeError):
    """The installed or linked framework differs from the qualified input."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_package_version(path: Path) -> str:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))["version"]
    except (OSError, KeyError, TypeError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot read framework identity from {path.name}") from exc
    return str(value)


def require_hash(path: Path, expected: str, label: str) -> str:
    if not path.is_file():
        raise ContractError(f"missing qualified {label}: {path}")
    actual = sha256_file(path)
    if actual != expected:
        raise ContractError(f"{label} identity mismatch: expected {expected}, got {actual}")
    return actual


def verify_framework(platform_dir: Path, arduino_dir: Path, libs_dir: Path, memory_type: str) -> dict[str, object]:
    platform_json = platform_dir / "platform.json"
    arduino_package = arduino_dir / "package.json"
    libs_package = libs_dir / "package.json"
    selected = libs_dir / "esp32s3" / memory_type
    sdkconfig = selected / "include" / "sdkconfig.h"
    esp_system = selected / "libesp_system.a"
    versions = libs_dir / "esp32s3" / "versions.txt"

    versions_found = {
        "platform_version": read_package_version(platform_json),
        "arduino_version": read_package_version(arduino_package),
        "libs_version": read_package_version(libs_package),
    }
    for key, actual in versions_found.items():
        if actual != EXPECTED[key]:
            raise ContractError(f"{key} mismatch: expected {EXPECTED[key]}, got {actual}")

    hashes = {
        "platform_json_sha256": require_hash(
            platform_json, str(EXPECTED["platform_json_sha256"]), "platform manifest"
        ),
        "arduino_package_sha256": require_hash(
            arduino_package, str(EXPECTED["arduino_package_sha256"]), "Arduino package manifest"
        ),
        "libs_package_sha256": require_hash(
            libs_package, str(EXPECTED["libs_package_sha256"]), "framework-libs package manifest"
        ),
        "sdkconfig_sha256": require_hash(sdkconfig, str(EXPECTED["sdkconfig_sha256"]), "ESP32-S3 sdkconfig"),
        "esp_system_archive_sha256": require_hash(
            esp_system, str(EXPECTED["esp_system_archive_sha256"]), "ESP32-S3 esp_system archive"
        ),
        "versions_sha256": require_hash(versions, str(EXPECTED["versions_sha256"]), "framework versions ledger"),
    }

    config_text = sdkconfig.read_text(encoding="utf-8", errors="strict")
    expected_define = f"#define CONFIG_ESP_IPC_TASK_STACK_SIZE {EXPECTED['ipc_stack_bytes']}"
    if re.findall(r"^#define CONFIG_ESP_IPC_TASK_STACK_SIZE\s+\d+\s*$", config_text, re.MULTILINE) != [
        expected_define
    ]:
        raise ContractError(f"effective ESP32-S3 sdkconfig does not contain exactly {expected_define!r}")
    for required in (
        "#define CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY 1",
        "#define CONFIG_HEAP_POISONING_LIGHT 1",
    ):
        if required not in config_text:
            raise ContractError(f"qualified safety configuration missing: {required}")

    return {
        "contract": "v1simple-esp32s3-framework-v1",
        "memory_type": memory_type,
        **versions_found,
        **hashes,
        "ipc_stack_bytes": EXPECTED["ipc_stack_bytes"],
        "stack_canary": True,
        "heap_poisoning_light": True,
    }


def verify_linked_elf(elf: Path, objdump: Path) -> dict[str, object]:
    if not elf.is_file() or not objdump.is_file():
        raise ContractError("linked ELF or Xtensa objdump is missing")
    result = subprocess.run(
        [str(objdump), "-d", "--disassemble=esp_ipc_init", str(elf)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise ContractError(f"objdump could not inspect linked esp_ipc_init (exit {result.returncode})")
    disassembly = result.stdout
    if "<esp_ipc_init>:" not in disassembly or "<xTaskCreatePinnedToCore>" not in disassembly:
        raise ContractError("linked ELF does not expose the expected ESP IPC task creation path")
    # Xtensa passes the xTaskCreatePinnedToCore stack-depth argument in a12.
    # The qualified framework materializes 2048 as 1 << 11 immediately before
    # that call. A stock 1024-byte library uses a shift of 10 and fails here.
    if not re.search(r"\bslli\s+a12,\s*a12,\s*11\b", disassembly):
        raise ContractError("linked esp_ipc_init does not pass a 2048-byte stack to the IPC task")
    return {
        "elf_sha256": sha256_file(elf),
        "elf_ipc_symbol": "esp_ipc_init",
        "elf_ipc_stack_argument": "a12=1<<11",
        "elf_ipc_stack_bytes": EXPECTED["ipc_stack_bytes"],
    }


def write_evidence(path: Path, evidence: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def cli() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform-dir", type=Path, required=True)
    parser.add_argument("--arduino-dir", type=Path, required=True)
    parser.add_argument("--libs-dir", type=Path, required=True)
    parser.add_argument("--memory-type", default="qio_opi")
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--objdump", type=Path)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    evidence = verify_framework(args.platform_dir, args.arduino_dir, args.libs_dir, args.memory_type)
    if args.elf or args.objdump:
        if not args.elf or not args.objdump:
            raise ContractError("--elf and --objdump must be supplied together")
        evidence.update(verify_linked_elf(args.elf, args.objdump))
    if args.evidence:
        write_evidence(args.evidence, evidence)
    print(
        "[FrameworkContract] qualified "
        f"platform={evidence['platform_version']} arduino={evidence['arduino_version']} "
        f"libs={evidence['libs_version']} ipc_stack={evidence['ipc_stack_bytes']}"
    )
    return 0


def configure_scons() -> None:
    Import("env")  # noqa: F821  pylint: disable=undefined-variable

    platform = env.PioPlatform()  # noqa: F821  pylint: disable=undefined-variable
    platform_dir = Path(platform.get_dir())
    arduino_dir = Path(platform.get_package_dir("framework-arduinoespressif32") or "")
    libs_dir = Path(platform.get_package_dir("framework-arduinoespressif32-libs") or "")
    toolchain_dir = Path(platform.get_package_dir("toolchain-xtensa-esp-elf") or "")
    memory_type = str(env.BoardConfig().get("build.arduino.memory_type", "qio_qspi"))  # noqa: F821
    evidence_path = Path(env.subst("$BUILD_DIR")) / "framework_contract.json"  # noqa: F821

    try:
        evidence = verify_framework(platform_dir, arduino_dir, libs_dir, memory_type)
        write_evidence(evidence_path, evidence)
    except ContractError as exc:
        print(f"Error: ESP32-S3 framework contract failed: {exc}")
        env.Exit(1)  # noqa: F821  pylint: disable=undefined-variable

    def verify_after_link(target, source, env):
        del source
        del env
        try:
            linked = verify_linked_elf(
                Path(str(target[0])), toolchain_dir / "bin" / "xtensa-esp32s3-elf-objdump"
            )
            combined = dict(evidence)
            combined.update(linked)
            write_evidence(evidence_path, combined)
            print("[FrameworkContract] linked ELF passes 2048-byte IPC stack proof")
            return 0
        except ContractError as exc:
            print(f"Error: ESP32-S3 linked framework contract failed: {exc}")
            return 1

    env.AddPostAction(  # noqa: F821
        "$BUILD_DIR/${PROGNAME}.elf",
        env.VerboseAction(verify_after_link, "Verifying linked ESP IPC stack contract"),  # noqa: F821
    )
    print(
        "[FrameworkContract] qualified "
        f"platform={evidence['platform_version']} arduino={evidence['arduino_version']} "
        f"libs={evidence['libs_version']} ipc_stack={evidence['ipc_stack_bytes']}"
    )


if __name__ == "__main__":
    raise SystemExit(cli())
elif "Import" in globals():
    configure_scons()
