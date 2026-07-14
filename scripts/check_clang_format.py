#!/usr/bin/env python3
"""Enforce clang-format cleanliness for the ratcheted set of firmware sources.

Only the paths listed in test/contracts/format_clean_manifest.txt are gated.
The rest of src/ and include/ is not clean yet (277 of 294 non-generated files
drifted when this gate landed), and a big-bang reformat was rejected: it would
invalidate the tracked mutation anchors wholesale and be unreviewable. So the
manifest is a ratchet -- entries are only ever added -- and this checker prints
every unratcheted file that is already clean and could be added for free.

Rules:
1) The clang-format binary must report the pinned version (22.1.8). A different
   release formats the same input differently, so an unpinned formatter would
   make this gate nondeterministic across dev machines and CI.
2) Every manifest path must exist. A deleted or renamed file must not silently
   drop out of the ratchet.
3) No manifest path may be a generated/vendored file excluded by
   .clang-format-ignore: clang-format skips those, so they would pass vacuously.
4) Every manifest path must be byte-identical to clang-format output.

Usage:
    python3 scripts/check_clang_format.py [--clang-format BIN]
                                          [--require-version X.Y.Z]
                                          [--manifest PATH] [--root PATH]

Exit codes:
    0 = every ratcheted file is clang-format clean
    1 = drift, bad manifest entry, or missing/mismatched formatter
    2 = usage / input error
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_RELPATH = "test/contracts/format_clean_manifest.txt"

# Pinned via the PyPI wheel: pip install "clang-format==22.1.8".
# Bumping this means bumping .github/workflows/ci.yml, .clang-format and
# CONTRIBUTING.md in the same commit, and re-verifying the manifest.
REQUIRED_VERSION = "22.1.8"

SCAN_DIRS = ("src", "include")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
VERSION_RE = re.compile(r"clang-format version (\d+\.\d+\.\d+)")
MAX_WORKERS = 8


class FormatCheckError(Exception):
    """Environment or manifest problem that makes the gate unrunnable."""


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-format", default="clang-format", help="clang-format executable")
    parser.add_argument(
        "--require-version",
        default=REQUIRED_VERSION,
        help="Exact clang-format version this repo is pinned to",
    )
    parser.add_argument("--manifest", default=None, help="Path to the ratchet manifest")
    parser.add_argument("--root", default=None, help="Repository root to scan")
    return parser.parse_args(argv)


def probe_version(binary: str) -> str:
    """Return the version string reported by the clang-format binary."""
    try:
        proc = subprocess.run(
            [binary, "--version"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except OSError as exc:
        raise FormatCheckError(
            f"failed to execute {binary!r}: {exc}\n"
            f'  Install the pinned formatter: pip install "clang-format=={REQUIRED_VERSION}"'
        ) from exc

    output = (proc.stdout or "").strip()
    if proc.returncode != 0:
        raise FormatCheckError(f"{binary} --version exited {proc.returncode}: {output}")

    match = VERSION_RE.search(output)
    if match is None:
        raise FormatCheckError(f"unable to parse clang-format version from: {output!r}")
    return match.group(1)


def require_version(binary: str, expected: str) -> str:
    version = probe_version(binary)
    if version != expected:
        raise FormatCheckError(
            f"clang-format {version} is not the pinned version {expected}.\n"
            "  Different clang-format releases format the same input differently, so an\n"
            "  unpinned formatter would make this gate nondeterministic across machines.\n"
            f'  Install the pin: pip install "clang-format=={expected}"'
        )
    return version


def read_ignore_patterns(ignore_path: Path) -> list[str]:
    """Read .clang-format-ignore, the single source of truth for exclusions."""
    if not ignore_path.is_file():
        raise FormatCheckError(f"missing exclusion list: {ignore_path}")
    patterns = [
        line.strip()
        for line in ignore_path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.strip().startswith("#")
    ]
    if not patterns:
        raise FormatCheckError(f"{ignore_path} lists no exclusion patterns")
    return patterns


def is_excluded(rel_path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatch(rel_path, pattern) for pattern in patterns)


def read_manifest(manifest_path: Path) -> tuple[list[str], list[str]]:
    """Return (paths, malformed-row errors) for the ratchet manifest."""
    if not manifest_path.is_file():
        raise FormatCheckError(f"manifest not found: {manifest_path}")

    paths: list[str] = []
    errors: list[str] = []
    seen: set[str] = set()
    previous: str | None = None
    for line_no, raw in enumerate(manifest_path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line in seen:
            errors.append(f"line {line_no}: duplicate entry: {line}")
            continue
        if previous is not None and line < previous:
            errors.append(f"line {line_no}: manifest is not sorted ({line} must precede {previous})")
        seen.add(line)
        previous = line
        paths.append(line)
    return paths, errors


def source_files(root: Path) -> list[str]:
    """Every production C/C++ source under src/ and include/, repo-relative."""
    found: list[str] = []
    for scan_dir in SCAN_DIRS:
        base = root / scan_dir
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                found.append(path.relative_to(root).as_posix())
    return sorted(found)


def dry_run(binary: str, root: Path, rel_path: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [binary, "--dry-run", "--Werror", "--style=file", rel_path],
        cwd=root,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def clean_paths(binary: str, root: Path, candidates: list[str]) -> set[str]:
    """Subset of candidates that clang-format reports as already formatted."""
    if not candidates:
        return set()
    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as pool:
        results = pool.map(lambda rel: (rel, dry_run(binary, root, rel).returncode == 0), candidates)
    return {rel for rel, ok in results if ok}


def display(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def run(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve() if args.root else ROOT
    manifest_path = Path(args.manifest).resolve() if args.manifest else root / MANIFEST_RELPATH
    ignore_path = root / ".clang-format-ignore"
    config_path = root / ".clang-format"

    if not config_path.is_file():
        raise FormatCheckError(f"missing formatter config: {config_path}")

    version = require_version(args.clang_format, args.require_version)
    ignore_patterns = read_ignore_patterns(ignore_path)
    manifest, errors = read_manifest(manifest_path)

    missing: list[str] = []
    excluded: list[str] = []
    gated: list[str] = []
    for rel_path in manifest:
        if not (root / rel_path).is_file():
            missing.append(rel_path)
        elif is_excluded(rel_path, ignore_patterns):
            excluded.append(rel_path)
        else:
            gated.append(rel_path)

    drifted = sorted(set(gated) - clean_paths(args.clang_format, root, gated))

    manifest_set = set(manifest)
    candidates = [
        rel_path
        for rel_path in source_files(root)
        if rel_path not in manifest_set and not is_excluded(rel_path, ignore_patterns)
    ]
    ready = sorted(clean_paths(args.clang_format, root, candidates))

    if errors or missing or excluded or drifted:
        print(f"[format] clang-format ratchet failed (clang-format {version}):")
        if errors:
            print("[format] malformed manifest rows:")
            for message in errors:
                print(f"  - {message}")
        if missing:
            print("[format] manifest entries that do not exist (renamed or deleted?):")
            for rel_path in missing:
                print(f"  - {rel_path}")
            print("  A ratcheted file must not silently drop out. Point the entry at the new path.")
        if excluded:
            print("[format] manifest entries excluded by .clang-format-ignore (generated/vendored):")
            for rel_path in excluded:
                print(f"  - {rel_path}")
            print("  clang-format skips these, so listing them would pass vacuously. Remove them.")
        if drifted:
            print("[format] ratcheted files that drifted from clang-format output:")
            for rel_path in drifted:
                print(f"  - {rel_path}")
                diagnostics = dry_run(args.clang_format, root, rel_path).stderr.strip()
                for line in diagnostics.splitlines()[:10]:
                    print(f"      {line}")
            print("  Fix with: clang-format -i " + " ".join(drifted))
        return 1

    print(
        f"[format] clang-format ratchet clean (clang-format {version}, "
        f"{len(gated)} ratcheted file(s), {len(candidates)} not ratcheted yet)"
    )
    if ready:
        print(
            f"[format] {len(ready)} unratcheted file(s) are ALREADY clang-format clean -- "
            f"add them to {display(manifest_path, root)} to advance the ratchet for free:"
        )
        for rel_path in ready:
            print(f"  + {rel_path}")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        return run(args)
    except FormatCheckError as exc:
        print(f"[format] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
