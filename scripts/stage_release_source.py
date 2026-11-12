#!/usr/bin/env python3
"""Bind and stage the source that corresponds to a production release image."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

from check_release_firmware_version import require_release_firmware_version
from verify_esp32s3_framework import EXPECTED, sha256_file


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE_NAME = "arduino-esp32-source.tar.gz"
RECEIPT_NAME = "release_source.json"
FRAMEWORK_DIR = "framework-arduinoespressif32"
LICENSE_NAME = "LGPL-2.1.txt"
SOURCE_INPUT_ROOTS = ("src", "include", "lib", "interface/src", "interface/static")
ARDUINO_INPUTS = {
    "arduino_package_sha256": "package.json",
    "arduino_builder_sha256": "tools/pioarduino-build.py",
    "network_client_sha256": "libraries/Network/src/NetworkClient.cpp",
    "webserver_parsing_sha256": "libraries/WebServer/src/Parsing.cpp",
    "fs_header_sha256": "libraries/FS/src/FS.h",
}


def git_text(root: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def clean_source_sha(root: Path) -> str:
    if git_text(root, "status", "--porcelain", "--untracked-files=all") or git_text(
        root, "ls-files", "--others", "--ignored", "--exclude-standard", "--", *SOURCE_INPUT_ROOTS,
    ):
        raise ValueError("corresponding source requires a clean checkout without untracked build inputs")
    return git_text(root, "rev-parse", "HEAD")


def framework_path_included(path: Path) -> bool:
    return (
        not any(part.startswith(".") or part == "__pycache__" for part in path.parts)
        and path.suffix not in {".pyc", ".orig"}
    )


def inventory_sha256(entries: dict[str, str | tuple[int, str]]) -> str:
    digest = hashlib.sha256()
    for name, entry in sorted(entries.items()):
        digest.update((json.dumps([name, entry]) + "\n").encode())
    return digest.hexdigest()


def framework_tree_sha256(framework: Path) -> str:
    entries: dict[str, str | tuple[int, str]] = {}
    for path in framework.rglob("*"):
        relative = path.relative_to(framework)
        if not framework_path_included(relative):
            continue
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise ValueError(f"unexpected file type in Arduino source: {relative}")
        name = relative.as_posix()
        if path.is_dir():
            entries[name] = "directory"
        else:
            entries[name] = (path.stat().st_mode & 0o777, sha256_file(path))
    return inventory_sha256(entries)


def archived_framework_sha256(path: Path) -> str:
    entries: dict[str, str | tuple[int, str]] = {}
    with tarfile.open(path, mode="r|gz") as archive:
        for member in archive:
            if not member.name.startswith(f"{FRAMEWORK_DIR}/"):
                continue
            name = member.name.removeprefix(f"{FRAMEWORK_DIR}/")
            if member.isdir():
                entries[name] = "directory"
            elif member.isfile():
                with archive.extractfile(member) as source:
                    entries[name] = (member.mode, hashlib.file_digest(source, "sha256").hexdigest())
    return inventory_sha256(entries)


def record_build(root: Path, build_dir: Path, framework: Path, sha: str, tag: str) -> None:
    if clean_source_sha(root) != sha:
        raise ValueError("source changed during the release build")
    image = build_dir / "firmware.bin"
    require_release_firmware_version(image, tag)
    contract = json.loads((build_dir / "framework_contract.json").read_text(encoding="utf-8"))
    arduino = {path: contract.get(key) for key, path in ARDUINO_INPUTS.items()}
    if (
        arduino != {path: EXPECTED[key] for key, path in ARDUINO_INPUTS.items()}
        or arduino != {path: sha256_file(framework / path) for path in ARDUINO_INPUTS.values()}
    ):
        raise ValueError("release build did not record the qualified Arduino input hashes")
    receipt = {
        "tag": tag, "source_sha": sha, "firmware_sha256": sha256_file(image),
        "arduino": arduino, "arduino_tree_sha256": framework_tree_sha256(framework),
    }
    (build_dir / RECEIPT_NAME).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


def framework_member(member: tarfile.TarInfo) -> tarfile.TarInfo | None:
    name = Path(member.name)
    # Keep the installed package content, excluding installer/cache state and
    # the backup made when PlatformIO normalizes an upstream dependency file.
    if not framework_path_included(name):
        return None
    if not (member.isdir() or member.isfile()):
        raise ValueError(f"unexpected file type in Arduino source: {member.name}")
    member.uid = member.gid = member.mtime = 0
    member.mode &= 0o777
    member.uname = member.gname = ""
    member.pax_headers = {}
    return member


def add_project_source(archive: tarfile.TarFile, root: Path, sha: str) -> None:
    with tempfile.TemporaryFile() as project:
        subprocess.run(
            ["git", "-C", str(root), "archive", "--format=tar", "--prefix=v1simple/", sha],
            check=True,
            stdout=project,
        )
        project.seek(0)
        with tarfile.open(fileobj=project) as tracked:
            for member in tracked:
                archive.addfile(member, tracked.extractfile(member) if member.isfile() else None)


def stage_source(
    root: Path, build_dir: Path, framework: Path, release_dir: Path, tag: str,
) -> None:
    receipt = json.loads((build_dir / RECEIPT_NAME).read_text(encoding="utf-8"))
    sha = clean_source_sha(root)
    if (
        receipt.get("tag") != tag or receipt.get("source_sha") != sha
        or receipt.get("firmware_sha256") != sha256_file(release_dir / "firmware.bin")
    ):
        raise ValueError("corresponding source receipt disagrees with the release tag, checkout or firmware")
    arduino = {path: sha256_file(framework / path) for path in ARDUINO_INPUTS.values()}
    if arduino != receipt.get("arduino") or arduino != {path: EXPECTED[key] for key, path in ARDUINO_INPUTS.items()}:
        raise ValueError("Arduino source disagrees with the qualified release build inputs")
    if framework_tree_sha256(framework) != receipt.get("arduino_tree_sha256"):
        raise ValueError("Arduino source tree changed after the release build")

    archive_path = release_dir / ARCHIVE_NAME
    staging = archive_path.with_suffix(".tmp")
    try:
        with staging.open("wb") as output, gzip.GzipFile(filename="", fileobj=output, mode="wb", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w|") as archive:
                add_project_source(archive, root, sha)
                archive.add(framework, arcname=FRAMEWORK_DIR, filter=framework_member)
                archive.add(root / "licenses" / LICENSE_NAME, LICENSE_NAME, filter=framework_member)
                archive.add(root / "docs" / "ARDUINO_ESP32_REBUILD.md", "README.md", filter=framework_member)
                identity = (json.dumps(receipt, indent=2) + "\n").encode()
                info = tarfile.TarInfo(RECEIPT_NAME)
                info.size = len(identity)
                archive.addfile(info, io.BytesIO(identity))
        if archived_framework_sha256(staging) != receipt["arduino_tree_sha256"]:
            raise ValueError("archived Arduino source differs from the release build tree")
        staging.replace(archive_path)
    finally:
        staging.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-source", action="store_true", help="Print the clean release source SHA")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--arduino-source-dir", type=Path)
    parser.add_argument("--source-sha")
    parser.add_argument("--tag")
    args = parser.parse_args()
    try:
        if args.check_source:
            print(clean_source_sha(ROOT))
            return 0
        if not all((args.build_dir, args.arduino_source_dir, args.source_sha, args.tag)):
            parser.error("recording a build requires --build-dir, --arduino-source-dir, --source-sha and --tag")
        record_build(ROOT, args.build_dir, args.arduino_source_dir, args.source_sha, args.tag)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"[release-source] {exc}", file=sys.stderr)
        return 1
    print("[release-source] recorded corresponding source identity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
