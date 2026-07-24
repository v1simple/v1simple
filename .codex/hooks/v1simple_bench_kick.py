#!/usr/bin/env python3
"""Fail-open turn-end bridge to the optional private advisory bench."""

from __future__ import annotations

import hashlib
import hmac
import json
import os
from pathlib import Path
import subprocess
import sys


PUBLIC_REPO = Path(__file__).resolve().parents[2]
BENCH_SCRIPT = Path("scripts") / "bench.py"
MAX_INPUT_BYTES = 262_144


def _contract_matches() -> bool:
    if len(sys.argv) != 3 or sys.argv[1] != "--contract-sha256":
        return False
    actual = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    return hmac.compare_digest(sys.argv[2], actual)


def _private_companion() -> Path | None:
    candidates: list[Path] = []
    configured = os.environ.get("V1SIMPLE_INTERNAL_REPO")
    if configured:
        candidates.append(Path(configured).expanduser())
    candidates.append(PUBLIC_REPO.parent / "v1simple-internal")

    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved.is_dir() and (resolved / BENCH_SCRIPT).is_file():
            return resolved
    return None


def main() -> int:
    raw = sys.stdin.buffer.read(MAX_INPUT_BYTES + 1)
    if len(raw) > MAX_INPUT_BYTES or not _contract_matches():
        return 0
    try:
        payload = json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return 0
    if not isinstance(payload, dict) or payload.get("hook_event_name") != "Stop":
        return 0

    companion = _private_companion()
    if companion is None:
        return 0

    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    try:
        subprocess.Popen(
            [
                "python3",
                str(companion / BENCH_SCRIPT),
                "--fast",
                "--fail-fast",
                "--no-wait",
            ],
            cwd=companion,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True,
        )
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        # Advisory automation must never block the engineering turn.
        raise SystemExit(0)
