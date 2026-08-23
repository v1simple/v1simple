#!/usr/bin/env python3
"""Summarize panic endpoint JSONL for the perf CSV importer."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any


def parse_panic_jsonl(path: Path) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "samples": 0,
        "ok_samples": 0,
        "was_crash_true": 0,
        "has_panic_file_true": 0,
        "first_was_crash": None,
        "last_was_crash": None,
        "first_has_panic_file": None,
        "last_has_panic_file": None,
        "first_reset_reason": "",
        "last_reset_reason": "",
        "state_change_count": 0,
    }
    previous: tuple[int, int, str] | None = None

    try:
        lines = path.open("r", encoding="utf-8")
    except FileNotFoundError:
        return summary

    with lines:
        for raw in lines:
            line = raw.strip()
            if not line:
                continue
            summary["samples"] += 1
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            data = record.get("data") if isinstance(record, dict) and record.get("ok") else None
            if not isinstance(data, dict):
                continue

            summary["ok_samples"] += 1
            was_crash = int(data.get("wasCrash") is True)
            has_panic_file = int(data.get("hasPanicFile") is True)
            reset_reason = data.get("lastResetReason")
            state = (
                was_crash,
                has_panic_file,
                reset_reason if isinstance(reset_reason, str) else "",
            )
            summary["was_crash_true"] += was_crash
            summary["has_panic_file_true"] += has_panic_file
            if previous is None:
                summary["first_was_crash"] = was_crash
                summary["first_has_panic_file"] = has_panic_file
                summary["first_reset_reason"] = state[2]
            elif state != previous:
                summary["state_change_count"] += 1
            previous = state
            summary["last_was_crash"] = was_crash
            summary["last_has_panic_file"] = has_panic_file
            summary["last_reset_reason"] = state[2]

    return summary


def runtime_crash_detected(summary: dict[str, Any]) -> bool:
    first = summary.get("first_was_crash")
    last = summary.get("last_was_crash")
    changes = summary.get("state_change_count")
    return (
        isinstance(first, int)
        and isinstance(last, int)
        and isinstance(changes, int)
        and last == 1
        and (first == 0 or changes > 0)
    )


def preexisting_crash_state(summary: dict[str, Any]) -> bool:
    return (
        summary.get("first_was_crash") == 1
        and summary.get("last_was_crash") == 1
        and summary.get("state_change_count") == 0
    )


def render_kv(summary: dict[str, Any]) -> str:
    return "".join(
        f"{key}={'' if value is None else value}\n"
        for key, value in summary.items()
    )


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print("usage: soak_parse_panic.py <panic_jsonl>", file=sys.stderr)
        return 2
    print(render_kv(parse_panic_jsonl(Path(args[0]))), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
