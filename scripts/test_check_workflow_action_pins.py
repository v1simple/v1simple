#!/usr/bin/env python3
"""Regression tests for immutable actions and declaration-driven setup wiring."""

from __future__ import annotations

import tempfile
from pathlib import Path

import check_workflow_action_pins as checker

ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_fixture(relative: str, source: str, bootstrap_source: str | None = None) -> list[str]:
    if bootstrap_source is None:
        bootstrap_source = (ROOT / "scripts/bootstrap_linux_validation.sh").read_text()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        path = root / relative
        path.parent.mkdir(parents=True)
        path.write_text(source)
        bootstrap = root / "scripts/bootstrap_linux_validation.sh"
        bootstrap.parent.mkdir(parents=True)
        bootstrap.write_text(bootstrap_source)
        original_root, original_bootstrap = checker.ROOT, checker.BOOTSTRAP
        try:
            checker.ROOT, checker.BOOTSTRAP = root, bootstrap
            return [*checker.check_workflow(path)[1],
                    *checker.check_reproducible_linux_contract(path),
                    *checker.check_bootstrap_contract()]
        finally:
            checker.ROOT, checker.BOOTSTRAP = original_root, original_bootstrap


def main() -> int:
    ci = (ROOT / ".github/workflows/ci.yml").read_text()
    release = (ROOT / ".github/workflows/release.yml").read_text()
    for name, source in (("ci.yml", ci), ("release.yml", release)):
        require(not check_fixture(".github/workflows/" + name, source), "valid declared workflow rejected")
    broken = release.replace("ubuntu-24.04", "ubuntu-latest").replace(
        checker.PIO_CACHE_HASH, "hashFiles('platformio.ini')").replace(
        "          key:", "          restore-keys: old-pio\n          key:")
    broken = broken.replace(checker.PYTHON_INPUT, "python-version: '3.11'").replace(
        checker.NODE_INPUT, "node-version: '22.23.1'")
    broken = broken.replace("./scripts/bootstrap_linux_validation.sh release", 'pip install "platformio==6.2.0"')
    errors = "\n".join(check_fixture(".github/workflows/release.yml", broken))
    for expected in ("must pin", "patch and verifier inputs", "must not restore", "Python setup", "Node.js setup",
                     "bootstrap invocation", "bootstrap owner"):
        require(expected in errors, "missing independent rejection: " + expected)
    for wrong in ("", "python3 -m missing_reader"):
        errors = check_fixture(".github/workflows/ci.yml", ci.replace(checker.HOST_OUTPUT_COMMAND, wrong))
        require(any("declaration read" in error for error in errors), "missing bootstrap read accepted")
    read_step = '''      - name: Read qualified host tools
        id: host_tools
        run: python3 scripts/host_tools.py workflow linux >> "$GITHUB_OUTPUT"

'''
    late = ci.replace(read_step, "").replace("      - name: Cache PlatformIO", read_step + "      - name: Cache PlatformIO")
    require(any("precede tool setup" in error for error in check_fixture(".github/workflows/ci.yml", late)),
            "reading desired Python after setup was accepted")
    mutable = ci.replace("actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1", "actions/checkout@main")
    require(any("40-character" in error for error in check_fixture(".github/workflows/ci.yml", mutable)),
            "mutable action accepted")
    bootstrap = (ROOT / "scripts/bootstrap_linux_validation.sh").read_text()
    for changed, expected in (
        (bootstrap.replace('package_lines="$(host_tool packages "$PROFILE")"', 'package_lines="pioarduino==6.2.0"'), "profile package list"),
        (bootstrap.replace("for package in ffmpeg libpcre3", "for package in ffmpeg"), "libpcre3"),
        (bootstrap + '\npython3 -m pip install "platformio==6.2.0"\n', "overlapping platformio"),
    ):
        require(any(expected in error for error in check_fixture(".github/workflows/ci.yml", ci, changed)),
                "invalid bootstrap accepted: " + expected)
    print("workflow action and declaration contract tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
