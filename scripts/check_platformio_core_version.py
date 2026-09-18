#!/usr/bin/env python3
"""Fail early unless the active PlatformIO Core matches the release pin."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

REQUIRED_VERSION = (6, 1, 19)
REQUIRED_DISTRIBUTION = "pioarduino"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pio", default="pio", help="PlatformIO executable to check")
    parser.add_argument(
        "--required-version",
        default=".".join(str(part) for part in REQUIRED_VERSION),
        help="Exact required PlatformIO Core version",
    )
    return parser.parse_args()


def parse_version(raw: str) -> tuple[int, ...] | None:
    match = re.search(
        r"(?<![0-9A-Za-z])(\d+)\.(\d+)\.(\d+)(?![0-9A-Za-z.+-])",
        raw,
    )
    if match is None:
        return None
    return tuple(int(part) for part in match.groups())


def format_version(version: tuple[int, ...]) -> str:
    return ".".join(str(part) for part in version)


def pio_interpreter(pio: str) -> str | None:
    executable = shutil.which(pio) if "/" not in pio else pio
    if not executable:
        return None
    try:
        first_line = Path(executable).read_text(encoding="utf-8").splitlines()[0]
        command = shlex.split(first_line.removeprefix("#!"))
    except (OSError, IndexError, ValueError):
        return None
    if not first_line.startswith("#!") or not command:
        return None
    if Path(command[0]).name == "env":
        if len(command) != 2:
            return None
        return shutil.which(command[1])
    return command[0]


def installed_pioarduino_version(pio: str) -> tuple[int, ...] | None:
    interpreter = pio_interpreter(pio)
    if interpreter is None:
        return None
    probe = subprocess.run(
        [
            interpreter,
            "-c",
            (
                "from importlib.metadata import version; "
                f"print(version({REQUIRED_DISTRIBUTION!r}))"
            ),
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if probe.returncode != 0:
        return None
    return parse_version(probe.stdout.strip())


def main() -> int:
    args = parse_args()
    required_version = parse_version(args.required_version)
    if required_version is None:
        print(f"[toolchain] invalid required version: {args.required_version}", file=sys.stderr)
        return 2

    try:
        proc = subprocess.run(
            [args.pio, "--version"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except OSError as exc:
        print(f"[toolchain] failed to execute {args.pio!r}: {exc}", file=sys.stderr)
        return 1

    output = proc.stdout.strip()
    version = parse_version(output)
    if proc.returncode != 0 or version is None:
        print(f"[toolchain] unable to parse PlatformIO Core version from: {output}", file=sys.stderr)
        return 1

    if version == required_version:
        distribution_version = installed_pioarduino_version(args.pio)
        if distribution_version == required_version:
            print(
                f"[toolchain] pioarduino Core {format_version(version)} "
                "matches the release pin"
            )
            return 0
        found = (
            format_version(distribution_version)
            if distribution_version is not None
            else "missing or unreadable"
        )
        print(
            f"[toolchain] {REQUIRED_DISTRIBUTION} distribution is {found}; "
            f"required {format_version(required_version)}.",
            file=sys.stderr,
        )
        return 1

    print(
        f"[toolchain] PlatformIO Core {format_version(version)} does not match "
        f"the required release pin {format_version(required_version)}.",
        file=sys.stderr,
    )
    print(
        "[toolchain] Install the exact release toolchain, for example:\n"
        "  python3 -m pip install 'pioarduino==6.1.19'\n"
        "\n"
        "[toolchain] Or use an isolated repo-local toolchain:\n"
        "  python3 -m venv .artifacts/pio-core-6.1.19\n"
        "  .artifacts/pio-core-6.1.19/bin/python -m pip install --upgrade pip setuptools wheel\n"
        "  .artifacts/pio-core-6.1.19/bin/python -m pip install 'pioarduino==6.1.19'\n"
        "  export PIO_CMD=\"$PWD/.artifacts/pio-core-6.1.19/bin/pio\"\n"
        "\n"
        "[toolchain] Repo scripts automatically export certifi's CA bundle for PlatformIO TLS downloads.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
