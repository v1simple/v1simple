#!/usr/bin/env python3
"""Validate a typed OBD/proxy hardware-qualification evidence artifact."""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "tools" / "obd_proxy_qualification_profile_v1.json"
FULL_GIT_SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")
UTC_TIMESTAMP_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", required=True, help="Path to qualification_result.json.")
    parser.add_argument(
        "--profile",
        default=str(DEFAULT_PROFILE),
        help="Qualification profile JSON (default: tools/obd_proxy_qualification_profile_v1.json).",
    )
    return parser.parse_args()


def load_json(path: Path, label: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"{label} not found: {path}") from exc
    except OSError as exc:
        raise ValueError(f"could not read {label} {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(
            f"{label} is not valid JSON: {path}:{exc.lineno}:{exc.colno}: {exc.msg}"
        ) from exc


def required_case_ids(profile: Any) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    if not isinstance(profile, dict):
        return [], ["profile root must be an object"]
    if type(profile.get("schema_version")) is not int or profile.get("schema_version") != 1:
        errors.append("profile.schema_version must be 1")
    if not isinstance(profile.get("profile_id"), str) or not profile["profile_id"].strip():
        errors.append("profile.profile_id is required")
    if type(profile.get("profile_version")) is not int or profile["profile_version"] < 1:
        errors.append("profile.profile_version must be a positive integer")

    cases = profile.get("required_cases")
    if not isinstance(cases, list) or not cases:
        errors.append("profile.required_cases must be a non-empty array")
        return [], errors

    ids: list[str] = []
    seen: set[str] = set()
    for index, case in enumerate(cases):
        prefix = f"profile.required_cases[{index}]"
        if not isinstance(case, dict):
            errors.append(f"{prefix} must be an object")
            continue
        case_id = case.get("id")
        if not isinstance(case_id, str) or not case_id.strip():
            errors.append(f"{prefix}.id is required")
            continue
        case_id = case_id.strip()
        if case_id in seen:
            errors.append(f"profile has duplicate required case id: {case_id}")
            continue
        seen.add(case_id)
        ids.append(case_id)
    return ids, errors


def parse_utc_timestamp(raw: Any, field: str) -> tuple[datetime | None, str | None]:
    if not isinstance(raw, str) or not raw.strip():
        return None, f"{field} is required"
    text = raw.strip()
    if not UTC_TIMESTAMP_RE.fullmatch(text):
        return None, f"{field} must be an RFC3339 UTC timestamp ending in Z"
    try:
        parsed = datetime.fromisoformat(text[:-1] + "+00:00")
    except ValueError:
        return None, f"{field} must be a valid RFC3339 UTC timestamp"
    if parsed.tzinfo is None or parsed.utcoffset() != timezone.utc.utcoffset(parsed):
        return None, f"{field} must be UTC"
    return parsed, None


def resolve_safe_evidence_log(artifact_path: Path, raw: Any) -> tuple[Path | None, str | None]:
    if not isinstance(raw, str) or not raw.strip():
        return None, "evidence_log is required"
    text = raw.strip()
    posix = PurePosixPath(text)
    windows = PureWindowsPath(text)
    if "\\" in text or posix.is_absolute() or windows.is_absolute() or windows.drive:
        return None, "evidence_log must be a relative POSIX path"
    if ".." in posix.parts:
        return None, "evidence_log must not contain path traversal"

    base = artifact_path.resolve().parent
    candidate = (base / Path(*posix.parts)).resolve()
    try:
        candidate.relative_to(base)
    except ValueError:
        return None, "evidence_log resolves outside the artifact directory"
    if not candidate.exists():
        return None, f"evidence_log does not exist: {candidate}"
    if not candidate.is_file():
        return None, f"evidence_log is not a file: {candidate}"
    try:
        if candidate.stat().st_size <= 0:
            return None, f"evidence_log is empty: {candidate}"
    except OSError as exc:
        return None, f"could not inspect evidence_log {candidate}: {exc}"
    return candidate, None


def validate_artifact(payload: Any, artifact_path: Path, profile: Any) -> list[str]:
    errors: list[str] = []
    required_ids, profile_errors = required_case_ids(profile)
    errors.extend(profile_errors)
    if not isinstance(payload, dict):
        return errors + ["artifact root must be an object"]
    if profile_errors:
        return errors

    if type(payload.get("schema_version")) is not int or payload.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    if payload.get("profile_id") != profile.get("profile_id"):
        errors.append(f"profile_id must be {profile.get('profile_id')}")
    if payload.get("profile_version") != profile.get("profile_version"):
        errors.append(f"profile_version must be {profile.get('profile_version')}")
    if payload.get("result") != "PASS":
        errors.append("result must be PASS")

    release_git_sha = payload.get("release_git_sha")
    if not isinstance(release_git_sha, str) or not FULL_GIT_SHA_RE.fullmatch(release_git_sha.strip()):
        errors.append("release_git_sha must be a full 40-character hexadecimal git SHA")

    for field in ("firmware_version", "dut_board_id", "rig_id"):
        value = payload.get(field)
        if not isinstance(value, str) or not value.strip():
            errors.append(f"{field} is required")

    started, started_error = parse_utc_timestamp(payload.get("started_at_utc"), "started_at_utc")
    completed, completed_error = parse_utc_timestamp(payload.get("completed_at_utc"), "completed_at_utc")
    if started_error:
        errors.append(started_error)
    if completed_error:
        errors.append(completed_error)
    if started is not None and completed is not None and completed < started:
        errors.append("completed_at_utc must not precede started_at_utc")

    safety = payload.get("safety")
    if not isinstance(safety, dict):
        errors.append("safety must be an object")
    else:
        for field in ("watchdog_resets", "panics"):
            value = safety.get(field)
            if type(value) is not int or value != 0:  # bool is intentionally not accepted as an integer.
                errors.append(f"safety.{field} must be integer 0")

    cases = payload.get("cases")
    if not isinstance(cases, list):
        errors.append("cases must be an array")
        return errors

    seen: set[str] = set()
    required_set = set(required_ids)
    for index, case in enumerate(cases):
        prefix = f"cases[{index}]"
        if not isinstance(case, dict):
            errors.append(f"{prefix} must be an object")
            continue
        case_id = case.get("id")
        if not isinstance(case_id, str) or not case_id.strip():
            errors.append(f"{prefix}.id is required")
            continue
        case_id = case_id.strip()
        if case_id in seen:
            errors.append(f"duplicate case id: {case_id}")
        seen.add(case_id)
        if case_id not in required_set:
            errors.append(f"unknown case id: {case_id}")
        if case.get("result") != "PASS":
            errors.append(f"{prefix}.result must be PASS")
        _, log_error = resolve_safe_evidence_log(artifact_path, case.get("evidence_log"))
        if log_error:
            errors.append(f"{prefix}.{log_error}")

    for missing in required_ids:
        if missing not in seen:
            errors.append(f"missing required case id: {missing}")
    return errors


def validate_artifact_file(artifact_path: Path, profile_path: Path = DEFAULT_PROFILE) -> list[str]:
    try:
        profile = load_json(profile_path, "profile")
        payload = load_json(artifact_path, "artifact")
    except ValueError as exc:
        return [str(exc)]
    return validate_artifact(payload, artifact_path, profile)


def main() -> int:
    args = parse_args()
    artifact_path = Path(args.artifact).expanduser()
    profile_path = Path(args.profile).expanduser()
    errors = validate_artifact_file(artifact_path, profile_path)
    if errors:
        print("[obd-proxy-qualification] validation failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print(f"[obd-proxy-qualification] artifact valid: {artifact_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
