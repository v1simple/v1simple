#!/usr/bin/env python3
"""Build the validated release bundle from production build outputs."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

import check_release_image_info as image_info
import stage_release_licenses as licenses
import write_release_manifests as manifests


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = ROOT / ".pio" / "build" / "waveshare-349"
DEFAULT_RELEASE_DIR = ROOT / "release"
RELEASE_IMAGES = (
    manifests.BOOTLOADER_IMAGE,
    manifests.PARTITION_TABLE_IMAGE,
    manifests.APP_IMAGE,
    "littlefs.bin",
)


def run_checked(command: list[str]) -> None:
    completed = subprocess.run(command, cwd=ROOT, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"release command failed with exit {completed.returncode}: {command[0]}"
        )


def require_empty_directory(path: Path) -> None:
    if path.exists() and any(path.iterdir()):
        raise ValueError(f"release output directory is not empty: {path}")
    path.mkdir(parents=True, exist_ok=True)


def copy_build_outputs(build_dir: Path, release_dir: Path) -> None:
    for name in RELEASE_IMAGES:
        source = build_dir / name
        if not source.is_file() or source.stat().st_size == 0:
            raise ValueError(f"production build output is missing or empty: {source}")
        shutil.copyfile(source, release_dir / name)
    shutil.copyfile(
        release_dir / manifests.APP_IMAGE,
        release_dir / manifests.UPDATE_IMAGE,
    )


def merge_fresh_image(
    release_dir: Path,
    *,
    environment: str,
    partitions_path: Path,
) -> None:
    partitions = manifests.read_partitions(partitions_path)
    esptool = shutil.which("esptool")
    if esptool is None:
        raise ValueError("esptool is not available on PATH")
    command = [
        esptool,
        "--chip",
        "esp32s3",
        "merge-bin",
        "-o",
        str(release_dir / manifests.FRESH_IMAGE),
        "--flash-mode",
        image_info.expected_flash_mode(environment),
        "--flash-freq",
        image_info.expected_flash_freq(environment),
        "--flash-size",
        image_info.expected_flash_size(environment),
        "0x0",
        str(release_dir / manifests.BOOTLOADER_IMAGE),
        hex(manifests.PARTITION_TABLE_OFFSET),
        str(release_dir / manifests.PARTITION_TABLE_IMAGE),
        hex(partitions["app"].offset),
        str(release_dir / manifests.APP_IMAGE),
        hex(partitions["storage"].offset),
        str(release_dir / "littlefs.bin"),
    ]
    run_checked(command)


def stage_installer(release_dir: Path) -> Path:
    pages_dir = release_dir / "pages"
    pages_dir.mkdir()

    template = (ROOT / "web-installer" / "index.html").read_text(encoding="utf-8")
    source_logo = "../interface/static/branding/v1simple-logo-transparent.png"
    if template.count(source_logo) != 1:
        raise ValueError("installer template must contain exactly one source logo path")
    (pages_dir / "index.html").write_text(
        template.replace(source_logo, "v1simple-logo-transparent.png"),
        encoding="utf-8",
    )
    shutil.copyfile(
        ROOT / "interface" / "static" / "branding" / "v1simple-logo-transparent.png",
        pages_dir / "v1simple-logo-transparent.png",
    )
    for name in (
        manifests.FRESH_MANIFEST,
        manifests.UPDATE_MANIFEST,
        manifests.FRESH_IMAGE,
        manifests.UPDATE_IMAGE,
    ):
        shutil.copyfile(release_dir / name, pages_dir / name)
    licenses.stage(ROOT, release_dir, pages_dir)
    return pages_dir


def package_release(
    *,
    build_dir: Path,
    release_dir: Path,
    version: str,
    environment: str,
    partitions_path: Path,
) -> None:
    require_empty_directory(release_dir)
    copy_build_outputs(build_dir, release_dir)
    merge_fresh_image(
        release_dir,
        environment=environment,
        partitions_path=partitions_path,
    )
    run_checked(
        [
            sys.executable,
            "scripts/check_release_image_info.py",
            "--image",
            str(release_dir / manifests.FRESH_IMAGE),
            "--env",
            environment,
        ]
    )
    manifests.write_manifests(release_dir, version, partitions_path)
    pages_dir = stage_installer(release_dir)
    run_checked(
        [
            sys.executable,
            "scripts/check_web_installer_page.py",
            "--site-dir",
            str(pages_dir),
            "--expected-version",
            version,
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--release-dir", type=Path, default=DEFAULT_RELEASE_DIR)
    parser.add_argument("--env", default="waveshare-349")
    parser.add_argument("--partitions", type=Path, default=manifests.DEFAULT_PARTITIONS)
    args = parser.parse_args()

    try:
        package_release(
            build_dir=args.build_dir.resolve(),
            release_dir=args.release_dir.resolve(),
            version=args.version,
            environment=args.env,
            partitions_path=args.partitions.resolve(),
        )
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"[release-package] {exc}", file=sys.stderr)
        return 1

    print(f"[release-package] validated release and installer bundles for {args.version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
