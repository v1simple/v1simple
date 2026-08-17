#!/usr/bin/env python3
"""Run one command in its own process group while teeing and cleaning up."""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import BinaryIO


MANAGED_SHUTDOWN_GRACE_SECONDS = 70.0
MANAGED_V1_RADIO_LEASE_FD_ENV = "V1SIMPLE_MANAGED_V1_LEASE_FD"


def inherited_pass_fds() -> tuple[int, ...]:
    """Preserve the controller-owned radio lease through this wrapper."""
    raw = os.environ.get(MANAGED_V1_RADIO_LEASE_FD_ENV)
    if raw is None:
        return ()
    try:
        descriptor = int(raw, 10)
    except ValueError as exc:
        raise ValueError(
            f"{MANAGED_V1_RADIO_LEASE_FD_ENV} must be a canonical open descriptor"
        ) from exc
    if descriptor < 3 or raw != str(descriptor):
        raise ValueError(
            f"{MANAGED_V1_RADIO_LEASE_FD_ENV} must be a canonical open descriptor"
        )
    try:
        os.fstat(descriptor)
    except OSError as exc:
        raise ValueError(
            f"{MANAGED_V1_RADIO_LEASE_FD_ENV} does not name an open descriptor"
        ) from exc
    return (descriptor,)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdout", required=True)
    parser.add_argument("--stderr", required=True)
    parser.add_argument("--combined", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("a command is required after --")
    return args


def copy_stream(
    source: BinaryIO,
    terminal: BinaryIO,
    own_log: BinaryIO,
    combined_log: BinaryIO,
    combined_lock: threading.Lock,
) -> None:
    while True:
        chunk = source.readline()
        if not chunk:
            break
        terminal.write(chunk)
        terminal.flush()
        own_log.write(chunk)
        own_log.flush()
        with combined_lock:
            combined_log.write(chunk)
            combined_log.flush()


def main() -> int:
    args = parse_args()
    for raw_path in (args.stdout, args.stderr, args.combined):
        Path(raw_path).resolve().parent.mkdir(parents=True, exist_ok=True)

    interrupted_status = 0
    interrupted_at = 0.0
    termination_forwarded = False
    process: subprocess.Popen[bytes] | None = None

    def handle_signal(signum: int, _frame: object) -> None:
        nonlocal interrupted_status, interrupted_at, termination_forwarded
        if interrupted_status:
            return
        interrupted_status = 128 + signum
        interrupted_at = time.monotonic()
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                termination_forwarded = True
            except OSError:
                pass

    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, handle_signal)

    with (
        Path(args.stdout).open("wb") as stdout_log,
        Path(args.stderr).open("wb") as stderr_log,
        Path(args.combined).open("ab") as combined_log,
    ):
        try:
            pass_fds = inherited_pass_fds()
            process = subprocess.Popen(
                args.command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
                pass_fds=pass_fds,
            )
        except (OSError, ValueError) as exc:
            message = f"managed command could not start: {exc}\n".encode("utf-8", errors="replace")
            sys.stderr.buffer.write(message)
            stderr_log.write(message)
            combined_log.write(message)
            return 3

        if interrupted_status and not termination_forwarded and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                termination_forwarded = True
            except OSError:
                pass

        assert process.stdout is not None
        assert process.stderr is not None
        lock = threading.Lock()
        stdout_thread = threading.Thread(
            target=copy_stream,
            args=(process.stdout, sys.stdout.buffer, stdout_log, combined_log, lock),
            daemon=True,
        )
        stderr_thread = threading.Thread(
            target=copy_stream,
            args=(process.stderr, sys.stderr.buffer, stderr_log, combined_log, lock),
            daemon=True,
        )
        stdout_thread.start()
        stderr_thread.start()

        while process.poll() is None:
            if (
                interrupted_status
                and time.monotonic() - interrupted_at >= MANAGED_SHUTDOWN_GRACE_SECONDS
            ):
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except OSError:
                    pass
            try:
                process.wait(timeout=0.25)
            except subprocess.TimeoutExpired:
                continue

        stdout_thread.join(timeout=5)
        stderr_thread.join(timeout=5)

    return interrupted_status or int(process.returncode or 0)


if __name__ == "__main__":
    raise SystemExit(main())
