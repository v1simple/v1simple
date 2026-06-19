#!/usr/bin/env python3
"""Validate an ESP Web Tools merged image matches PlatformIO flash policy."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLATFORMIO_INI = ROOT / "platformio.ini"


def read_env_block(env: str) -> str:
    text = PLATFORMIO_INI.read_text(encoding="utf-8")
    match = re.search(
        rf"^\[env:{re.escape(env)}\]\s*$([\s\S]*?)(?=^\[|\Z)",
        text,
        re.MULTILINE,
    )
    if not match:
        raise ValueError(f"platformio.ini missing [env:{env}]")
    return match.group(1)


def read_env_value(env: str, key: str) -> str:
    block = read_env_block(env)
    match = re.search(rf"^\s*{re.escape(key)}\s*=\s*(.+?)\s*$", block, re.MULTILINE)
    if not match:
        raise ValueError(f"[env:{env}] missing {key}")
    return match.group(1).strip()


def expected_flash_freq(env: str) -> str:
    raw = read_env_value(env, "board_build.f_flash").rstrip("Ll")
    hz = int(raw)
    if hz % 1_000_000 != 0:
        raise ValueError(f"unsupported board_build.f_flash value: {raw}")
    return f"{hz // 1_000_000}m"


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


def expected_flash_policy(env: str) -> dict[str, str]:
    return {
        "Flash mode": read_env_value(env, "board_build.flash_mode").upper(),
        "Flash freq": expected_flash_freq(env),
        "Flash size": read_env_value(env, "board_build.flash_size").upper(),
    }


def self_check(env: str = "waveshare-349") -> int:
    try:
        expected = expected_flash_policy(env)
    except (OSError, ValueError) as exc:
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
    parser.add_argument("--env", default="waveshare-349")
    args = parser.parse_args()

    image = args.image
    if not image.is_absolute():
        image = ROOT / image
    if not image.is_file():
        print(f"[release-image] missing image: {image}", file=sys.stderr)
        return 1

    expected = expected_flash_policy(args.env)

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
