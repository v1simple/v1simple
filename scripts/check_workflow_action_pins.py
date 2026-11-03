#!/usr/bin/env python3
"""Require immutable full-SHA refs for external GitHub workflow actions."""

from __future__ import annotations

import re
import sys
from pathlib import Path

import host_tools

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS_DIR = ROOT / ".github" / "workflows"
USES_LINE_RE = re.compile(
    r"^\s*(?:-\s*)?uses\s*:\s*(?P<value>[^#]+?)(?:\s+#.*)?$"
)
USES_PREFIX_RE = re.compile(r"^(?:-\s*)?uses\s*:")
PINNED_ACTION_RE = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")
HOST_OUTPUT_COMMAND = 'python3 scripts/host_tools.py workflow linux >> "$GITHUB_OUTPUT"'
PYTHON_INPUT = "python-version: ${{ steps.host_tools.outputs.python }}"
NODE_INPUT = "node-version: ${{ steps.host_tools.outputs.node }}"
PIO_CACHE_HASH = (
    "hashFiles('platformio.ini', 'scripts/patch_*.py', "
    "'scripts/verify_esp32s3_framework.py', "
    "'scripts/check_platformio_core_version.py', 'config/host_tools.json', 'scripts/host_tools.py', 'scripts/build_context.py')"
)
CPP_CHECK_RUNTIME = "libpcre3"
BOOTSTRAP = ROOT / "scripts" / "bootstrap_linux_validation.sh"


def workflow_files() -> list[Path]:
    return sorted((*WORKFLOWS_DIR.glob("*.yml"), *WORKFLOWS_DIR.glob("*.yaml")))


def parse_uses_value(raw: str) -> str:
    value = raw.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        value = value[1:-1].strip()
    return value


def check_workflow(path: Path) -> tuple[int, list[str]]:
    errors: list[str] = []
    uses_count = 0
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            continue
        if not USES_PREFIX_RE.match(stripped):
            continue

        match = USES_LINE_RE.fullmatch(line)
        if not match:
            errors.append(f"{path.relative_to(ROOT)}:{line_number}: could not parse uses entry")
            continue

        uses_count += 1
        value = parse_uses_value(match.group("value"))
        if value.startswith("./"):
            continue
        if not PINNED_ACTION_RE.fullmatch(value):
            errors.append(
                f"{path.relative_to(ROOT)}:{line_number}: external action must use a "
                f"40-character lowercase commit SHA: {value}"
            )
    return uses_count, errors


