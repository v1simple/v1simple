#!/usr/bin/env python3
"""Validate a local release hardware-evidence manifest.

This is intentionally not part of normal PR CI because the artifacts are local
and hardware-lab specific. Run it during release prep after `./bench.sh` and any
required OBD/proxy/arbitration hardware pass or accepted-risk waiver.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import check_obd_proxy_qualification as obd_proxy_qualification

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / ".artifacts" / "release_evidence" / "manifest.json"
ALLOWED_RESULTS = {
    "PASS",
    "WARN",
    "FAIL",
    "COLLECTION_FAILED",
    "NO_BASELINE",
    "ACCEPTED_RISK",
}
CORE_BENCH_ID = "core-display-bench"
OBD_PROXY_ID = "obd-proxy-arbitration"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        default=str(DEFAULT_MANIFEST),
        help="Path to release evidence manifest JSON (default: .artifacts/release_evidence/manifest.json).",
    )
    parser.add_argument(
        "--allow-bench-warn",
        action="store_true",
        help="Allow WARN for core-display-bench after human investigation is documented.",
    )
    return parser.parse_args()


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"manifest not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"manifest is not valid JSON: {path}:{exc.lineno}:{exc.colno}: {exc.msg}") from exc


def resolve_artifact_path(raw: str) -> Path:
    path = Path(raw)
    return path if path.is_absolute() else (ROOT / path)


def validate_bench_result_file(path: Path, declared_result: str) -> list[str]:
    errors: list[str] = []
    if path.name != "bench_result.json":
        return errors
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 - report file parse detail without crashing validation
        return [f"{path}: could not parse bench_result.json: {exc}"]

    actual = str(payload.get("result", "")).strip()
    if actual and actual != declared_result:
        errors.append(
            f"{path}: manifest result {declared_result} does not match bench_result.json result {actual}"
        )
    return errors


def has_nonempty_scope(value: Any) -> bool:
    if isinstance(value, str):
        return bool(value.strip())
    if isinstance(value, list):
        return bool(value) and all(isinstance(item, str) and item.strip() for item in value)
    return False


def validate_manifest(payload: Any, *, allow_bench_warn: bool = False) -> list[str]:
    errors: list[str] = []
    if not isinstance(payload, dict):
        return ["manifest root must be an object"]

    if payload.get("schema_version") != 1:
        errors.append("schema_version must be 1")

    evidence = payload.get("evidence")
    if not isinstance(evidence, list) or not evidence:
        errors.append("evidence must be a non-empty array")
        return errors

    seen_ids: set[str] = set()
    core_seen = False
    obd_proxy_seen = False
    for index, item in enumerate(evidence):
        prefix = f"evidence[{index}]"
        if not isinstance(item, dict):
            errors.append(f"{prefix} must be an object")
            continue

        evidence_id = str(item.get("id", "")).strip()
        kind = str(item.get("kind", "")).strip()
        result = str(item.get("result", "")).strip()
        artifact_raw = str(item.get("artifact_path", "")).strip()
        rationale = str(item.get("rationale", "")).strip()

        if not evidence_id:
            errors.append(f"{prefix}.id is required")
        elif evidence_id in seen_ids:
            errors.append(f"duplicate evidence id: {evidence_id}")
        else:
            seen_ids.add(evidence_id)

        if result not in ALLOWED_RESULTS:
            errors.append(f"{prefix}.result must be one of {sorted(ALLOWED_RESULTS)}")

        if result == "ACCEPTED_RISK" and not rationale:
            errors.append(f"{prefix}.rationale is required when result is ACCEPTED_RISK")

        if not artifact_raw and result != "ACCEPTED_RISK":
            errors.append(f"{prefix}.artifact_path is required")
        elif artifact_raw:
            artifact_path = resolve_artifact_path(artifact_raw)
            if not artifact_path.exists():
                errors.append(f"{prefix}.artifact_path does not exist: {artifact_path}")
            elif artifact_path.is_file():
                errors.extend(validate_bench_result_file(artifact_path, result))

        if evidence_id == CORE_BENCH_ID:
            core_seen = True
            allowed_core_results = {"PASS", "WARN"} if allow_bench_warn else {"PASS"}
            if result not in allowed_core_results:
                errors.append(
                    f"{CORE_BENCH_ID}.result must be PASS"
                    + (" or WARN with --allow-bench-warn" if allow_bench_warn else "")
                )

        if evidence_id == OBD_PROXY_ID:
            obd_proxy_seen = True
            if kind == "hardware-qualification":
                if result != "PASS":
                    errors.append(f"{prefix}.result must be PASS for hardware-qualification")
                if not artifact_raw:
                    # The generic artifact rule above reports the missing path.
                    pass
                else:
                    artifact_path = resolve_artifact_path(artifact_raw)
                    if artifact_path.exists() and not artifact_path.is_file():
                        errors.append(f"{prefix}.artifact_path must be a file: {artifact_path}")
                    elif artifact_path.is_file():
                        for error in obd_proxy_qualification.validate_artifact_file(artifact_path):
                            errors.append(f"{prefix}.artifact: {error}")
            elif kind == "accepted-risk":
                if result != "ACCEPTED_RISK":
                    errors.append(f"{prefix}.result must be ACCEPTED_RISK for accepted-risk")
                if not rationale:
                    errors.append(f"{prefix}.rationale is required for accepted-risk")
                if not has_nonempty_scope(item.get("scope")):
                    errors.append(
                        f"{prefix}.scope must be a non-empty string or array of non-empty strings"
                    )
                if artifact_raw:
                    errors.append(f"{prefix}.artifact_path must be omitted for accepted-risk")
            else:
                errors.append(
                    f"{prefix}.kind must be hardware-qualification or accepted-risk for {OBD_PROXY_ID}"
                )

    if not core_seen:
        errors.append(f"missing required evidence id: {CORE_BENCH_ID}")
    if not obd_proxy_seen:
        errors.append(f"missing required evidence id: {OBD_PROXY_ID}")

    return errors


def main() -> int:
    args = parse_args()
    manifest_path = Path(args.manifest).expanduser()
    try:
        payload = load_json(manifest_path)
    except ValueError as exc:
        print(f"[release-evidence] {exc}")
        return 1

    errors = validate_manifest(payload, allow_bench_warn=args.allow_bench_warn)
    if errors:
        print("[release-evidence] manifest validation failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(f"[release-evidence] manifest valid: {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
