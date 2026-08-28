#!/usr/bin/env python3
"""Write destructive-fresh and app-only preservation manifests for one release."""

from __future__ import annotations

import argparse
import csv
import json
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PARTITIONS = ROOT / "partitions_v1.csv"
FRESH_MANIFEST = "manifest.json"
UPDATE_MANIFEST = "manifest-update.json"
FRESH_IMAGE = "merged-firmware.bin"
UPDATE_IMAGE = "firmware-update.bin"
APP_IMAGE = "firmware.bin"
BOOTLOADER_IMAGE = "bootloader.bin"
PARTITION_TABLE_IMAGE = "partitions.bin"
PARTITION_TABLE_OFFSET = 0x8000


@dataclass(frozen=True)
class Partition:
    name: str
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size


def read_partitions(path: Path) -> dict[str, Partition]:
    partitions: dict[str, Partition] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row or row[0].strip().startswith("#"):
                continue
            if len(row) < 5:
                raise ValueError(f"malformed partition row in {path}: {row!r}")
            name = row[0].strip()
            partitions[name] = Partition(
                name=name,
                offset=int(row[3].strip(), 0),
                size=int(row[4].strip(), 0),
            )
    for required in ("nvs", "app", "storage"):
        if required not in partitions:
            raise ValueError(f"partition table missing {required} partition")
    return partitions


def overlaps(start: int, end: int, partition: Partition) -> bool:
    return start < partition.end and partition.offset < end


def require_embedded_image(merged: Path, component: Path, offset: int) -> None:
    expected = component.read_bytes()
    with merged.open("rb") as handle:
        handle.seek(offset)
        actual = handle.read(len(expected))
    if actual != expected:
        raise ValueError(
            f"{FRESH_IMAGE} does not contain {component.name} at {offset:#x}"
        )


def validate_release_images(release_dir: Path, partitions: dict[str, Partition]) -> None:
    fresh_image = release_dir / FRESH_IMAGE
    update_image = release_dir / UPDATE_IMAGE
    app_image = release_dir / APP_IMAGE
    bootloader_image = release_dir / BOOTLOADER_IMAGE
    partition_table_image = release_dir / PARTITION_TABLE_IMAGE
    littlefs_image = release_dir / "littlefs.bin"
    for image in (
        fresh_image,
        update_image,
        app_image,
        bootloader_image,
        partition_table_image,
        littlefs_image,
    ):
        if not image.is_file() or image.stat().st_size == 0:
            raise ValueError(f"release image missing or empty: {image}")

    app = partitions["app"]
    update_start = app.offset
    update_end = update_start + update_image.stat().st_size
    if update_end > app.end:
        raise ValueError(
            f"{UPDATE_IMAGE} exceeds app partition: {update_end:#x} > {app.end:#x}"
        )
    for name, partition in partitions.items():
        if name != "app" and overlaps(update_start, update_end, partition):
            raise ValueError(f"{UPDATE_IMAGE} overlaps {name} partition")
    first_partition_offset = min(partition.offset for partition in partitions.values())
    reserved_regions = (
        Partition("bootloader", 0, PARTITION_TABLE_OFFSET),
        Partition(
            "partition table",
            PARTITION_TABLE_OFFSET,
            first_partition_offset - PARTITION_TABLE_OFFSET,
        ),
    )
    for reserved in reserved_regions:
        if reserved.size <= 0:
            raise ValueError("partition table leaves no valid bootloader/metadata region")
        if overlaps(update_start, update_end, reserved):
            raise ValueError(f"{UPDATE_IMAGE} overlaps {reserved.name}")
    if update_image.read_bytes() != app_image.read_bytes():
        raise ValueError(f"{UPDATE_IMAGE} is not the exact production {APP_IMAGE}")

    storage = partitions["storage"]
    required_fresh_end = storage.offset + littlefs_image.stat().st_size
    if fresh_image.stat().st_size < required_fresh_end:
        raise ValueError(
            f"{FRESH_IMAGE} does not contain the complete LittleFS image: "
            f"{fresh_image.stat().st_size:#x} < {required_fresh_end:#x}"
        )

    for component, offset in (
        (bootloader_image, 0),
        (partition_table_image, PARTITION_TABLE_OFFSET),
        (app_image, app.offset),
        (littlefs_image, storage.offset),
    ):
        require_embedded_image(fresh_image, component, offset)

    nvs = partitions["nvs"]
    with fresh_image.open("rb") as handle:
        handle.seek(nvs.offset)
        fresh_nvs = handle.read(nvs.size)
    if len(fresh_nvs) != nvs.size or any(byte != 0xFF for byte in fresh_nvs):
        raise ValueError(f"{FRESH_IMAGE} does not erase the complete NVS partition")


def manifest(version: str, image: str, offset: int) -> dict[str, object]:
    return {
        "name": "V1-Simple",
        "version": version,
        "home_assistant_domain": "",
        "funding_url": "",
        # ESP Web Tools otherwise erases without asking when Improv is absent.
        # The installer page explains that preservation requires leaving its
        # explicit erase choice off; the update part itself is app-only.
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [{"path": image, "offset": offset}],
            }
        ],
    }


def write_manifests(release_dir: Path, version: str, partitions_path: Path) -> None:
    if not version or version.strip() != version:
        raise ValueError("release version must be a non-empty trimmed string")
    partitions = read_partitions(partitions_path)
    validate_release_images(release_dir, partitions)

    documents = {
        FRESH_MANIFEST: manifest(version, FRESH_IMAGE, 0),
        UPDATE_MANIFEST: manifest(version, UPDATE_IMAGE, partitions["app"].offset),
    }
    for name, document in documents.items():
        (release_dir / name).write_text(
            json.dumps(document, indent=2) + "\n",
            encoding="utf-8",
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-dir", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--partitions", type=Path, default=DEFAULT_PARTITIONS)
    args = parser.parse_args()
    write_manifests(args.release_dir, args.version, args.partitions)
    print(
        f"[release] wrote {UPDATE_MANIFEST} at the parsed app offset and "
        f"{FRESH_MANIFEST} for the complete merged image"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
