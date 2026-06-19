#!/usr/bin/env python3
"""Ensure release artifacts come from the tested PlatformIO production env."""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLATFORMIO_INI = ROOT / "platformio.ini"
CI_TEST = ROOT / "scripts" / "ci-test.sh"
CI_YML = ROOT / ".github" / "workflows" / "ci.yml"
RELEASE_YML = ROOT / ".github" / "workflows" / "release.yml"
PARTITIONS = ROOT / "partitions_v1.csv"
ENV = "waveshare-349"


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
    with PARTITIONS.open(newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
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


def require_contains(text: str, needle: str, label: str, errors: list[str]) -> None:
    if needle not in text:
        errors.append(f"{label} missing required text: {needle!r}")


def check_production_env_is_tested(errors: list[str]) -> None:
    """Guard the invariant that release firmware is the firmware CI tests."""

    ci_test_text = CI_TEST.read_text(encoding="utf-8")
    ci_workflow_text = CI_YML.read_text(encoding="utf-8")
    release_text = RELEASE_YML.read_text(encoding="utf-8")

    require_contains(ci_test_text, f"PIO_BUILD_ARGS=(-e {ENV})", "scripts/ci-test.sh", errors)
    require_contains(ci_workflow_text, "./scripts/ci-test.sh", ".github/workflows/ci.yml", errors)
    require_contains(release_text, "./scripts/ci-test.sh", ".github/workflows/release.yml", errors)
    require_contains(
        release_text,
        f"pio run -e {ENV} -j 1",
        ".github/workflows/release.yml firmware build",
        errors,
    )
    require_contains(
        release_text,
        f"pio run -e {ENV} -t buildfs -j 1",
        ".github/workflows/release.yml filesystem build",
        errors,
    )

    ci_index = release_text.find("./scripts/ci-test.sh")
    firmware_index = release_text.find(f"pio run -e {ENV} -j 1")
    if ci_index == -1 or firmware_index == -1:
        return
    if ci_index > firmware_index:
        errors.append("release.yml must run scripts/ci-test.sh before building release firmware")


def main() -> int:
    errors: list[str] = []
    check_production_env_is_tested(errors)

    try:
        expected = {
            "--flash-mode": env_value("board_build.flash_mode"),
            "--flash-freq": expected_freq(),
            "--flash-size": env_value("board_build.flash_size"),
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
        app_partition_offset = partition_offset("app")
        storage_partition_offset = partition_offset("storage")
        upload_offset = int(env_value("board_upload.offset_address"), 0)
        firmware_offset = workflow_image_offset("firmware.bin")
        littlefs_offset = workflow_image_offset("littlefs.bin")
        if upload_offset != app_partition_offset:
            errors.append(
                "board_upload.offset_address must match app partition offset: "
                f"{upload_offset:#x} != {app_partition_offset:#x}"
            )
        if firmware_offset != app_partition_offset:
            errors.append(
                f"release firmware offset: expected {app_partition_offset:#x}, got {firmware_offset:#x}"
            )
        if littlefs_offset != storage_partition_offset:
            errors.append(
                f"release LittleFS offset: expected {storage_partition_offset:#x}, got {littlefs_offset:#x}"
            )
    except ValueError as exc:
        errors.append(str(exc))

    release_text = RELEASE_YML.read_text(encoding="utf-8")
    if "merge_bin" in release_text or "--flash_mode" in release_text:
        errors.append("release.yml must use current esptool merge-bin/--flash-* spelling")
    if "scripts/check_release_image_info.py" not in release_text:
        errors.append("release.yml must validate merged-firmware.bin with check_release_image_info.py")
    for required in (
        "web-installer/index.html",
        "scripts/check_web_installer_page.py --site-dir release/pages",
        "actions/configure-pages@v5",
        "enablement: true",
        "actions/upload-pages-artifact@v5",
        "actions/deploy-pages@v4",
    ):
        if required not in release_text:
            errors.append(f"release.yml missing web installer Pages deployment contract: {required}")

    if errors:
        print("[contract] release workflow production-env contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] release artifacts use the tested production env and flash policy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
