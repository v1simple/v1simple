#!/usr/bin/env python3
"""Enforce clang-format cleanliness for every firmware source file.

This gate landed as a ratchet: only the paths listed in a manifest were gated,
because 277 of 294 non-generated files drifted from the never-applied
.clang-format config, and a big-bang reformat would have invalidated the tracked
mutation anchors wholesale and been unreviewable. The reformat then landed in
four reviewable slices (leaf modules -> wifi/system -> include+src root ->
hot paths), and the manifest reached full coverage, so the ratchet is retired:
this now globs src/ and include/ and the manifest is gone.

Rules:
1) The clang-format binary must report the pinned version (22.1.8). A different
   release formats the same input differently, so an unpinned formatter would
   make this gate nondeterministic across dev machines and CI.
2) Every non-excluded C/C++ file under src/ and include/ must be byte-identical
   to clang-format output.
3) Exclusions come from .clang-format-ignore, which clang-format itself honors,
   so editors and `clang-format -i` skip the same generated/vendored files this
   gate does. There is no second exclusion list to drift out of sync.

Usage:
    python3 scripts/check_clang_format.py [--clang-format BIN]
                                          [--require-version X.Y.Z] [--root PATH]

Exit codes:
    0 = every gated file is clang-format clean
    1 = drift, or a missing/mismatched formatter
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

# Pinned via the PyPI wheel: pip install "clang-format==22.1.8".
# Bumping this means bumping .github/workflows/ci.yml, .clang-format and
# CONTRIBUTING.md in the same commit, and reformatting the tree in that commit.
REQUIRED_VERSION = "22.1.8"

SCAN_DIRS = ("src", "include")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
VERSION_RE = re.compile(r"clang-format version (\d+\.\d+\.\d+)")
MAX_WORKERS = 8


class FormatCheckError(Exception):
    """Environment problem that makes the gate unrunnable."""


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-format", default="clang-format", help="clang-format executable")
    parser.add_argument(
        "--require-version",
        default=REQUIRED_VERSION,
        help="Exact clang-format version this repo is pinned to",
    )
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


def run(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve() if args.root else ROOT
    ignore_path = root / ".clang-format-ignore"
    config_path = root / ".clang-format"

    if not config_path.is_file():
        raise FormatCheckError(f"missing formatter config: {config_path}")

    version = require_version(args.clang_format, args.require_version)
    ignore_patterns = read_ignore_patterns(ignore_path)

    gated = [rel for rel in source_files(root) if not is_excluded(rel, ignore_patterns)]
    if not gated:
        raise FormatCheckError(
            "no source files to gate -- every file under src/ and include/ is excluded.\n"
            "  A gate that checks nothing passes vacuously; check .clang-format-ignore."
        )

    drifted = sorted(set(gated) - clean_paths(args.clang_format, root, gated))

    if drifted:
        print(f"[format] clang-format check failed (clang-format {version}):")
        print("[format] files that drifted from clang-format output:")
        for rel_path in drifted:
            print(f"  - {rel_path}")
            diagnostics = dry_run(args.clang_format, root, rel_path).stderr.strip()
            for line in diagnostics.splitlines()[:10]:
                print(f"      {line}")
        print("  Fix with: clang-format -i " + " ".join(drifted))
        return 1

    print(f"[format] clang-format clean (clang-format {version}, {len(gated)} file(s) gated)")
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
