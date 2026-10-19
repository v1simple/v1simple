#!/usr/bin/env python3
"""Check the selected PlatformIO Python and prepare packages before web deployment."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys


LITTLEFS_VERSION = "0.18.0"


def prepare(pio: str, environment: str) -> None:
    result = subprocess.run(
        [pio, "system", "info", "--json-output"],
        check=True, text=True, capture_output=True,
    )
    info = json.loads(result.stdout)
    python = info.get("python_exe", {}).get("value")
    if not isinstance(python, str) or not python:
        raise ValueError("PlatformIO did not report its Python executable")
    probe = subprocess.run(
        [python, "-c", "from importlib.metadata import version; "
         "from littlefs import LittleFS; from littlefs.errors import LittleFSError; "
         "print(version('littlefs-python'))"],
        text=True, capture_output=True, check=False,
    )
    if probe.returncode != 0 or probe.stdout.strip() != LITTLEFS_VERSION:
        command = shlex.join([python, "-m", "pip", "install", f"littlefs-python=={LITTLEFS_VERSION}"])
        raise ValueError(
            f"The selected PlatformIO Python requires littlefs-python=={LITTLEFS_VERSION}.\n"
            f"Install it in that interpreter before building:\n  {command}"
        )
    # Deployment reads the platform builder and framework SDK configuration.
    # A Core-only installation does not contain either until project packages
    # have been prepared for the requested environment.
    subprocess.run([pio, "pkg", "install", "-e", environment], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pio", default="pio")
    parser.add_argument("--env", default="waveshare-349")
    args = parser.parse_args()
    try:
        prepare(args.pio, args.env)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"[web-build] preparation failed: {exc}", file=sys.stderr)
        return 1
    print("[web-build] LittleFS Python support and PlatformIO project packages are ready")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
