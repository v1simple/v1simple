#!/usr/bin/env python3
"""Stage and verify the license material shipped with every release."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

ROOT_FILES = (
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
)

LICENSE_FILES = (
    "licenses/ArduinoJson-LICENSE.txt",
    "licenses/NimBLE-Arduino-LICENSE.txt",
    "licenses/NimBLE-Arduino-NOTICE.txt",
    "licenses/Arduino-GFX-LICENSE.txt",
    "licenses/OpenFontRender-LICENSE.txt",
    "licenses/FreeType-FTL.txt",
    "licenses/GNU-FreeFont-COPYING.txt",
    "licenses/GNU-FreeFont-README.txt",
    "licenses/OFL-1.1.txt",
    "licenses/Geist-OFL-1.1.txt",
    "licenses/CC-BY-4.0.txt",
    "licenses/Svelte-LICENSE.md",
    "licenses/SvelteKit-LICENSE.txt",
    "licenses/daisyUI-LICENSE.txt",
    "licenses/Tailwind-CSS-LICENSE.txt",
)

# Exact upstream files, retained without editing:
# - GNU FreeFont 2012-05-03 OTF archive from ftp.gnu.org, archive SHA-256
#   3a6c51868c71b006c33c4bcde63d90927e6fcca8f51c965b8ad62d021614a860
# - Creative Commons BY 4.0 legalcode.txt from creativecommons.org
PINNED_SOURCE_SHA256 = {
    "licenses/GNU-FreeFont-COPYING.txt":
        "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903",
    "licenses/GNU-FreeFont-README.txt":
        "d7375e3982a2e0afc471f8900c46ac6ea41b1112de1f5cdb9ca5c820adf25a33",
    "licenses/CC-BY-4.0.txt":
        "9ba9550ad48438d0836ddab3da480b3b69ffa0aac7b7878b5a0039e7ab429411",
}


def source_errors(source_root: Path) -> list[str]:
    errors: list[str] = []
    for relative in (*ROOT_FILES, *LICENSE_FILES):
        source = source_root / relative
        if not source.is_file():
            errors.append(f"missing release license source: {relative}")
            continue
        content = source.read_bytes()
        if not content:
            errors.append(f"empty release license source: {relative}")
            continue
        expected_hash = PINNED_SOURCE_SHA256.get(relative)
        if expected_hash and hashlib.sha256(content).hexdigest() != expected_hash:
            errors.append(f"pinned release license source changed: {relative}")
    return errors


def staged_paths(
    source_root: Path, release_dir: Path, pages_dir: Path
) -> list[tuple[Path, Path]]:
    paths: list[tuple[Path, Path]] = []
    for relative in ROOT_FILES:
        source = source_root / relative
        paths.append((source, release_dir / source.name))
        paths.append((source, pages_dir / source.name))
    for relative in LICENSE_FILES:
        source = source_root / relative
        paths.append((source, release_dir / source.name))
        paths.append((source, pages_dir / "licenses" / source.name))
    return paths


def staged_errors(
    source_root: Path, release_dir: Path, pages_dir: Path
) -> list[str]:
    errors = source_errors(source_root)
    if errors:
        return errors
    for source, destination in staged_paths(source_root, release_dir, pages_dir):
        if not destination.is_file():
            errors.append(f"missing staged release license: {destination}")
        elif destination.read_bytes() != source.read_bytes():
            errors.append(f"staged release license differs from source: {destination}")
    return errors


def stage(source_root: Path, release_dir: Path, pages_dir: Path) -> None:
    errors = source_errors(source_root)
    if errors:
        raise ValueError("; ".join(errors))

    release_dir.mkdir(parents=True, exist_ok=True)
    (pages_dir / "licenses").mkdir(parents=True, exist_ok=True)
    for source, destination in staged_paths(source_root, release_dir, pages_dir):
        shutil.copyfile(source, destination)

    errors = staged_errors(source_root, release_dir, pages_dir)
    if errors:
        raise ValueError("; ".join(errors))


def resolve_from_root(path: Path, root: Path) -> Path:
    return path if path.is_absolute() else root / path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--release-dir", type=Path, required=True)
    parser.add_argument("--pages-dir", type=Path, required=True)
    args = parser.parse_args()

    source_root = args.root.resolve()
    release_dir = resolve_from_root(args.release_dir, source_root)
    pages_dir = resolve_from_root(args.pages_dir, source_root)
    try:
        stage(source_root, release_dir, pages_dir)
    except ValueError as exc:
        print(f"[release-licenses] {exc}", file=sys.stderr)
        return 1

    count = len(ROOT_FILES) + len(LICENSE_FILES)
    print(f"[release-licenses] staged and verified {count} files in both packages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
