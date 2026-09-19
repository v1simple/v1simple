#!/usr/bin/env python3
"""Fail before the gate unless the declared Linux validation contract is present."""

from __future__ import annotations

import argparse
import os
import platform
import re
import subprocess
import sys
from collections.abc import Mapping
from dataclasses import asdict, dataclass
from pathlib import Path

import check_esptool_version
import check_platformio_core_version


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_NODE = "22.23.2"
EXPECTED_NPM = "10.9.8"
EXPECTED_PYTHON = "3.12"
EXPECTED_PIO = "6.1.19"
EXPECTED_ESPTOOL = "5.3.0"
EXPECTED_RUFF = "0.16.0"
EXPECTED_SHELLCHECK = "0.11.0"
EXPECTED_SWIFT = "6.3.3"


@dataclass(frozen=True)
class EnvironmentSnapshot:
    profile: str
    source_commit: str
    source_clean: bool
    image_identity: str
    os_id: str
    os_version: str
    architecture: str
    python_version: str
    node_version: str
    npm_version: str
    pioarduino_version: str
    esptool_version: str
    ruff_version: str
    shellcheck_version: str
    swift_version: str
    ffmpeg_version: str
    libpcre3_available: bool


def command_output(args: list[str]) -> str:
    try:
        completed = subprocess.run(
            args,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except OSError:
        return ""
    return completed.stdout.strip() if completed.returncode == 0 else ""


def first_version(raw: str) -> str:
    match = re.search(r"(?<![0-9A-Za-z])(\d+\.\d+(?:\.\d+)?)(?![0-9A-Za-z])", raw)
    return match.group(1) if match else ""


def os_release(path: Path = Path("/etc/os-release")) -> tuple[str, str]:
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return "", ""
    for line in lines:
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value.strip().strip('"')
    return values.get("ID", ""), values.get("VERSION_ID", "")


def tracked_source_is_clean(root: Path) -> bool:
    result = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "diff",
            "--quiet",
            "--ignore-submodules=untracked",
            "HEAD",
            "--",
        ],
        check=False,
    )
    return result.returncode == 0


def image_identity(environment: Mapping[str, str] = os.environ) -> str:
    explicit = environment.get("V1_CLEAN_LINUX_IMAGE", "")
    if explicit:
        return explicit
    image_os = environment.get("ImageOS", "")
    image_version = environment.get("ImageVersion", "")
    if image_os or image_version:
        return ":".join(value for value in (image_os, image_version) if value)
    return "github-hosted-unidentified"


def collect_snapshot(profile_name: str, root: Path) -> EnvironmentSnapshot:
    os_id, os_version = os_release()
    pio_raw = command_output(["pio", "--version"])
    pio_version = first_version(pio_raw)
    distribution = check_platformio_core_version.installed_pioarduino_version("pio")
    if distribution is None:
        pio_version = ""
    else:
        pio_version = check_platformio_core_version.format_version(distribution)

    esptool_raw = command_output([sys.executable, "-m", "esptool", "version"])
    esptool = check_esptool_version.parse_version(esptool_raw)
    ldconfig = command_output(["/sbin/ldconfig", "-p"])
    return EnvironmentSnapshot(
        profile=profile_name,
        source_commit=command_output(["git", "-C", str(root), "rev-parse", "HEAD"]),
        source_clean=tracked_source_is_clean(root),
        image_identity=image_identity(),
        os_id=os_id,
        os_version=os_version,
        architecture=platform.machine(),
        python_version=f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}",
        node_version=command_output(["node", "--version"]).removeprefix("v"),
        npm_version=command_output(["npm", "--version"]),
        pioarduino_version=pio_version,
        esptool_version=(
            check_esptool_version.format_version(esptool) if esptool is not None else ""
        ),
        ruff_version=first_version(command_output(["ruff", "--version"])),
        shellcheck_version=first_version(command_output(["shellcheck", "--version"])),
        swift_version=first_version(command_output(["swiftc", "--version"])),
        ffmpeg_version=first_version(command_output(["ffmpeg", "-version"])),
        libpcre3_available="libpcre.so.3" in ldconfig,
    )


def validate_snapshot(snapshot: EnvironmentSnapshot) -> list[str]:
    errors: list[str] = []
    expected = {
        "os_id": "ubuntu",
        "os_version": "24.04",
        "node_version": EXPECTED_NODE,
        "npm_version": EXPECTED_NPM,
        "pioarduino_version": EXPECTED_PIO,
        "esptool_version": EXPECTED_ESPTOOL,
    }
    for field, value in expected.items():
        actual = getattr(snapshot, field)
        if actual != value:
            errors.append(f"{field} is {actual or 'missing'}; required {value}")
    if not snapshot.python_version.startswith(f"{EXPECTED_PYTHON}."):
        errors.append(
            f"python_version is {snapshot.python_version or 'missing'}; "
            f"required {EXPECTED_PYTHON}.x"
        )
    if not re.fullmatch(r"[0-9a-f]{40}", snapshot.source_commit):
        errors.append("source_commit is missing or not a full Git commit")
    if not snapshot.source_clean:
        errors.append("tracked source does not match the recorded commit")

    if snapshot.profile == "ci":
        ci_expected = {
            "ruff_version": EXPECTED_RUFF,
            "shellcheck_version": EXPECTED_SHELLCHECK,
            "swift_version": EXPECTED_SWIFT,
        }
        for field, value in ci_expected.items():
            actual = getattr(snapshot, field)
            if actual != value:
                errors.append(f"{field} is {actual or 'missing'}; required {value}")
        if not snapshot.ffmpeg_version:
            errors.append("ffmpeg is missing")
        if not snapshot.libpcre3_available:
            errors.append("libpcre3 runtime is missing for the pinned cppcheck binary")
    elif snapshot.profile != "release":
        errors.append(f"unsupported validation profile {snapshot.profile!r}")
    return errors


def write_manifest(path: Path, snapshot: EnvironmentSnapshot, errors: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    values = asdict(snapshot)
    with path.open("w", encoding="utf-8") as output:
        output.write(f"validation_status={'FAIL' if errors else 'PASS'}\n")
        for key, value in values.items():
            rendered = str(value).lower() if isinstance(value, bool) else str(value)
            output.write(f"{key}={rendered}\n")
        for index, error in enumerate(errors, start=1):
            output.write(f"error_{index}={error}\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=("ci", "release"))
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=ROOT / ".artifacts" / "linux-validation-environment.txt",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    snapshot = collect_snapshot(args.profile, args.root.resolve())
    errors = validate_snapshot(snapshot)
    write_manifest(args.manifest, snapshot, errors)
    if errors:
        print("[linux-validation] environment contract failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(
        f"[linux-validation] {args.profile} environment matches {snapshot.source_commit}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
