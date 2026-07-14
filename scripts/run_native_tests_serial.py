#!/usr/bin/env python3
"""Run PlatformIO native test suites one at a time with a clean build dir.

This avoids aggregate `pio test -e native` cross-suite artifact reuse, which can
surface stale binaries, missing `program` artifacts, or sporadic SIGKILLs that
do not reproduce when the same suite is run in isolation.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from native_test_source_manifest import (
    LINKED_NATIVE_TEST_PILOT_ENV,
    linked_test_names,
    validate_manifest,
)

ROOT = Path(__file__).resolve().parents[1]
TEST_ROOT = ROOT / "test"
BUILD_ROOT = ROOT / ".artifacts" / "serial_test_builds"

VALID_ENVS = {"native", "native-car", "native_car", "native-sanitized"}


def discover_native_tests(env: str) -> list[str]:
    if env == "native_car":
        return sorted(
            path.name
            for path in TEST_ROOT.iterdir()
            if path.is_dir() and path.name.startswith("test_car_mode_")
        )
    ignore_prefixes = ("test_device_", "test_car_mode_")
    return sorted(
        path.name
        for path in TEST_ROOT.iterdir()
        if path.is_dir()
        and path.name.startswith("test_")
        and not path.name.startswith(ignore_prefixes)
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--env",
        default="native",
        choices=sorted(VALID_ENVS),
        help="PlatformIO test environment (default: native).",
    )
    parser.add_argument(
        "--linked-pilot",
        action="store_true",
        help=(
            "Compile allow-listed production sources separately for the focused "
            "linked-test pilot."
        ),
    )
    parser.add_argument(
        "tests",
        nargs="*",
        help="Optional native test directory names (for example: test_display_pipeline_module).",
    )
    return parser.parse_args()


def suite_build_root(env: str, test_name: str, linked_pilot: bool = False) -> Path:
    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", test_name)
    if linked_pilot:
        return BUILD_ROOT / "linked-pilot" / env / safe_name
    return BUILD_ROOT / env / safe_name


def remove_tree(path: Path) -> None:
    if not path.exists():
        return

    def _onerror(_func, _target, exc_info):
        _, exc, _ = exc_info
        if isinstance(exc, FileNotFoundError):
            return
        raise exc

    shutil.rmtree(path, onerror=_onerror)


def main() -> int:
    args = parse_args()
    env = "native_car" if args.env == "native-car" else args.env
    available = discover_native_tests(env)
    if args.linked_pilot:
        if env not in {"native", "native-sanitized"}:
            print(
                "[linked-pilot] only native and native-sanitized are supported",
                file=sys.stderr,
            )
            return 2
        try:
            validate_manifest(ROOT)
        except ValueError as exc:
            print(f"[linked-pilot] invalid source manifest: {exc}", file=sys.stderr)
            return 2
        selected = args.tests or list(linked_test_names())
    else:
        selected = args.tests or available

    unknown = sorted(set(selected) - set(available))
    if unknown:
        print(f"[{env}-serial] unknown test suite(s):")
        for name in unknown:
            print(f"  - {name}")
        return 2

    if args.linked_pilot:
        unsupported = sorted(set(selected) - set(linked_test_names()))
        if unsupported:
            print("[linked-pilot] suite(s) are not in the exact source allow-list:")
            for name in unsupported:
                print(f"  - {name}")
            return 2

    failures: list[tuple[str, int]] = []

    for index, test_name in enumerate(selected, start=1):
        build_root = suite_build_root(env, test_name, args.linked_pilot)
        remove_tree(build_root)

        child_env = os.environ.copy()
        child_env["PLATFORMIO_BUILD_DIR"] = str(build_root)
        child_env.pop(LINKED_NATIVE_TEST_PILOT_ENV, None)
        if args.linked_pilot:
            child_env[LINKED_NATIVE_TEST_PILOT_ENV] = "1"

        mode = "linked-pilot" if args.linked_pilot else "serial"
        print(
            f"[{env}-{mode}] ({index}/{len(selected)}) running {test_name} "
            f"with build root {build_root}"
        )
        result = subprocess.run(
            ["pio", "test", "-e", env, "-f", test_name],
            cwd=ROOT,
            env=child_env,
        )
        if result.returncode != 0:
            failures.append((test_name, result.returncode))

    if failures:
        mode = "linked-pilot" if args.linked_pilot else "serial"
        print(f"[{env}-{mode}] failed suite(s):")
        for test_name, returncode in failures:
            print(f"  - {test_name} (exit {returncode})")
        return 1

    mode = "linked-pilot" if args.linked_pilot else "serial"
    print(f"[{env}-{mode}] all {len(selected)} suite(s) passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
