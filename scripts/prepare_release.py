#!/usr/bin/env python3
"""Choose the semantic version for an exact, already-tested release commit.

Normal releases increment the newest strict ``vMAJOR.MINOR.PATCH`` tag by one
patch. A reviewed ``FIRMWARE_VERSION`` set to exactly the next minor or major
selects that larger bump. The script never edits the checkout: Release injects
the selected version while building and tags the tested commit directly.

Only an annotated tag carrying the current GitHub Actions run ID may resume an
interrupted publication. The newest strict tag is also reported so an older
retry cannot replace a newer Pages installer.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEMVER_RE = re.compile(
    r"^(?P<major>0|[1-9][0-9]*)\."
    r"(?P<minor>0|[1-9][0-9]*)\."
    r"(?P<patch>0|[1-9][0-9]*)$"
)
CONFIG_VERSION_RE = re.compile(
    r'^\s*#define\s+FIRMWARE_VERSION\s+"(?P<version>[^"]+)"\s*$',
    re.MULTILINE,
)
RUN_ID_RE = re.compile(r"^[1-9][0-9]*$")


class ReleasePreparationError(RuntimeError):
    """A release cannot be selected without violating version invariants."""


@dataclass(frozen=True, order=True)
class Version:
    major: int
    minor: int
    patch: int

    @classmethod
    def parse(cls, value: str) -> "Version":
        match = SEMVER_RE.fullmatch(value)
        if not match:
            raise ReleasePreparationError(
                f"invalid stable semantic version {value!r}; expected MAJOR.MINOR.PATCH"
            )
        return cls(*(int(match.group(name)) for name in ("major", "minor", "patch")))

    def bump(self, kind: str) -> "Version":
        if kind == "patch":
            return Version(self.major, self.minor, self.patch + 1)
        if kind == "minor":
            return Version(self.major, self.minor + 1, 0)
        if kind == "major":
            return Version(self.major + 1, 0, 0)
        raise ReleasePreparationError(f"unsupported release bump: {kind}")

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"


@dataclass(frozen=True)
class VersionTag:
    version: Version
    name: str


def git(root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown git error"
        raise ReleasePreparationError(f"git {' '.join(args)} failed: {detail}")
    return result


def strict_version_tags(root: Path) -> list[VersionTag]:
    tags: list[VersionTag] = []
    for raw in git(root, "tag", "--list").stdout.splitlines():
        name = raw.strip()
        if name.startswith("v") and SEMVER_RE.fullmatch(name[1:]):
            tags.append(VersionTag(Version.parse(name[1:]), name))
    return sorted(tags, key=lambda item: item.version)


def resolve_commit(root: Path, ref: str) -> str | None:
    result = git(root, "rev-parse", "--verify", f"{ref}^{{commit}}", check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def release_for_run_id(root: Path, run_id: str) -> tuple[VersionTag, str] | None:
    """Find the unique annotated semantic-version tag published by one run."""

    if not RUN_ID_RE.fullmatch(run_id):
        raise ReleasePreparationError(f"invalid GitHub Actions run ID: {run_id!r}")
    marker = f"Release-Run-ID: {run_id}"
    matches: list[tuple[VersionTag, str]] = []
    for tag in strict_version_tags(root):
        object_type = git(root, "cat-file", "-t", f"refs/tags/{tag.name}").stdout.strip()
        if object_type != "tag":
            continue
        contents = git(
            root,
            "for-each-ref",
            "--format=%(contents)",
            f"refs/tags/{tag.name}",
        ).stdout
        if marker not in contents.splitlines():
            continue
        commit = resolve_commit(root, tag.name)
        if commit is None:
            raise ReleasePreparationError(f"could not resolve release tag {tag.name}")
        matches.append((tag, commit))

    if len(matches) > 1:
        names = ", ".join(tag.name for tag, _ in matches)
        raise ReleasePreparationError(
            f"workflow run {run_id} is recorded by multiple release tags: {names}"
        )
    return matches[0] if matches else None


def require_ancestor(root: Path, ancestor: str, descendant: str) -> None:
    result = git(root, "merge-base", "--is-ancestor", ancestor, descendant, check=False)
    if result.returncode == 0:
        return
    if result.returncode == 1:
        raise ReleasePreparationError(
            f"latest release tag {ancestor} is not an ancestor of {descendant}; "
            "refusing a branched release"
        )
    raise ReleasePreparationError(result.stderr.strip() or "git merge-base failed")


def read_config_version(config_path: Path) -> Version:
    matches = list(CONFIG_VERSION_RE.finditer(config_path.read_text(encoding="utf-8")))
    if len(matches) != 1:
        raise ReleasePreparationError(
            f"{config_path} must contain exactly one plain FIRMWARE_VERSION definition"
        )
    return Version.parse(matches[0].group("version"))


def next_version(latest: Version, source: Version) -> tuple[Version, str]:
    """Select patch by default; accept only an exact reviewed minor/major jump."""

    candidates = {
        "patch": latest.bump("patch"),
        "minor": latest.bump("minor"),
        "major": latest.bump("major"),
    }
    if source == candidates["minor"]:
        return source, "minor"
    if source == candidates["major"]:
        return source, "major"
    if source > candidates["patch"]:
        raise ReleasePreparationError(
            f"FIRMWARE_VERSION {source} is not the next patch, minor, or major after {latest}"
        )
    return candidates["patch"], "patch"


def prepare_release(root: Path, resume_tag: str = "") -> dict[str, str]:
    root = root.resolve()
    config_path = root / "include" / "config.h"
    if not config_path.is_file():
        raise ReleasePreparationError("release root must contain include/config.h")
    if git(root, "status", "--porcelain").stdout.strip():
        raise ReleasePreparationError("release selection requires a clean working tree")

    head = git(root, "rev-parse", "HEAD").stdout.strip()
    source = read_config_version(config_path)
    tags = strict_version_tags(root)
    latest = tags[-1] if tags else None
    resume_tag = resume_tag.strip()

    if resume_tag:
        if not resume_tag.startswith("v") or not SEMVER_RE.fullmatch(resume_tag[1:]):
            raise ReleasePreparationError(
                f"invalid resume tag {resume_tag!r}; expected vMAJOR.MINOR.PATCH"
            )
        resume_commit = resolve_commit(root, resume_tag)
        if resume_commit != head:
            found = resume_commit or "missing"
            raise ReleasePreparationError(
                f"resume tag {resume_tag} resolves to {found}, expected HEAD {head}"
            )
        target = Version.parse(resume_tag[1:])
        bump = "rerun"
        mode = "rerun"
    elif latest is None:
        target = source
        bump = "initial"
        mode = "initial"
    else:
        require_ancestor(root, latest.name, head)
        target, bump = next_version(latest.version, source)
        mode = "new"
        target_tag = f"v{target}"
        existing = resolve_commit(root, target_tag)
        if existing is not None:
            raise ReleasePreparationError(
                f"release tag {target_tag} already exists at {existing}; "
                "only the workflow run recorded in that tag may reuse it"
            )

    values = {
        "version": str(target),
        "tag": f"v{target}",
        "source_version": str(source),
        "previous_tag": latest.name if latest else "",
        "mode": mode,
        "bump": bump,
    }
    print(
        f"Release version: source={source}, selected={target}, mode={mode}, "
        f"bump={bump}, previous_tag={values['previous_tag'] or 'none'}"
    )
    return values


def write_outputs(path: Path | None, values: dict[str, str]) -> None:
    if path is None:
        return
    with path.open("a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--prepare", action="store_true")
    operation.add_argument("--lookup-run-id", metavar="RUN_ID")
    operation.add_argument("--latest-tag", action="store_true")
    parser.add_argument(
        "--resume-tag",
        default="",
        help="Reuse this tag only after --lookup-run-id matched the same workflow run",
    )
    parser.add_argument(
        "--github-output",
        type=Path,
        default=Path(os.environ["GITHUB_OUTPUT"]) if os.environ.get("GITHUB_OUTPUT") else None,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.latest_tag:
            if args.resume_tag:
                raise ReleasePreparationError("--resume-tag is valid only with --prepare")
            tags = strict_version_tags(args.root.resolve())
            values = {"latest_tag": tags[-1].name if tags else ""}
            print(values["latest_tag"])
        elif args.lookup_run_id:
            if args.resume_tag:
                raise ReleasePreparationError("--resume-tag is valid only with --prepare")
            match = release_for_run_id(args.root.resolve(), args.lookup_run_id)
            values = {
                "resume_tag": match[0].name if match else "",
                "resume_sha": match[1] if match else "",
            }
            if match:
                print(
                    f"Found prior publication for run {args.lookup_run_id}: "
                    f"{match[0].name} at {match[1]}"
                )
            else:
                print(f"No prior publication found for run {args.lookup_run_id}")
        else:
            values = prepare_release(args.root, args.resume_tag)
        write_outputs(args.github_output, values)
    except (OSError, ReleasePreparationError) as exc:
        print(f"[release-version] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
