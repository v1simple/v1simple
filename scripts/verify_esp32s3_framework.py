#!/usr/bin/env python3
"""Fail closed unless the qualified ESP32-S3 framework and linked IPC stack match."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
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
    "webserver_parsing_sha256": "c43c6827b6ccdf198d6150bddf0b627683e4bdd6d51e957e03df5e1b7ad0db52",
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
    webserver_parsing = arduino_dir / "libraries" / "WebServer" / "src" / "Parsing.cpp"

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
        "webserver_parsing_sha256": require_hash(
            webserver_parsing, str(EXPECTED["webserver_parsing_sha256"]), "hardened WebServer body parser"
        ),
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


def run_tool(command: list[str], label: str) -> str:
    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise ContractError(f"{label} failed (exit {result.returncode})")
    return result.stdout


def symbol_record(symbols: str, name: str) -> tuple[int, str, int]:
    match = re.search(
        rf"^([0-9a-fA-F]+)\s+\S+\s+F\s+(\S+)\s+([0-9a-fA-F]+)\s+{re.escape(name)}$",
        symbols,
        re.MULTILINE,
    )
    if match is None:
        raise ContractError(f"linked ELF does not expose {name}")
    return int(match.group(1), 16), match.group(2), int(match.group(3), 16)


def object_symbol_record(symbols: str, name: str) -> tuple[int, str, int]:
    match = re.search(
        rf"^([0-9a-fA-F]+)\s+\S+\s+O\s+(\S+)\s+([0-9a-fA-F]+)\s+{re.escape(name)}$",
        symbols,
        re.MULTILINE,
    )
    if match is None:
        raise ContractError(f"linked ELF does not expose {name}")
    return int(match.group(1), 16), match.group(2), int(match.group(3), 16)


def section_vma(headers: str, name: str) -> int:
    match = re.search(
        rf"^\s*\d+\s+{re.escape(name)}\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+",
        headers,
        re.MULTILINE,
    )
    if match is None:
        raise ContractError(f"linked ELF does not expose section {name}")
    return int(match.group(1), 16)


def instruction_texts(disassembly: str) -> list[str]:
    instructions: list[str] = []
    for line in disassembly.splitlines():
        match = re.match(r"^\s*[0-9a-fA-F]+:\s+[0-9a-fA-F]+\s+(.+?\S)\s*$", line)
        if match is not None:
            instructions.append(match.group(1))
    return instructions


def verify_ipc_call_sequence(disassembly: str, task_address: int) -> None:
    instructions = instruction_texts(disassembly)
    task_target = re.compile(
        rf"^l32r\s+a8,\s*.*\((?:0x)?0*{task_address:x}(?:\s+<xTaskCreatePinnedToCore>)?\)$"
    )
    for index in range(len(instructions) - 3):
        if re.fullmatch(r"slli\s+a12,\s*a12,\s*11", instructions[index]) is None:
            continue
        if re.fullmatch(r"mov(?:\.n)?\s+a11,\s*a3", instructions[index + 1]) is None:
            continue
        if task_target.fullmatch(instructions[index + 2]) is None:
            continue
        if re.fullmatch(r"callx8\s+a8", instructions[index + 3]) is not None:
            return
    raise ContractError(
        "linked esp_ipc_init does not materialize a 2048-byte stack for the exact xTaskCreatePinnedToCore call"
    )


def verify_raw_linked_ipc(
    elf: Path,
    objdump: Path,
    ipc_address: int,
    ipc_section: str,
    ipc_size: int,
    task_address: int,
) -> None:
    """Verify linked bytes when Xtensa property metadata labels code as data.

    GNU objdump consults the linked ``.xt.prop`` instruction-boundary table.
    When that table labels a linked range as data, objdump prints a real
    function as raw words even though its symbol and executable section are
    intact. Dumping only that
    executable section and disassembling its exact symbol range as raw Xtensa
    bytes avoids accepting a package/archive-only proof while retaining the
    linked call-target and stack-argument checks.
    """

    if ipc_size == 0:
        raise ContractError("linked esp_ipc_init has an empty symbol range")

    headers = run_tool([str(objdump), "-h", str(elf)], "objdump section inspection")
    vma = section_vma(headers, ipc_section)
    objcopy = objdump.with_name(objdump.name.replace("objdump", "objcopy"))
    if not objcopy.is_file():
        raise ContractError("Xtensa objcopy is missing for linked IPC byte verification")

    with tempfile.TemporaryDirectory(prefix="v1simple-ipc-proof-") as raw:
        section_bytes = Path(raw) / "section.bin"
        run_tool(
            [str(objcopy), "--dump-section", f"{ipc_section}={section_bytes}", str(elf)],
            "objcopy linked section extraction",
        )
        disassembly = run_tool(
            [
                str(objdump),
                "-D",
                "-b",
                "binary",
                "-m",
                "xtensa",
                f"--adjust-vma=0x{vma:x}",
                f"--start-address=0x{ipc_address:x}",
                f"--stop-address=0x{ipc_address + ipc_size:x}",
                str(section_bytes),
            ],
            "objdump raw linked IPC inspection",
        )

    verify_ipc_call_sequence(disassembly, task_address)


def verify_linked_elf(elf: Path, objdump: Path) -> dict[str, object]:
    if not elf.is_file() or not objdump.is_file():
        raise ContractError("linked ELF or Xtensa objdump is missing")
    symbols = run_tool([str(objdump), "-t", str(elf)], "objdump symbol inspection")
    ipc_address, ipc_section, ipc_size = symbol_record(symbols, "esp_ipc_init")
    task_address, _, _ = symbol_record(symbols, "xTaskCreatePinnedToCore")
    body_address, body_section, body_size = object_symbol_record(
        symbols, "v1simple_webserver_exact_body_contract"
    )
    if body_size != 4:
        raise ContractError("linked WebServer body-ingress contract marker has an unexpected size")
    headers = run_tool([str(objdump), "-h", str(elf)], "objdump section inspection")
    body_section_vma = section_vma(headers, body_section)
    objcopy = objdump.with_name(objdump.name.replace("objdump", "objcopy"))
    if not objcopy.is_file():
        raise ContractError("Xtensa objcopy is missing for linked WebServer marker verification")
    with tempfile.TemporaryDirectory(prefix="v1simple-webserver-proof-") as raw:
        section_bytes = Path(raw) / "section.bin"
        run_tool(
            [str(objcopy), "--dump-section", f"{body_section}={section_bytes}", str(elf)],
            "objcopy linked WebServer marker extraction",
        )
        marker = section_bytes.read_bytes()[body_address - body_section_vma : body_address - body_section_vma + 4]
    if marker != (0x56314232).to_bytes(4, "little"):
        raise ContractError("linked WebServer body-ingress contract marker value is incorrect")
    disassembly = run_tool(
        [str(objdump), "-d", "--disassemble=esp_ipc_init", str(elf)],
        "objdump linked esp_ipc_init inspection",
    )
    # Xtensa passes the xTaskCreatePinnedToCore stack-depth argument in a12.
    # The qualified framework materializes 2048 as 1 << 11 immediately before
    # that call. A stock 1024-byte library uses a shift of 10 and fails here.
    decoded = [item for item in instruction_texts(disassembly) if not item.startswith((".byte", ".word"))]
    if decoded:
        verify_ipc_call_sequence(disassembly, task_address)
    else:
        verify_raw_linked_ipc(elf, objdump, ipc_address, ipc_section, ipc_size, task_address)
    return {
        "elf_sha256": sha256_file(elf),
        "elf_ipc_symbol": "esp_ipc_init",
        "elf_ipc_stack_argument": "a12=1<<11",
        "elf_ipc_stack_bytes": EXPECTED["ipc_stack_bytes"],
        "elf_webserver_body_symbol": "v1simple_webserver_exact_body_contract",
        "elf_webserver_body_contract": "0x56314232",
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
            print(
                "[FrameworkContract] linked ELF passes 2048-byte IPC stack and "
                "exact WebServer body-ingress marker proofs"
            )
            return 0
        except ContractError as exc:
            print(f"Error: ESP32-S3 linked framework contract failed: {exc}")
            return 1

    env.AddPostAction(  # noqa: F821
        "$BUILD_DIR/${PROGNAME}.elf",
        env.VerboseAction(  # noqa: F821
            verify_after_link,
            "Verifying linked ESP IPC stack and exact WebServer body-ingress contracts",
        ),  # noqa: F821
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
