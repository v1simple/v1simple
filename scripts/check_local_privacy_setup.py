#!/usr/bin/env python3
"""Verify this checkout's local, fail-closed public-repository privacy setup."""

from __future__ import annotations

import os
from pathlib import Path
import stat
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
REQUIRED_HOOKS = (
    "pre-commit",
    "prepare-commit-msg",
    "commit-msg",
    "reference-transaction",
    "pre-push",
)
REQUIRED_LOCAL_CONFIG = {
    "core.hooksPath": ".githooks",
    "user.useConfigOnly": "true",
    "user.name": "v1simple",
    "user.email": "noreply@example.invalid",
    "author.name": "v1simple",
    "author.email": "noreply@example.invalid",
    "committer.name": "v1simple",
    "committer.email": "noreply@example.invalid",
    "push.default": "simple",
    "push.followTags": "false",
    "remote.pushDefault": "origin",
    "branch.main.remote": "origin",
    "branch.main.pushRemote": "origin",
    "branch.main.merge": "refs/heads/main",
}
PUBLIC_REMOTE_URLS = {
    "https://github.com/v1simple/v1simple.git",
}
CONFIG_INJECTION_ENV = (
    "GIT_CONFIG_COUNT",
    "GIT_CONFIG_GLOBAL",
    "GIT_CONFIG_PARAMETERS",
    "GIT_CONFIG_SYSTEM",
)


def run(arguments: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )


def local_config(key: str) -> str | None:
    completed = run(["git", "config", "--local", "--get", key])
    if completed.returncode == 1:
        return None
    if completed.returncode != 0:
        raise RuntimeError("Git configuration could not be inspected")
    return completed.stdout.rstrip("\n")


def git_value(arguments: list[str]) -> str | None:
    completed = run(["git", *arguments])
    if completed.returncode == 1:
        return None
    if completed.returncode != 0:
        raise RuntimeError("Git configuration could not be inspected")
    return completed.stdout.rstrip("\n")


def active_private_terms(path: Path) -> int:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise RuntimeError("Private term list could not be inspected") from exc
    return sum(
        1
        for line in text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )


def main() -> int:
    errors: list[str] = []

    for key, expected in REQUIRED_LOCAL_CONFIG.items():
        try:
            actual = local_config(key)
        except RuntimeError:
            actual = None
        if actual != expected:
            errors.append(f"required local Git setting is missing or unsafe: {key}")

    try:
        configured_push_url = local_config("remote.origin.pushurl")
        fetch_url = git_value(["remote", "get-url", "origin"])
        push_url = git_value(["remote", "get-url", "--push", "origin"])
    except RuntimeError:
        configured_push_url = None
        fetch_url = None
        push_url = None
    if configured_push_url is not None:
        errors.append("origin has a separate push destination")
    if fetch_url not in PUBLIC_REMOTE_URLS or push_url != fetch_url:
        errors.append("origin does not resolve to the approved public repository")

    for hook_name in REQUIRED_HOOKS:
        hook = ROOT / ".githooks" / hook_name
        if not hook.is_file() or not os.access(hook, os.X_OK):
            errors.append(f"required privacy hook is missing or not executable: {hook_name}")

    for variable in CONFIG_INJECTION_ENV:
        if variable in os.environ:
            errors.append(f"Git configuration injection environment is active: {variable}")
    if any(name.startswith("GIT_CONFIG_KEY_") for name in os.environ):
        errors.append("Git configuration key injection environment is active")
    if any(name.startswith("GIT_CONFIG_VALUE_") for name in os.environ):
        errors.append("Git configuration value injection environment is active")

    terms_override = os.environ.get("V1SIMPLE_PRIVACY_TERMS")
    terms_path = (
        Path(terms_override)
        if terms_override
        else Path.home() / ".config" / "v1simple" / "privacy_terms.txt"
    )
    try:
        mode = stat.S_IMODE(terms_path.stat().st_mode)
        if not terms_path.is_file() or mode & 0o077:
            errors.append("private term list is missing or not owner-only")
        elif active_private_terms(terms_path) == 0:
            errors.append("private term list has no active entries")
    except (OSError, RuntimeError):
        errors.append("private term list is missing or unreadable")

    checks = (
        [sys.executable, "scripts/check_public_commit_metadata.py", "--identity-only"],
        [sys.executable, "scripts/check_public_commit_metadata.py", "--revision=--all"],
        [sys.executable, "scripts/check_public_snapshot_privacy.py", "--index"],
        [sys.executable, "scripts/check_public_snapshot_privacy.py", "--all-history"],
        [sys.executable, "scripts/test_scanner_parity.py"],
    )
    for arguments in checks:
        completed = run(arguments)
        if completed.returncode != 0:
            errors.append(f"privacy validation failed: {Path(arguments[1]).name}")

    if errors:
        for error in errors:
            print(f"[local-privacy-setup] ERROR: {error}", file=sys.stderr)
        return 1

    print(
        "[local-privacy-setup] local identity, hooks, private terms, index, "
        "history, and push destination are safe"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
