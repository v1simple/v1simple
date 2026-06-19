#!/usr/bin/env python3
"""Clean generated workspace clutter without touching tracked source files.

By default this is a dry-run of the safe profile.

Usage:
    python3 scripts/clean_workspace.py
    python3 scripts/clean_workspace.py --safe --apply
    python3 scripts/clean_workspace.py --deep --apply
"""

from __future__ import annotations

import argparse
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SAFE_GLOBS = (
    ".artifacts/test_reports/**/workspace",
    ".artifacts/iron_gate",
    ".artifacts/extended_gate_*",
    ".artifacts/solution_validation_*",
    ".artifacts/stability_surgery",
    ".artifacts/web-installer",
    ".artifacts/web_baseline_*",
    "release",
    "dist",
    "interface/build",
    "interface/.svelte-kit",
    "interface/coverage",
    "scripts/**/__pycache__",
    "tools/**/__pycache__",
    "road_map.bin",
    "compile_commands.json",
    "docs/install/*.bin",
)
DEEP_PATHS = (
    ".artifacts",
    ".pio",
    "interface/node_modules",
    ".scratch",
    "data",
)
POST_DELETE_EMPTY_DIRS = (
    "docs/install",
)


@dataclass(frozen=True)
class CleanupTarget:
    path: Path
    size_bytes: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    profile = parser.add_mutually_exclusive_group()
    profile.add_argument("--safe", action="store_true", help="clean generated outputs only")
    profile.add_argument(
        "--deep",
        action="store_true",
        help="clean safe targets plus heavyweight caches and dependencies",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="delete matched paths (default is dry-run)",
    )
    return parser.parse_args()


def ensure_within_root(path: Path) -> Path:
    resolved = path.resolve(strict=False)
    if not resolved.is_relative_to(ROOT.resolve()):
        raise RuntimeError(f"Refusing to clean path outside repo root: {path}")
    return resolved


def iter_safe_candidates() -> list[Path]:
    candidates: list[Path] = []
    for pattern in SAFE_GLOBS:
        candidates.extend(sorted(ROOT.glob(pattern)))
    return candidates


def iter_deep_candidates() -> list[Path]:
    return [ROOT / relative for relative in DEEP_PATHS]


def normalize_targets(raw_paths: list[Path]) -> list[Path]:
    existing: list[Path] = []
    for path in raw_paths:
        if not path.exists() and not path.is_symlink():
            continue
        ensure_within_root(path)
        existing.append(path)

    existing = sorted(set(existing), key=lambda item: (len(item.parts), str(item)))

    filtered: list[Path] = []
    for path in existing:
        if any(path.is_relative_to(parent) for parent in filtered):
            continue
        filtered.append(path)
    return filtered


def path_size(path: Path) -> int:
    if path.is_symlink() or path.is_file():
        try:
            return path.lstat().st_size
        except FileNotFoundError:
            return 0

    total = 0
    for child in path.rglob("*"):
        if child.is_symlink() or child.is_file():
            try:
                total += child.lstat().st_size
            except FileNotFoundError:
                continue
    return total


def collect_targets(profile: str) -> list[CleanupTarget]:
    raw_paths = iter_safe_candidates()
    if profile == "deep":
        raw_paths.extend(iter_deep_candidates())
    targets = normalize_targets(raw_paths)
    return [CleanupTarget(path=path, size_bytes=path_size(path)) for path in targets]


def human_size(size_bytes: int) -> str:
    units = ("B", "KB", "MB", "GB", "TB")
    size = float(size_bytes)
    for unit in units:
        if size < 1024 or unit == units[-1]:
            if unit == "B":
                return f"{int(size)}{unit}"
            return f"{size:.1f}{unit}"
        size /= 1024
    return f"{size_bytes}B"


def delete_path(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink(missing_ok=True)
        return
    shutil.rmtree(path, ignore_errors=False)


def remove_empty_post_dirs() -> list[Path]:
    removed: list[Path] = []
    for relative in POST_DELETE_EMPTY_DIRS:
        path = ROOT / relative
        if not path.is_dir():
            continue
        try:
            next(path.iterdir())
            continue
        except StopIteration:
            path.rmdir()
            removed.append(path)
    return removed


def main() -> int:
    args = parse_args()
    profile = "deep" if args.deep else "safe"
    targets = collect_targets(profile)
    total_bytes = sum(target.size_bytes for target in targets)

    action = "Deleting" if args.apply else "Dry-run"
    print(
        f"{action} {len(targets)} path(s) from profile '{profile}' "
        f"({human_size(total_bytes)} total)"
    )

    if not targets:
        print("Nothing to clean.")
        return 0

    for target in targets:
        rel = target.path.relative_to(ROOT)
        print(f" - {rel} ({human_size(target.size_bytes)})")

    if not args.apply:
        print("No files deleted. Re-run with --apply to remove these paths.")
        return 0

    for target in targets:
        delete_path(target.path)

    removed_empty = remove_empty_post_dirs()
    for path in removed_empty:
        print(f" - removed empty directory {path.relative_to(ROOT)}")

    print("Workspace cleanup complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