def check_reproducible_linux_contract(path: Path) -> list[str]:
    """Keep CI/release runners and mutated PlatformIO caches deterministic."""
    relative = path.relative_to(ROOT).as_posix()
    if relative not in {".github/workflows/ci.yml", ".github/workflows/release.yml"}:
        return []

    text = path.read_text(encoding="utf-8")
    errors: list[str] = []
    linux_runner = host_tools.load().get("linux_runner")
    if "ubuntu-latest" in text:
        errors.append(f"{relative}: Linux jobs must pin {linux_runner}")

    for declaration, label in ((PYTHON_INPUT, "Python"), (NODE_INPUT, "Node.js")):
        if text.count(declaration) != 1:
            errors.append(f"{relative}: {label} setup must consume the shared host-tools output")
    if text.count(HOST_OUTPUT_COMMAND) != 1:
        errors.append(f"{relative}: expected one bootstrap-Python host-tools read")
    for job in re.split(r"(?m)^  [A-Za-z0-9_-]+:\s*$", text):
        if "actions/setup-node@" not in job and "actions/setup-python@" not in job:
            continue
        read_at = job.find(HOST_OUTPUT_COMMAND)
        checkout_at = job.find("actions/checkout@")
        setup_positions = [job.find(action) for action in ("actions/setup-node@", "actions/setup-python@")]
        if (checkout_at < 0 or read_at < checkout_at or "id: host_tools" not in job
                or any(position < read_at for position in setup_positions if position >= 0)):
            errors.append(f"{relative}: checkout and base-Python declaration read must precede tool setup")
    if relative.endswith("/ci.yml"):
        mac_read = 'python3 scripts/host_tools.py workflow macos >> "$GITHUB_OUTPUT"'
        mac_input = "DEVELOPER_DIR: ${{ steps.host_tools.outputs.macos_developer_dir }}"
        if text.count(mac_read) != 1 or text.count(mac_input) != 4:
            errors.append(f"{relative}: macOS jobs must consume the declared Xcode selection")
        elif text.index(mac_read) > text.index(mac_input):
            errors.append(f"{relative}: read the Xcode selection before using it")

    profile = "ci" if relative.endswith("/ci.yml") else "release"
    bootstrap_call = f"./scripts/bootstrap_linux_validation.sh {profile}"
    if text.count(bootstrap_call) != 1:
        errors.append(f"{relative}: expected one {profile} bootstrap invocation")
    if "pip install" in text or "apt-get install" in text:
        errors.append(f"{relative}: dependency installation must stay in the bootstrap owner")

    expected_runner_count = 1 if relative.endswith("/ci.yml") else 2
    actual_runner_count = text.count(f"runs-on: {linux_runner}")
    if actual_runner_count != expected_runner_count:
        errors.append(
            f"{relative}: expected {expected_runner_count} pinned Linux runners, "
            f"found {actual_runner_count}"
        )

    if "name: Cache PlatformIO" in text:
        if PIO_CACHE_HASH not in text:
            errors.append(
                f"{relative}: PlatformIO cache key must cover dependency patch and verifier inputs"
            )
        if "restore-keys:" in text:
            errors.append(
                f"{relative}: PlatformIO cache must not restore a differently patched package state"
            )
    return errors


def check_bootstrap_contract() -> list[str]:
    if not BOOTSTRAP.is_file():
        return ["scripts/bootstrap_linux_validation.sh: shared bootstrap is missing"]
    text = BOOTSTRAP.read_text(encoding="utf-8")
    errors: list[str] = []
    if text.count('package_lines="$(host_tool packages "$PROFILE")"') != 1:
        errors.append("scripts/bootstrap_linux_validation.sh: install the declared profile package list")
    if 'host_tool() { "$PYTHON_CMD" "$HOST_TOOLS" "$@"; }' not in text:
        errors.append("scripts/bootstrap_linux_validation.sh: read tools using the available bootstrap Python")
    if "platformio==" in text:
        errors.append(
            "scripts/bootstrap_linux_validation.sh: overlapping platformio package is forbidden"
        )
    if text.count(CPP_CHECK_RUNTIME) != 1:
        errors.append(
            f"scripts/bootstrap_linux_validation.sh: install {CPP_CHECK_RUNTIME} exactly once"
        )
    for key in ("node", "python", "swift", "swift_sha256", "shellcheck"):
        if f"$(host_tool get {key})" not in text:
            errors.append(f"scripts/bootstrap_linux_validation.sh: missing declared {key} requirement")
    for tool in ("swift", "shellcheck"):
        if f'host_tool requires "$PROFILE" {tool}' not in text:
            errors.append(f"scripts/bootstrap_linux_validation.sh: {tool} must follow profile membership")
    return errors


def main() -> int:
    paths = workflow_files()
    if not paths:
        print(f"[workflow-pins] no workflow files found under {WORKFLOWS_DIR}")
        return 1

    errors: list[str] = []
    total_uses = 0
    for path in paths:
        uses_count, path_errors = check_workflow(path)
        total_uses += uses_count
        errors.extend(path_errors)
        errors.extend(check_reproducible_linux_contract(path))
    errors.extend(check_bootstrap_contract())

    if errors:
        print("[workflow-pins] immutable workflow contract failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(
        f"[workflow-pins] {total_uses} action references and reproducible Linux "
        f"contracts validated across {len(paths)} workflow files"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
