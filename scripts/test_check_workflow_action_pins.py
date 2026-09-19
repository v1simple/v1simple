#!/usr/bin/env python3
"""Regression tests for immutable action, runner, and PlatformIO cache checks."""

from __future__ import annotations

import tempfile
from pathlib import Path

import check_workflow_action_pins as checker


VALID_BOOTSTRAP = f'''#!/bin/bash
NODE_VERSION="22.23.2"
COMMON=(
  {checker.PIOARDUINO_CORE_PIN}
)
apt-get install {checker.CPP_CHECK_RUNTIME}
'''


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_fixture(
    relative: str,
    source: str,
    bootstrap_source: str = VALID_BOOTSTRAP,
) -> list[str]:
    with tempfile.TemporaryDirectory() as temp_dir:
        root = Path(temp_dir)
        path = root / relative
        path.parent.mkdir(parents=True)
        path.write_text(source, encoding="utf-8")
        bootstrap = root / "scripts" / "bootstrap_linux_validation.sh"
        bootstrap.parent.mkdir(parents=True)
        bootstrap.write_text(bootstrap_source, encoding="utf-8")
        original_root = checker.ROOT
        original_bootstrap = checker.BOOTSTRAP
        try:
            checker.ROOT = root
            checker.BOOTSTRAP = bootstrap
            return [
                *checker.check_reproducible_linux_contract(path),
                *checker.check_bootstrap_contract(),
            ]
        finally:
            checker.ROOT = original_root
            checker.BOOTSTRAP = original_bootstrap


def test_accepts_pinned_runner_and_exact_cache_key() -> None:
    errors = check_fixture(
        ".github/workflows/ci.yml",
        f"""jobs:
  test:
    runs-on: {checker.PINNED_LINUX_RUNNER}
    steps:
      - with:
          {checker.PINNED_PYTHON}
          {checker.PINNED_NODE}
      - name: Cache PlatformIO
        with:
          key: ${{{{ runner.os }}}}-pio-${{{{ {checker.PIO_CACHE_HASH} }}}}
      - run: ./scripts/bootstrap_linux_validation.sh ci
""",
    )
    require(not errors, f"valid workflow rejected: {errors}")


def test_rejects_mutable_runner_and_broad_cache_restore() -> None:
    errors = check_fixture(
        ".github/workflows/release.yml",
        """jobs:
  release:
    runs-on: ubuntu-latest
    steps:
      - name: Cache PlatformIO
        with:
          key: ${{ runner.os }}-pio-${{ hashFiles('platformio.ini') }}
          restore-keys: ${{ runner.os }}-pio-
      - run: pip install "platformio==6.1.19"
  deploy-pages:
    runs-on: ubuntu-latest
""",
        bootstrap_source=f'''#!/bin/bash
NODE_VERSION="22.23.2"
apt-get install {checker.CPP_CHECK_RUNTIME}
python3 -m pip install "platformio==6.1.19"
''',
    )
    joined = "\n".join(errors)
    require("must pin" in joined, f"mutable runner was accepted: {errors}")
    require("patch and verifier inputs" in joined, f"weak cache key was accepted: {errors}")
    require("must not restore" in joined, f"broad cache restore was accepted: {errors}")
    require(
        "expected one pioarduino" in joined,
        f"missing pioarduino pin was accepted: {errors}",
    )
    require(
        "overlapping platformio" in joined,
        f"overlapping core packages were accepted: {errors}",
    )
    require("Python 3.12" in joined, f"Python version drift was accepted: {errors}")
    require("Node.js 22.23.2" in joined, f"Node version drift was accepted: {errors}")
    require("bootstrap invocation" in joined, f"missing bootstrap was accepted: {errors}")
    require("bootstrap owner" in joined, f"inline install was accepted: {errors}")


def test_rejects_missing_cppcheck_runtime() -> None:
    errors = check_fixture(
        ".github/workflows/ci.yml",
        f"""jobs:
  test:
    runs-on: {checker.PINNED_LINUX_RUNNER}
    steps:
      - with:
          {checker.PINNED_PYTHON}
          {checker.PINNED_NODE}
      - name: Cache PlatformIO
        with:
          key: ${{{{ runner.os }}}}-pio-${{{{ {checker.PIO_CACHE_HASH} }}}}
      - run: ./scripts/bootstrap_linux_validation.sh ci
""",
        bootstrap_source=f'''#!/bin/bash
NODE_VERSION="22.23.2"
COMMON=(
  {checker.PIOARDUINO_CORE_PIN}
)
''',
    )
    require(
        any(checker.CPP_CHECK_RUNTIME in error for error in errors),
        f"missing cppcheck runtime was accepted: {errors}",
    )


def main() -> int:
    test_accepts_pinned_runner_and_exact_cache_key()
    test_rejects_mutable_runner_and_broad_cache_restore()
    test_rejects_missing_cppcheck_runtime()
    print("workflow action and runner contract tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
