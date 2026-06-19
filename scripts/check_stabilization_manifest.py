#!/usr/bin/env python3
"""Verify the anti-drift stabilization manifest is complete and well-formed."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "config" / "stabilization_manifest.json"

REQUIRED_ARTIFACT_IDS = {
    "stabilization_manifest",
    "stabilization_manifest_check",
    "main_loop_phases_harness",
    "tap_gesture_production_test",
    "power_module_production_test",
}
REQUIRED_GAP_IDS = {
    "native_test_lane_determinism",
    "post_api_settings_route_cleanup",
    "wrapper_primary_risky_seam_tests",
    "main_loop_phases_harness",
    "tap_gesture_production_coverage",
    "duplicate_owner_inventory",
}
REQUIRED_PROTECTIVE_IDS = {
    "wifi_fail_open_paths",
    "runtime_null_guards",
    "busy_mutation_rejections",
}
REQUIRED_DUPLICATE_OWNER_IDS = {
    "auto_push_execution",
    "alert_time_volume_execution",
    "wifi_start_reenable_lifecycle",
    "speed_arbitration",
}
VALID_GAP_STATUSES = {"open", "in_progress", "resolved"}
VALID_CLASSIFICATIONS = {"dead", "duplicate", "protective"}


def load_manifest() -> dict:
    if not MANIFEST_PATH.is_file():
        raise FileNotFoundError(f"manifest not found: {MANIFEST_PATH}")
    return json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))


def require_string(entry: dict, key: str, errors: list[str], context: str) -> None:
    value = entry.get(key)
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{context} missing non-empty string field '{key}'")


def require_list(entry: dict, key: str, errors: list[str], context: str) -> list:
    value = entry.get(key)
    if not isinstance(value, list):
        errors.append(f"{context} missing list field '{key}'")
        return []
    return value


def check_required_artifacts(manifest: dict, errors: list[str]) -> None:
    artifacts = manifest.get("required_artifacts")
    if not isinstance(artifacts, list):
        errors.append("required_artifacts must be a list")
        return

    ids: set[str] = set()
    for index, artifact in enumerate(artifacts):
        context = f"required_artifacts[{index}]"
        if not isinstance(artifact, dict):
            errors.append(f"{context} must be an object")
            continue
        require_string(artifact, "id", errors, context)
        require_string(artifact, "path", errors, context)
        require_string(artifact, "purpose", errors, context)
        artifact_id = artifact.get("id")
        artifact_path = artifact.get("path")
        if isinstance(artifact_id, str):
            ids.add(artifact_id)
        if isinstance(artifact_path, str) and artifact_path.strip():
            target = ROOT / artifact_path
            if not target.exists():
                errors.append(f"{context} path does not exist: {artifact_path}")

    missing = sorted(REQUIRED_ARTIFACT_IDS - ids)
    if missing:
        errors.append(f"required_artifacts missing ids: {', '.join(missing)}")


def check_remaining_gaps(manifest: dict, errors: list[str]) -> None:
    gaps = manifest.get("remaining_gaps")
    if not isinstance(gaps, list):
        errors.append("remaining_gaps must be a list")
        return

    ids: set[str] = set()
    for index, gap in enumerate(gaps):
        context = f"remaining_gaps[{index}]"
        if not isinstance(gap, dict):
            errors.append(f"{context} must be an object")
            continue
        require_string(gap, "id", errors, context)
        require_string(gap, "description", errors, context)
        status = gap.get("status")
        if status not in VALID_GAP_STATUSES:
            errors.append(
                f"{context} has invalid status {status!r}; "
                f"expected one of {sorted(VALID_GAP_STATUSES)}"
            )
        wave = gap.get("wave")
        if not isinstance(wave, int) or wave < 0:
            errors.append(f"{context} must include non-negative integer field 'wave'")
        gap_id = gap.get("id")
        if isinstance(gap_id, str):
            ids.add(gap_id)

    missing = sorted(REQUIRED_GAP_IDS - ids)
    if missing:
        errors.append(f"remaining_gaps missing ids: {', '.join(missing)}")


def check_protective_inventory(manifest: dict, errors: list[str]) -> None:
    inventory = manifest.get("protective_cleanup_inventory")
    if not isinstance(inventory, list):
        errors.append("protective_cleanup_inventory must be a list")
        return

    ids: set[str] = set()
    for index, item in enumerate(inventory):
        context = f"protective_cleanup_inventory[{index}]"
        if not isinstance(item, dict):
            errors.append(f"{context} must be an object")
            continue
        require_string(item, "id", errors, context)
        require_string(item, "current_owner", errors, context)
        require_string(item, "preserved_negative_semantics", errors, context)
        tests = require_list(item, "tests", errors, context)
        if tests and not all(isinstance(test, str) and test.strip() for test in tests):
            errors.append(f"{context} tests must be a list of non-empty strings")
        item_id = item.get("id")
        if isinstance(item_id, str):
            ids.add(item_id)

    missing = sorted(REQUIRED_PROTECTIVE_IDS - ids)
    if missing:
        errors.append(f"protective_cleanup_inventory missing ids: {', '.join(missing)}")


def check_cleanup_ledger(manifest: dict, errors: list[str]) -> None:
    ledger = manifest.get("cleanup_review_ledger")
    if not isinstance(ledger, list):
        errors.append("cleanup_review_ledger must be a list")
        return

    for index, item in enumerate(ledger):
        context = f"cleanup_review_ledger[{index}]"
        if not isinstance(item, dict):
            errors.append(f"{context} must be an object")
            continue
        require_string(item, "path", errors, context)
        classification = item.get("classification")
        if classification not in VALID_CLASSIFICATIONS:
            errors.append(
                f"{context} has invalid classification {classification!r}; "
                f"expected one of {sorted(VALID_CLASSIFICATIONS)}"
            )
        require_string(item, "current_owner", errors, context)
        require_string(item, "replacement_or_deadness_proof", errors, context)
        require_string(item, "preserved_negative_semantics", errors, context)
        tests = require_list(item, "tests", errors, context)
        if tests and not all(isinstance(test, str) and test.strip() for test in tests):
            errors.append(f"{context} tests must be a list of non-empty strings")


def check_duplicate_owner_inventory(manifest: dict, errors: list[str]) -> None:
    inventory = manifest.get("duplicate_owner_inventory")
    if not isinstance(inventory, list):
        errors.append("duplicate_owner_inventory must be a list")
        return

    ids: set[str] = set()
    for index, item in enumerate(inventory):
        context = f"duplicate_owner_inventory[{index}]"
        if not isinstance(item, dict):
            errors.append(f"{context} must be an object")
            continue
        require_string(item, "id", errors, context)
        require_string(item, "behavior", errors, context)
        require_string(item, "intended_single_owner", errors, context)
        require_string(item, "preserved_negative_semantics", errors, context)
        classification = item.get("classification")
        if classification != "duplicate":
            errors.append(
                f"{context} has invalid classification {classification!r}; "
                "expected 'duplicate'"
            )
        status = item.get("consolidation_status")
        if status not in VALID_GAP_STATUSES:
            errors.append(
                f"{context} has invalid consolidation_status {status!r}; "
                f"expected one of {sorted(VALID_GAP_STATUSES)}"
            )
        wave = item.get("wave")
        if wave != 4:
            errors.append(f"{context} must include wave 4")
        owners = require_list(item, "current_owner_candidates", errors, context)
        if owners and not all(isinstance(owner, str) and owner.strip() for owner in owners):
            errors.append(
                f"{context} current_owner_candidates must be a list of non-empty strings"
            )
        tests = require_list(item, "tests", errors, context)
        for test in tests:
            if not isinstance(test, str) or not test.strip():
                errors.append(f"{context} tests must be a list of non-empty strings")
                continue
            if not (ROOT / test).exists():
                errors.append(f"{context} test path does not exist: {test}")
        item_id = item.get("id")
        if isinstance(item_id, str):
            ids.add(item_id)

    missing = sorted(REQUIRED_DUPLICATE_OWNER_IDS - ids)
    if missing:
        errors.append(f"duplicate_owner_inventory missing ids: {', '.join(missing)}")


def main() -> int:
    errors: list[str] = []

    try:
        manifest = load_manifest()
    except (FileNotFoundError, json.JSONDecodeError) as exc:
        print(f"[stabilization-manifest] {exc}")
        return 1

    if not isinstance(manifest, dict):
        print("[stabilization-manifest] manifest root must be an object")
        return 1

    if manifest.get("version") != 1:
        errors.append("version must equal 1")

    check_required_artifacts(manifest, errors)
    check_remaining_gaps(manifest, errors)
    check_protective_inventory(manifest, errors)
    check_duplicate_owner_inventory(manifest, errors)
    check_cleanup_ledger(manifest, errors)

    if errors:
        print("[stabilization-manifest] manifest validation failed:")
        for message in errors:
            print(f"  - {message}")
        return 1

    open_gaps = sum(1 for gap in manifest["remaining_gaps"] if gap["status"] != "resolved")
    open_duplicate_owners = sum(
        1
        for item in manifest["duplicate_owner_inventory"]
        if item["consolidation_status"] != "resolved"
    )
    print(
        "[stabilization-manifest] "
        f"manifest valid ({len(manifest['remaining_gaps'])} gaps tracked, "
        f"{len(manifest['protective_cleanup_inventory'])} protective items, "
        f"{len(manifest['duplicate_owner_inventory'])} duplicate-owner items, "
        f"{len(manifest['cleanup_review_ledger'])} cleanup ledger entries, "
        f"{open_gaps} unresolved gaps, "
        f"{open_duplicate_owners} unresolved duplicate-owner consolidations)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
