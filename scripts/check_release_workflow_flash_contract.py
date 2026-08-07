#!/usr/bin/env python3
"""Validate the release workflow's production build and flash artifacts."""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path

import stage_release_licenses as release_licenses


ROOT = Path(__file__).resolve().parents[1]
PLATFORMIO_INI = ROOT / "platformio.ini"
PRODUCTION_BUILD = ROOT / "scripts" / "build_production_artifacts.sh"
RELEASE_YML = ROOT / ".github" / "workflows" / "release.yml"
RELEASE_BUMP_POLICY = ROOT / ".release-bump"
PARTITIONS = ROOT / "partitions_v1.csv"
ENV = "waveshare-349"
VALID_RELEASE_BUMPS = {"patch", "minor", "major"}


def env_value(key: str) -> str:
    text = PLATFORMIO_INI.read_text(encoding="utf-8")
    match = re.search(
        rf"^\[env:{ENV}\]\s*$([\s\S]*?)(?=^\[|\Z)",
        text,
        re.MULTILINE,
    )
    if not match:
        raise ValueError(f"platformio.ini missing [env:{ENV}]")
    value = re.search(rf"^\s*{re.escape(key)}\s*=\s*(.+?)\s*$", match.group(1), re.MULTILINE)
    if not value:
        raise ValueError(f"[env:{ENV}] missing {key}")
    return value.group(1).strip()


def env_value_optional(key: str) -> str | None:
    text = PLATFORMIO_INI.read_text(encoding="utf-8")
    match = re.search(
        rf"^\[env:{ENV}\]\s*$([\s\S]*?)(?=^\[|\Z)",
        text,
        re.MULTILINE,
    )
    if not match:
        raise ValueError(f"platformio.ini missing [env:{ENV}]")
    value = re.search(rf"^\s*{re.escape(key)}\s*=\s*(.+?)\s*$", match.group(1), re.MULTILINE)
    return value.group(1).strip() if value else None


def workflow_flag(flag: str) -> str:
    text = RELEASE_YML.read_text(encoding="utf-8")
    match = re.search(rf"{re.escape(flag)}\s+([^\s\\]+)", text)
    if not match:
        raise ValueError(f"release.yml missing {flag}")
    return match.group(1).strip()


def workflow_image_offset(image_name: str) -> int:
    text = RELEASE_YML.read_text(encoding="utf-8")
    match = re.search(rf"(?i)\b(0x[0-9a-f]+|\d+)\s+release/{re.escape(image_name)}\b", text)
    if not match:
        raise ValueError(f"release.yml missing merge-bin offset for {image_name}")
    return int(match.group(1), 0)


