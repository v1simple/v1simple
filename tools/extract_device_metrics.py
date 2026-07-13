#!/usr/bin/env python3
"""Extract structured device metric lines from a suite log into NDJSON."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Extract structured device metrics from a PlatformIO suite log")
    parser.add_argument("log_path", help="Path to suite log")
    parser.add_argument("out_path", help="Path to append normalized NDJSON metrics")
    parser.add_argument("--run-id", default="", help="Run id to inject when the metric line leaves it empty")
    parser.add_argument("--git-sha", default="", help="Git sha to inject when the metric line leaves it empty")
    parser.add_argument("--suite", required=True, help="Suite name to inject when the metric line leaves it empty")
    return parser.parse_args()


def _normalize_metric(record: dict[str, Any], *, run_id: str, git_sha: str, suite: str) -> dict[str, Any]:
    if record.get("schema_version") is None:
        record["schema_version"] = 1
    if not record.get("run_id"):
        record["run_id"] = run_id
    if not record.get("git_sha"):
        record["git_sha"] = git_sha
    if not record.get("run_kind"):
        record["run_kind"] = "device_suite"
    if not record.get("suite_or_profile"):
        record["suite_or_profile"] = suite
    if not record.get("sample"):
        record["sample"] = "value"
    if not record.get("unit"):
        record["unit"] = "count"
    if not isinstance(record.get("tags"), dict):
        record["tags"] = {}

    value = record.get("value")
    if isinstance(value, bool):
        value = int(value)
    if not isinstance(value, (int, float)):
        raise ValueError("metric line has non-numeric value")
    record["value"] = float(value)

    for field in [
        "schema_version",
        "run_id",
        "git_sha",
        "run_kind",
        "suite_or_profile",
        "metric",
        "sample",
        "value",
        "unit",
        "tags",
    ]:
        if field not in record:
            raise ValueError(f"metric line missing '{field}'")

    return record


def main() -> int:
    args = parse_args()
    log_path = Path(args.log_path)
    out_path = Path(args.out_path)

    if not log_path.exists():
        print(f"ERROR: suite log not found: {log_path}", file=sys.stderr)
        return 2

    count = 0
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("r", encoding="utf-8", errors="replace") as handle, out_path.open(
        "a", encoding="utf-8"
    ) as out_handle:
        for lineno, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line.startswith("{") or "\"metric\"" not in line:
                continue
            try:
                parsed = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(parsed, dict):
                continue
            try:
                normalized = _normalize_metric(
                    parsed,
                    run_id=args.run_id,
                    git_sha=args.git_sha,
                    suite=args.suite,
                )
            except Exception as exc:
                print(f"ERROR: invalid metric line {lineno} in {log_path}: {exc}", file=sys.stderr)
                return 2
            out_handle.write(json.dumps(normalized, sort_keys=True))
            out_handle.write("\n")
            count += 1

    print(count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
