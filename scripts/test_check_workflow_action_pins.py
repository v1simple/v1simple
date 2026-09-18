#!/usr/bin/env python3
"""Regression tests for immutable action, runner, and PlatformIO cache checks."""

from __future__ import annotations

import tempfile
from pathlib import Path

import check_workflow_action_pins as checker


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_fixture(relative: str, source: str) -> list[str]:
    with tempfile.TemporaryDirectory() as temp_dir:
        root = Path(temp_dir)
        path = root / relative
        path.parent.mkdir(parents=True)
        path.write_text(source, encoding="utf-8")
        original_root = checker.ROOT
        try:
            checker.ROOT = root
            return checker.check_reproducible_linux_contract(path)
        finally:
            checker.ROOT = original_root


def test_accepts_pinned_runner_and_exact_cache_key() -> None:
    errors = check_fixture(
        ".github/workflows/ci.yml",
        f"""jobs:
  test:
    runs-on: {checker.PINNED_LINUX_RUNNER}
    steps:
      - with:
          {checker.PINNED_PYTHON}
      - name: Cache PlatformIO
        with:
          key: ${{{{ runner.os }}}}-pio-${{{{ {checker.PIO_CACHE_HASH} }}}}
      - run: pip install {checker.PIOARDUINO_CORE_PIN}
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
    )
    joined = "\n".join(errors)
    require("must pin" in joined, f"mutable runner was accepted: {errors}")
    require("patch and verifier inputs" in joined, f"weak cache key was accepted: {errors}")
    require("must not restore" in joined, f"broad cache restore was accepted: {errors}")
    require("must pin pioarduino" in joined, f"missing pioarduino pin was accepted: {errors}")
    require("overlapping platformio" in joined, f"overlapping core packages were accepted: {errors}")
    require("Python 3.12" in joined, f"Python version drift was accepted: {errors}")


def main() -> int:
    test_accepts_pinned_runner_and_exact_cache_key()
    test_rejects_mutable_runner_and_broad_cache_restore()
    print("workflow action and runner contract tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
