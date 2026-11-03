#!/usr/bin/env python3
"""Validate an ESP Web Tools merged image matches PlatformIO flash policy."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from build_context import FlashPolicy, resolve_flash_policy


ROOT = Path(__file__).resolve().parents[1]
IMAGE_HEADER_OFFSETS = {
    "bootloader": 0x0000,
    "app": 0x20000,
}
FLASH_MODE_CODES = {
    "QIO": 0x00,
    "QOUT": 0x01,
    "DIO": 0x02,
    "DOUT": 0x03,
}
FLASH_SIZE_CODES = {
    "1MB": 0x00,
    "2MB": 0x10,
    "4MB": 0x20,
    "8MB": 0x30,
    "16MB": 0x40,
    "32MB": 0x50,
    "64MB": 0x60,
    "128MB": 0x70,
}
FLASH_FREQ_CODES = {
    "40M": 0x00,
    "26M": 0x01,
    "20M": 0x02,
    "80M": 0x0F,
}


def run_image_info(image: Path) -> str:
    esptool = shutil.which("esptool")
    if esptool:
        cmd = [esptool, "image-info", str(image)]
    else:
        esptool_py = shutil.which("esptool.py")
        if not esptool_py:
            raise FileNotFoundError("esptool/esptool.py not found in PATH")
        cmd = [esptool_py, "image_info", str(image)]

    result = subprocess.run(
        cmd,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"{' '.join(cmd)} failed with exit code {result.returncode}\n{result.stdout}"
        )
    return result.stdout


def parse_image_field(output: str, label: str) -> str:
    match = re.search(rf"^\s*{re.escape(label)}:\s*([^\s]+)\s*$", output, re.MULTILINE)
    if not match:
        raise ValueError(f"image_info output missing '{label}'")
    return match.group(1).strip()


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def expected_flash_policy(env: str, pio: str | None = None) -> dict[str, str]:
    return resolve_flash_policy(ROOT, env, pio or os.environ.get("PIO_CMD", "pio")).image_fields()


def expected_header_bytes(expected: dict[str, str]) -> tuple[int, int]:
    mode = expected["Flash mode"].upper()
    size = expected["Flash size"].upper()
    freq = expected["Flash freq"].upper()
    try:
        return FLASH_MODE_CODES[mode], FLASH_SIZE_CODES[size] | FLASH_FREQ_CODES[freq]
    except KeyError as exc:
        raise ValueError(f"unsupported image header field value: {exc.args[0]}") from exc


def requires_full_layout(image: Path, image_size: int) -> bool:
    return image.name == "merged-firmware.bin" or image_size >= 0xDA0000


def check_embedded_image_headers(image: Path, expected: dict[str, str]) -> list[str]:
    data = image.read_bytes()
    expected_mode, expected_size_freq = expected_header_bytes(expected)
    required_offsets = IMAGE_HEADER_OFFSETS
    if not requires_full_layout(image, len(data)):
        required_offsets = {"image": 0x0000}

    errors: list[str] = []
    for label, offset in required_offsets.items():
        if len(data) <= offset + 3:
            errors.append(f"{label} image header missing at offset {offset:#x}")
            continue
        if data[offset] != 0xE9:
            errors.append(f"{label} image header at {offset:#x}: expected magic 0xe9, got {data[offset]:#x}")
            continue
        actual_mode = data[offset + 2]
        actual_size_freq = data[offset + 3]
        if actual_mode != expected_mode:
            errors.append(
                f"{label} image header at {offset:#x}: "
                f"expected flash mode byte {expected_mode:#04x}, got {actual_mode:#04x}"
            )
        if actual_size_freq != expected_size_freq:
            errors.append(
                f"{label} image header at {offset:#x}: "
                f"expected size/freq byte {expected_size_freq:#04x}, got {actual_size_freq:#04x}"
            )
    return errors


def self_check(env: str | None = None) -> int:
    env = env or os.environ.get("PIO_ENV", "waveshare-349")
    try:
        expected = expected_flash_policy(env)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"[release-image] self-check failed: {exc}", file=sys.stderr)
        return 2

    print(
        "[release-image] self-check OK: "
        f"{env} expects mode={expected['Flash mode']} "
        f"freq={expected['Flash freq']} size={expected['Flash size']}"
    )
    return 0


def main() -> int:
    if len(sys.argv) == 1:
        return self_check()

    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--env", default=os.environ.get("PIO_ENV", "waveshare-349"))
    parser.add_argument("--pio", default=os.environ.get("PIO_CMD", "pio"))
    parser.add_argument("--flash-mode")
    parser.add_argument("--flash-freq")
    parser.add_argument("--flash-size")
    args = parser.parse_args()

    image = args.image
    if not image.is_absolute():
        image = ROOT / image
    if not image.is_file():
        print(f"[release-image] missing image: {image}", file=sys.stderr)
        return 1

    explicit = (args.flash_mode, args.flash_freq, args.flash_size)
    if any(explicit) and not all(explicit):
        parser.error("standalone image checking needs all three explicit flash policy fields")
    expected = (FlashPolicy(*explicit).image_fields() if all(explicit)
                else expected_flash_policy(args.env, args.pio))

    try:
        output = run_image_info(image)
        actual = {label: parse_image_field(output, label) for label in expected}
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"[release-image] {exc}", file=sys.stderr)
        return 1

    errors: list[str] = []
    for label, expected_value in expected.items():
        actual_value = actual[label]
        if actual_value.upper() != expected_value.upper():
            errors.append(f"{label}: expected {expected_value}, got {actual_value}")
    errors.extend(check_embedded_image_headers(image, expected))

    report_path = image.with_name(f"{image.stem}-image-info.txt")
    report_path.write_text(output, encoding="utf-8")

    if errors:
        print("[release-image] flash policy mismatch:")
        for error in errors:
            print(f"  - {error}")
        print(f"[release-image] image_info saved to {display_path(report_path)}")
        return 1

    print(
        "[release-image] flash policy matches "
        f"{args.env}: mode={actual['Flash mode']} freq={actual['Flash freq']} "
        f"size={actual['Flash size']}"
    )
    print(f"[release-image] image_info saved to {display_path(report_path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
