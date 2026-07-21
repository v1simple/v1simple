#!/usr/bin/env python3
"""Read-only bridge from Codex lifecycle hooks to optional private context."""

from __future__ import annotations

import hashlib
import hmac
import json
import os
from pathlib import Path
import subprocess
import sys


PUBLIC_REPO = Path(__file__).resolve().parents[2]
CONTEXT_SCRIPT = Path("scripts") / "v1i.py"
EVENT_MAP = {
    "SessionStart": "session",
    "UserPromptSubmit": "prompt",
    "SubagentStart": "subagent",
}
MAX_CONTEXT_BYTES = 8_000
CONTEXT_TIMEOUT_SECONDS = 4
DEGRADED = (
    "[v1simple-context] Private engineering context is unavailable; "
    "continue with the standalone public repository and keep this limitation visible."
)


def _private_companion() -> Path | None:
    candidates: list[Path] = []
    configured = os.environ.get("V1SIMPLE_INTERNAL_REPO")
    if configured:
        candidates.append(Path(configured).expanduser())
    candidates.append(PUBLIC_REPO.parent / "v1simple-internal")

    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved.is_dir() and (resolved / CONTEXT_SCRIPT).is_file():
            return resolved
    return None


def _contract_matches() -> bool:
    if len(sys.argv) != 3 or sys.argv[1] != "--contract-sha256":
        return False
    actual = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    return hmac.compare_digest(sys.argv[2], actual)


def _compact_context(output: bytes) -> str:
    suffix = b"\n[v1simple-context] Context truncated by the public safety limit."
    if len(output) > MAX_CONTEXT_BYTES:
        budget = MAX_CONTEXT_BYTES - len(suffix)
        output = output[:budget].rstrip() + suffix
    return output.decode("utf-8", errors="replace").strip()


def _emit(event_name: str, context: str) -> None:
    output = {
        "hookSpecificOutput": {
            "hookEventName": event_name,
            "additionalContext": context,
        }
    }
    sys.stdout.write(json.dumps(output, ensure_ascii=False, separators=(",", ":")) + "\n")


def _emit_common_warning(context: str) -> None:
    sys.stdout.write(json.dumps({"continue": True, "systemMessage": context}, separators=(",", ":")) + "\n")


def main() -> int:
    hook_input = sys.stdin.buffer.read()
    try:
        payload = json.loads(hook_input)
    except (json.JSONDecodeError, UnicodeDecodeError):
        _emit_common_warning(DEGRADED + " Hook input was invalid.")
        return 0

    if not isinstance(payload, dict):
        _emit_common_warning(DEGRADED + " Hook input was not an object.")
        return 0

    hook_event = str(payload.get("hook_event_name", ""))
    event = EVENT_MAP.get(hook_event)
    if event is None:
        _emit_common_warning(DEGRADED + " Hook event was unsupported.")
        return 0

    if not _contract_matches():
        _emit(hook_event, DEGRADED + " The configured bridge digest is stale.")
        return 0

    companion = _private_companion()
    if companion is None:
        _emit(hook_event, DEGRADED)
        return 0

    command = [
        "python3",
        str(companion / CONTEXT_SCRIPT),
        "context",
        "--event",
        event,
        "--public-repo",
        str(PUBLIC_REPO),
    ]
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"

    try:
        result = subprocess.run(
            command,
            input=hook_input,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            cwd=companion,
            env=environment,
            check=False,
            timeout=CONTEXT_TIMEOUT_SECONDS,
            shell=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        _emit(hook_event, DEGRADED + " The context command could not complete.")
        return 0

    if result.returncode != 0:
        _emit(hook_event, DEGRADED + " The context command did not succeed.")
        return 0

    context = _compact_context(result.stdout)
    _emit(hook_event, context if context else DEGRADED + " The context command returned no guidance.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        # Hooks must never block public engineering work or expose private errors.
        _emit_common_warning(DEGRADED + " The bridge encountered an unexpected error.")
        raise SystemExit(0)
