#!/usr/bin/env python3
"""Require a generated release commit to change only FIRMWARE_VERSION in config.h."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Sequence


SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SEMVER_PATTERN = (
    r"(?:0|[1-9][0-9]*)\."
    r"(?:0|[1-9][0-9]*)\."
    r"(?:0|[1-9][0-9]*)"
)
SEMVER_RE = re.compile(rf"^{SEMVER_PATTERN}$")
CONFIG_VERSION_RE = re.compile(
    rf'^(?P<prefix>[ \t]*#define[ \t]+FIRMWARE_VERSION[ \t]+")'
    rf"(?P<version>{SEMVER_PATTERN})"
    rf'(?P<suffix>"[^\r\n]*)$',
    re.MULTILINE,
)


class ReleaseConfigChangeError(RuntimeError):
    """The release commit changed firmware configuration beyond its version."""


def _git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown git error"
        raise ReleaseConfigChangeError(f"git {' '.join(args)} failed: {detail}")
    return result.stdout


def _resolve_commit(root: Path, value: str, label: str) -> str:
    sha = value.strip().lower()
    if not SHA_RE.fullmatch(sha):
        raise ReleaseConfigChangeError(
            f"{label} must be a full 40-character lowercase commit SHA"
        )
    resolved = _git(root, "rev-parse", "--verify", f"{sha}^{{commit}}").strip().lower()
    if resolved != sha:
        raise ReleaseConfigChangeError(f"{label} resolved to {resolved}, expected {sha}")
    return sha


def _normalized_config(text: str, label: str) -> tuple[str, str]:
    matches = list(CONFIG_VERSION_RE.finditer(text))
    if len(matches) != 1:
        raise ReleaseConfigChangeError(
            f"{label} must define one stable FIRMWARE_VERSION; found {len(matches)}"
        )
    match = matches[0]
    version = match.group("version")
    normalized = (
        text[: match.start("version")]
        + "<FIRMWARE_VERSION>"
        + text[match.end("version") :]
    )
    return version, normalized


def validate_release_config_change(
    root: Path,
    parent_sha: str,
    release_sha: str,
    expected_version: str,
) -> None:
    """Validate that parent and release config differ only in the version value."""

    root = root.resolve()
    if not (root / ".git").exists():
        raise ReleaseConfigChangeError(f"release root is not a Git repository: {root}")
    parent = _resolve_commit(root, parent_sha, "parent SHA")
    release = _resolve_commit(root, release_sha, "release SHA")
    if not SEMVER_RE.fullmatch(expected_version):
        raise ReleaseConfigChangeError(
            f"expected version must be stable MAJOR.MINOR.PATCH; found {expected_version!r}"
        )

    parent_text = _git(root, "show", f"{parent}:include/config.h")
    release_text = _git(root, "show", f"{release}:include/config.h")
    parent_version, normalized_parent = _normalized_config(parent_text, "parent config")
    release_version, normalized_release = _normalized_config(release_text, "release config")

    if release_version != expected_version:
        raise ReleaseConfigChangeError(
            f"release config defines FIRMWARE_VERSION {release_version}, "
            f"expected {expected_version}"
        )
    if parent_version == release_version:
        raise ReleaseConfigChangeError(
            f"release config did not change FIRMWARE_VERSION from {parent_version}"
        )
    if normalized_parent != normalized_release:
        raise ReleaseConfigChangeError(
            "release commit must change only the FIRMWARE_VERSION value in include/config.h"
        )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--parent-sha", required=True)
    parser.add_argument("--release-sha", required=True)
    parser.add_argument("--expected-version", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        validate_release_config_change(
            args.root,
            args.parent_sha,
            args.release_sha,
            args.expected_version,
        )
    except (OSError, ReleaseConfigChangeError) as exc:
        print(f"[release-config] ERROR: {exc}", file=sys.stderr)
        return 1
    print(
        "[release-config] release commit changes only FIRMWARE_VERSION "
        "in include/config.h"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