def partition_offset(name: str) -> int:
    with PARTITIONS.open(newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row or row[0].strip().startswith("#"):
                continue
            if row[0].strip() == name:
                return int(row[3].strip(), 0)
    raise ValueError(f"partition table missing {name} partition")


def expected_freq() -> str:
    hz = int(env_value("board_build.f_flash").rstrip("Ll"))
    if hz % 1_000_000 != 0:
        raise ValueError(f"unsupported board_build.f_flash value: {hz}")
    return f"{hz // 1_000_000}m"


def expected_flash_mode() -> str:
    memory_type = (env_value_optional("board_build.arduino.memory_type") or "").lower()
    if memory_type in ("opi_opi", "opi_qspi"):
        return "dout"
    mode = env_value("board_build.flash_mode").lower()
    return "dio" if mode in ("qio", "qout") else mode


def expected_flash_size() -> str:
    return env_value_optional("board_upload.flash_size") or env_value("board_build.flash_size")


def require_contains(text: str, needle: str, label: str, errors: list[str]) -> None:
    if needle not in text:
        errors.append(f"{label} missing required text: {needle!r}")


def check_production_build(errors: list[str]) -> None:
    """Guard the shared production build and release packaging order."""

    production_text = PRODUCTION_BUILD.read_text(encoding="utf-8")
    release_text = RELEASE_YML.read_text(encoding="utf-8")
    dependency_command = f'"$PIO_CMD" pkg install -e {ENV}'

    for required in (
        f"PIO_BUILD_ARGS=(-e {ENV})",
        dependency_command,
        'run_step "Firmware clean" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t clean',
        'run_step "Firmware build" run_firmware_build_with_memory_log',
        'run_step "LittleFS image build" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t buildfs',
        "scripts/check_memory_headroom.py",
        "--warn-diram-zero",
        "scripts/report_flash_package_size.py",
        "scripts/check_littlefs_image_compatibility.py",
        "npm run build",
        "npm run deploy:built",
    ):
        require_contains(production_text, required, "scripts/build_production_artifacts.sh", errors)

    dependency_index = production_text.find(dependency_command)
    clean_index = production_text.find(
        'run_step "Firmware clean" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t clean'
    )
    if -1 not in (dependency_index, clean_index) and dependency_index > clean_index:
        errors.append(
            "scripts/build_production_artifacts.sh must install PlatformIO project "
            "dependencies before the first firmware command"
        )

    checker = "python3 scripts/check_release_workflow_flash_contract.py"
    build = "./scripts/build_production_artifacts.sh"
    merge = "Merge firmware for ESP Web Tools"
    for required in (checker, build, merge):
        require_contains(release_text, required, ".github/workflows/release.yml", errors)
    ordered = (release_text.find(checker), release_text.find(build), release_text.find(merge))
    if -1 not in ordered and list(ordered) != sorted(ordered):
        errors.append("release.yml must validate its artifact contract, build, then package firmware")


def check_version_policy(errors: list[str]) -> None:
    """Guard the reviewed one-release bump and its automatic patch reset."""

    try:
        bump = RELEASE_BUMP_POLICY.read_text(encoding="utf-8").strip()
    except OSError as exc:
        errors.append(f"unable to read .release-bump: {exc}")
        return
    if bump not in VALID_RELEASE_BUMPS:
        allowed = ", ".join(sorted(VALID_RELEASE_BUMPS))
        errors.append(f".release-bump must contain exactly one of: {allowed}")

    release_text = RELEASE_YML.read_text(encoding="utf-8")
    for required in (
        "RELEASE_BUMP: auto",
        "git add include/config.h CHANGELOG.md .release-bump",
        'if [ "$(cat .release-bump)" != "patch" ]; then',
        'EXPECTED_FILES="$(printf \'%s\\n\' .release-bump CHANGELOG.md include/config.h)"',
    ):
        require_contains(release_text, required, ".github/workflows/release.yml version policy", errors)


def check_flash_and_package(errors: list[str]) -> None:
    try:
        expected = {
            "--flash-mode": expected_flash_mode(),
            "--flash-freq": expected_freq(),
            "--flash-size": expected_flash_size(),
        }
        actual = {flag: workflow_flag(flag) for flag in expected}
    except ValueError as exc:
        errors.append(str(exc))
        expected = {}
        actual = {}

    for flag, expected_value in expected.items():
        actual_value = actual.get(flag, "")
        if actual_value.upper() != expected_value.upper():
            errors.append(f"{flag}: expected {expected_value}, got {actual_value}")

    try:
        app_offset = partition_offset("app")
        storage_offset = partition_offset("storage")
        upload_offset = int(env_value("board_upload.offset_address"), 0)
        firmware_offset = workflow_image_offset("firmware.bin")
        littlefs_offset = workflow_image_offset("littlefs.bin")
        if upload_offset != app_offset:
            errors.append(
                "board_upload.offset_address must match app partition offset: "
                f"{upload_offset:#x} != {app_offset:#x}"
            )
        if firmware_offset != app_offset:
            errors.append(f"release firmware offset: expected {app_offset:#x}, got {firmware_offset:#x}")
        if littlefs_offset != storage_offset:
            errors.append(
                f"release LittleFS offset: expected {storage_offset:#x}, got {littlefs_offset:#x}"
            )
    except ValueError as exc:
        errors.append(str(exc))

    release_text = RELEASE_YML.read_text(encoding="utf-8")
    errors.extend(release_licenses.source_errors(ROOT))
    if "merge_bin" in release_text or "--flash_mode" in release_text:
        errors.append("release.yml must use current esptool merge-bin/--flash-* spelling")
    require_contains(
        release_text,
        "scripts/check_release_image_info.py",
        ".github/workflows/release.yml merged-image validation",
        errors,
    )

    for required in (
        "web-installer/index.html",
        "python3 scripts/stage_release_licenses.py",
        "--release-dir release",
        "--pages-dir release/pages",
        "\n            release/LICENSE",
        "\n            release/THIRD_PARTY_NOTICES.md",
        "\n            release/ArduinoJson-LICENSE.txt",
        "\n            release/NimBLE-Arduino-LICENSE.txt",
        "\n            release/NimBLE-Arduino-NOTICE.txt",
        "\n            release/Arduino-GFX-LICENSE.txt",
        "\n            release/OpenFontRender-LICENSE.txt",
        "\n            release/FreeType-FTL.txt",
        "\n            release/GNU-FreeFont-COPYING.txt",
        "\n            release/GNU-FreeFont-README.txt",
        "\n            release/OFL-1.1.txt",
        "\n            release/CC-BY-4.0.txt",
        "\n            release/Svelte-LICENSE.md",
        "\n            release/SvelteKit-LICENSE.txt",
        "\n            release/daisyUI-LICENSE.txt",
        "\n            release/Tailwind-CSS-LICENSE.txt",
        "scripts/check_web_installer_page.py --site-dir release/pages",
        "uses: actions/configure-pages@",
        "enablement: true",
        "uses: actions/upload-pages-artifact@",
        "uses: actions/deploy-pages@",
    ):
        require_contains(release_text, required, ".github/workflows/release.yml package", errors)


def main() -> int:
    errors: list[str] = []
    check_production_build(errors)
    check_version_policy(errors)
    check_flash_and_package(errors)
    if errors:
        print("[release] production artifact contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("[release] production build, flash layout, installer, and licenses validated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
