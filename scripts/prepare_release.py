#!/usr/bin/env python3
"""Prepare an idempotent semantic-version release commit.

The release workflow calls this before its expensive CI and firmware builds.
It derives the next version from immutable ``vN.N.N`` tags, updates the
firmware version and changelog, and writes GitHub Actions step outputs.  Only a
tag positively matched to the same GitHub Actions run may be reused; a fresh
workflow dispatch from a tagged ``main`` prepares the selected bump.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REPOSITORY = "v1simple/v1simple"
SEMVER_RE = re.compile(
    r"^(?P<major>0|[1-9][0-9]*)\."
    r"(?P<minor>0|[1-9][0-9]*)\."
    r"(?P<patch>0|[1-9][0-9]*)$"
)
CONFIG_VERSION_RE = re.compile(
    r'^(?P<prefix>\s*#define\s+FIRMWARE_VERSION\s+")'
    r'(?P<version>[^"]+)'
    r'(?P<suffix>"\s*)$',
    re.MULTILINE,
)
UNRELEASED_HEADING_RE = re.compile(r"^## \[Unreleased\]\s*$", re.MULTILINE)
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
RUN_ID_RE = re.compile(r"^[1-9][0-9]*$")


class ReleasePreparationError(RuntimeError):
    """A release cannot be prepared without violating version invariants."""


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
        if not name.startswith("v"):
            continue
        match = SEMVER_RE.fullmatch(name[1:])
        if match:
            tags.append(VersionTag(Version.parse(name[1:]), name))
    return sorted(tags, key=lambda item: item.version)


def resolve_commit(root: Path, ref: str) -> str | None:
    result = git(root, "rev-parse", "--verify", f"{ref}^{{commit}}", check=False)
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def release_for_run_id(root: Path, run_id: str) -> tuple[VersionTag, str] | None:
    """Find the unique annotated semver tag published by a workflow run."""

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
            raise ReleasePreparationError(f"could not resolve release tag {tag.name} to a commit")
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
    detail = result.stderr.strip() or "git merge-base failed"
    raise ReleasePreparationError(detail)


def read_config_version(config_path: Path) -> tuple[str, Version]:
    text = config_path.read_text(encoding="utf-8")
    matches = list(CONFIG_VERSION_RE.finditer(text))
    if len(matches) != 1:
        raise ReleasePreparationError(
            f"{config_path} must contain exactly one plain FIRMWARE_VERSION definition"
        )
    raw = matches[0].group("version")
    return text, Version.parse(raw)


def update_config_version(text: str, version: Version) -> str:
    updated, count = CONFIG_VERSION_RE.subn(
        lambda match: f'{match.group("prefix")}{version}{match.group("suffix")}',
        text,
    )
    if count != 1:
        raise ReleasePreparationError("could not update the unique FIRMWARE_VERSION definition")
    return updated


def version_heading(version: Version) -> re.Pattern[str]:
    return re.compile(rf"^## \[{re.escape(str(version))}\](?:\s|$)", re.MULTILINE)


def update_changelog(
    text: str,
    version: Version,
    release_date: str,
    repository: str,
    previous_tag: str | None,
) -> str:
    if version_heading(version).search(text):
        updated = text
    else:
        unreleased = UNRELEASED_HEADING_RE.search(text)
        if not unreleased:
            raise ReleasePreparationError("CHANGELOG.md is missing the ## [Unreleased] heading")
        remaining = text[unreleased.end() :].lstrip("\n")
        release_intro = (
            f"## [{version}] - {release_date}\n\n"
            "Changes are summarized in the generated GitHub release notes.\n\n"
        )
        updated = text[: unreleased.end()] + "\n\n" + release_intro + remaining

    unreleased_url = f"https://github.com/{repository}/compare/v{version}...HEAD"
    unreleased_link = re.compile(r"^\[Unreleased\]:\s+\S+\s*$", re.MULTILINE)
    if unreleased_link.search(updated):
        updated = unreleased_link.sub(f"[Unreleased]: {unreleased_url}", updated, count=1)
    else:
        updated = updated.rstrip() + f"\n\n[Unreleased]: {unreleased_url}\n"

    release_link_re = re.compile(
        rf"^\[{re.escape(str(version))}\]:\s+\S+\s*$",
        re.MULTILINE,
    )
    if not release_link_re.search(updated):
        release_url = f"https://github.com/{repository}/releases/tag/v{version}"
        marker = f"[Unreleased]: {unreleased_url}"
        updated = updated.replace(marker, marker + f"\n[{version}]: {release_url}", 1)

    if previous_tag and previous_tag == f"v{version}":
        raise ReleasePreparationError("previous tag unexpectedly equals the prepared release tag")
    return updated


def write_outputs(path: Path | None, values: dict[str, str]) -> None:
    if path is None:
        return
    with path.open("a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")


def prepare_release(
    root: Path,
    bump: str,
    release_date: str,
    repository: str,
    resume_tag: str = "",
) -> dict[str, str]:
    root = root.resolve()
    config_path = root / "include" / "config.h"
    changelog_path = root / "CHANGELOG.md"
    if not config_path.is_file() or not changelog_path.is_file():
        raise ReleasePreparationError("release root must contain include/config.h and CHANGELOG.md")
    if not REPOSITORY_RE.fullmatch(repository):
        raise ReleasePreparationError(f"invalid GitHub repository name: {repository!r}")
    try:
        dt.date.fromisoformat(release_date)
    except ValueError as exc:
        raise ReleasePreparationError(
            f"invalid release date {release_date!r}; expected YYYY-MM-DD"
        ) from exc
    dirty = git(root, "status", "--porcelain").stdout.strip()
    if dirty:
        raise ReleasePreparationError(
            "release preparation requires a clean working tree before version files are updated"
        )

    head = git(root, "rev-parse", "HEAD").stdout.strip()
    config_text, current = read_config_version(config_path)
    tags = strict_version_tags(root)
    latest = tags[-1] if tags else None
    if latest:
        require_ancestor(root, latest.name, head)

    resume_tag = resume_tag.strip()
    if resume_tag:
        if not resume_tag.startswith("v") or not SEMVER_RE.fullmatch(resume_tag[1:]):
            raise ReleasePreparationError(
                f"invalid resume tag {resume_tag!r}; expected vMAJOR.MINOR.PATCH"
            )
        resume_version = Version.parse(resume_tag[1:])
        resume_commit = resolve_commit(root, resume_tag)
        if resume_commit != head:
            found = resume_commit or "missing"
            raise ReleasePreparationError(
                f"resume tag {resume_tag} resolves to {found}, expected HEAD {head}"
            )
        if resume_version != current:
            raise ReleasePreparationError(
                f"resume tag {resume_tag} does not match FIRMWARE_VERSION {current}"
            )
        target = current
        mode = "rerun"
    elif latest is None:
        target = current
        mode = "initial"
    else:
        if current != latest.version:
            raise ReleasePreparationError(
                f"FIRMWARE_VERSION {current} must match latest release tag {latest.name}; "
                "the workflow owns version bumps"
            )
        target = latest.version.bump(bump)
        mode = "new"

    target_tag = f"v{target}"
    target_tag_commit = resolve_commit(root, target_tag)
    if target_tag_commit and target_tag_commit != head:
        raise ReleasePreparationError(
            f"release tag {target_tag} already points at {target_tag_commit}; refusing to move it"
        )

    changelog_text = changelog_path.read_text(encoding="utf-8")
    if mode == "rerun":
        if not version_heading(target).search(changelog_text):
            raise ReleasePreparationError(
                f"tag {target_tag} points at HEAD but CHANGELOG.md lacks version {target}"
            )
        changed = False
    else:
        new_config = update_config_version(config_text, target)
        new_changelog = update_changelog(
            changelog_text,
            target,
            release_date,
            repository,
            latest.name if latest else None,
        )
        changed = new_config != config_text or new_changelog != changelog_text
        config_path.write_text(new_config, encoding="utf-8")
        changelog_path.write_text(new_changelog, encoding="utf-8")

    values = {
        "version": str(target),
        "tag": target_tag,
        "previous_tag": latest.name if latest else "",
        "mode": mode,
        "files_changed": "true" if changed else "false",
    }
    print(
        f"Release version: {current} -> {target} "
        f"(mode={mode}, requested_bump={bump}, previous_tag={values['previous_tag'] or 'none'})"
    )
    return values


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--bump", choices=("patch", "minor", "major"))
    operation.add_argument("--lookup-run-id", metavar="RUN_ID")
    parser.add_argument(
        "--resume-tag",
        default="",
        help="Reuse this tag only after --lookup-run-id matched the same workflow run",
    )
    parser.add_argument("--date", default=dt.datetime.now(dt.timezone.utc).date().isoformat())
    parser.add_argument(
        "--repository",
        default=os.environ.get("GITHUB_REPOSITORY", DEFAULT_REPOSITORY),
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
        if args.lookup_run_id:
            if args.resume_tag:
                raise ReleasePreparationError(
                    "--resume-tag is valid only with the --bump operation"
                )
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
            values = prepare_release(
                args.root,
                args.bump,
                args.date,
                args.repository,
                args.resume_tag,
            )
        write_outputs(args.github_output, values)
    except (OSError, ReleasePreparationError) as exc:
        print(f"[release-version] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
