#!/usr/bin/env python3
"""Require tracked source, including submodules, to match HEAD exactly."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    return parser.parse_args()


def tracked_source_is_clean(repo: Path) -> bool:
    completed = subprocess.run(
        [
            "git",
            "-C",
            str(repo),
            "diff",
            "--quiet",
            "--ignore-submodules=untracked",
            "HEAD",
            "--",
        ],
        check=False,
    )
    if completed.returncode not in (0, 1):
        raise RuntimeError("git could not compare the tracked source state to HEAD")
    return completed.returncode == 0


def main() -> int:
    args = parse_args()
    try:
        clean = tracked_source_is_clean(args.repo)
    except RuntimeError as exc:
        print(f"[source-state] {exc}.", file=sys.stderr)
        return 1

    if not clean:
        print(
            "[source-state] source worktree is dirty; local CI qualification "
            "requires an exact clean tracked source state.",
            file=sys.stderr,
        )
        return 1

    print("[source-state] tracked source matches HEAD")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
