#!/usr/bin/env python3
"""Validate a local release hardware-evidence manifest.

This is intentionally not part of normal PR CI because the artifacts are local
and hardware-lab specific. Run it during release prep after `./bench.sh` and any
required OBD/proxy/arbitration hardware pass or accepted-risk waiver.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Mapping

import check_obd_proxy_qualification as obd_proxy_qualification

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / ".artifacts" / "release_evidence" / "manifest.json"
DEFAULT_POLICY = ROOT / "config" / "release_evidence_policy.json"
FULL_GIT_SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")
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
        "--expected-git-sha",
        default="",
        help="Require the manifest and every typed artifact to match this full Git SHA.",
    )
    return parser.parse_args()


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"manifest not found: {path}") from exc
    except OSError as exc:
        raise ValueError(f"could not read manifest {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"manifest is not valid JSON: {path}:{exc.lineno}:{exc.colno}: {exc.msg}") from exc


def resolve_artifact_path(raw: str, root: Path = ROOT) -> Path:
    path = Path(raw)
    resolved = (path if path.is_absolute() else (root / path)).resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise ValueError("artifact path must stay inside the public repository") from exc
    return resolved


def accepted_risk_policy(
    policy: Mapping[str, Any] | None = None,
) -> tuple[dict[str, Any] | None, list[str]]:
    if policy is None:
        try:
            loaded = json.loads(DEFAULT_POLICY.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            return None, [f"could not load accepted-risk policy: {exc}"]
        if not isinstance(loaded, dict) or loaded.get("schema_version") != 1:
            return None, ["accepted-risk policy schema_version must be 1"]
        policy = loaded.get("accepted_risk")
    if not isinstance(policy, Mapping):
        return None, ["accepted-risk policy must be an object"]
    risk = dict(policy)
    if risk.get("id") != OBD_PROXY_ID:
        return None, [f"accepted-risk policy id must be {OBD_PROXY_ID}"]
    scope = risk.get("scope")
    if not isinstance(scope, list) or not has_nonempty_scope(scope):
        return None, ["accepted-risk policy scope must be a non-empty string array"]
    minimum = risk.get("minimum_rationale_characters")
    if type(minimum) is not int or minimum < 1:
        return None, ["accepted-risk policy rationale minimum must be positive"]
    return risk, []


def validate_bench_result_file(
    path: Path,
    declared_result: str,
    release_git_sha: str,
) -> list[str]:
    errors: list[str] = []
    if path.name != "bench_result.json":
        return errors
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 - report file parse detail without crashing validation
        return [f"{path}: could not parse bench_result.json: {exc}"]

    if not isinstance(payload, dict):
        return [f"{path}: bench_result.json root must be an object"]

    if payload.get("schema_version") != 2:
        errors.append(f"{path}: schema_version must be 2")
    if payload.get("kind") != "bench_result":
        errors.append(f"{path}: kind must be bench_result")

    actual = str(payload.get("result", "")).strip()
    if actual != declared_result:
        errors.append(
            f"{path}: manifest result {declared_result} does not match bench_result.json result {actual}"
        )
    if actual != "PASS":
        errors.append(f"{path}: result must be PASS for release evidence")
    artifact_git_sha = str(payload.get("git_sha", "")).strip().lower()
    if not FULL_GIT_SHA_RE.fullmatch(artifact_git_sha):
        errors.append(f"{path}: git_sha must be a full 40-character hexadecimal Git SHA")
    elif artifact_git_sha != release_git_sha:
        errors.append(
            f"{path}: git_sha {artifact_git_sha} does not match release_git_sha {release_git_sha}"
        )
    if payload.get("git_worktree_clean") is not True:
        errors.append(f"{path}: git_worktree_clean must be true for release evidence")
    if not str(payload.get("git_ref", "")).strip():
        errors.append(f"{path}: git_ref must be non-empty")

    windows = payload.get("windows")
    if not isinstance(windows, list):
        errors.append(f"{path}: windows must be an array")
    else:
        suites = {
            str(window.get("suite", "")).strip()
            for window in windows
            if isinstance(window, dict)
        }
        if len(windows) != 2 or suites != {"core", "display"}:
            errors.append(f"{path}: release bench must contain exactly core and display windows")
        for index, window in enumerate(windows):
            if not isinstance(window, dict):
                errors.append(f"{path}: windows[{index}] must be an object")
                continue
            if window.get("result") != "PASS":
                errors.append(f"{path}: windows[{index}].result must be PASS")
            window_sha = str(window.get("git_sha", "")).strip().lower()
            if window_sha != release_git_sha:
                errors.append(
                    f"{path}: windows[{index}].git_sha must match release_git_sha"
                )
            if window.get("git_worktree_clean") is not True:
                errors.append(f"{path}: windows[{index}].git_worktree_clean must be true")
            if not str(window.get("git_ref", "")).strip():
                errors.append(f"{path}: windows[{index}].git_ref must be non-empty")
    return errors


def has_nonempty_scope(value: Any) -> bool:
    if isinstance(value, str):
        return bool(value.strip())
    if isinstance(value, list):
        return bool(value) and all(isinstance(item, str) and item.strip() for item in value)
    return False


def validate_manifest(
    payload: Any,
    *,
    expected_git_sha: str = "",
    root: Path = ROOT,
    risk_policy: Mapping[str, Any] | None = None,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(payload, dict):
        return ["manifest root must be an object"]

    if payload.get("schema_version") != 2:
        errors.append("schema_version must be 2")

    generated_at_utc = payload.get("generated_at_utc")
    if not isinstance(generated_at_utc, str) or not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", generated_at_utc
    ):
        errors.append("generated_at_utc must be an ISO-like UTC timestamp")

    canonical_risk, policy_errors = accepted_risk_policy(risk_policy)
    errors.extend(policy_errors)

    release_git_sha = str(payload.get("release_git_sha", "")).strip().lower()
    if not FULL_GIT_SHA_RE.fullmatch(release_git_sha):
        errors.append("release_git_sha must be a full 40-character hexadecimal Git SHA")
    expected_git_sha = expected_git_sha.strip().lower()
    if expected_git_sha:
        if not FULL_GIT_SHA_RE.fullmatch(expected_git_sha):
            errors.append("expected_git_sha must be a full 40-character hexadecimal Git SHA")
        elif release_git_sha != expected_git_sha:
            errors.append(
                f"release_git_sha {release_git_sha or '<missing>'} does not match expected Git SHA {expected_git_sha}"
            )

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
            try:
                artifact_path = resolve_artifact_path(artifact_raw, root)
            except ValueError as exc:
                errors.append(f"{prefix}.artifact_path {exc}")
                artifact_path = None
            if artifact_path is not None:
                if not artifact_path.exists():
                    errors.append(f"{prefix}.artifact_path does not exist: {artifact_path}")
                elif not artifact_path.is_file():
                    errors.append(f"{prefix}.artifact_path must be a file: {artifact_path}")
                elif evidence_id == CORE_BENCH_ID:
                    errors.extend(
                        validate_bench_result_file(artifact_path, result, release_git_sha)
                    )

        if evidence_id == CORE_BENCH_ID:
            core_seen = True
            if kind != "bench":
                errors.append(f"{prefix}.kind must be bench for {CORE_BENCH_ID}")
            if result != "PASS":
                errors.append(f"{CORE_BENCH_ID}.result must be PASS")
            if artifact_raw and Path(artifact_raw).name != "bench_result.json":
                errors.append(f"{prefix}.artifact_path must name bench_result.json")

        if evidence_id == OBD_PROXY_ID:
            obd_proxy_seen = True
            if kind == "hardware-qualification":
                if result != "PASS":
                    errors.append(f"{prefix}.result must be PASS for hardware-qualification")
                if not artifact_raw:
                    # The generic artifact rule above reports the missing path.
                    pass
                else:
                    try:
                        artifact_path = resolve_artifact_path(artifact_raw, root)
                    except ValueError:
                        artifact_path = None
                    if artifact_path is not None and artifact_path.exists() and not artifact_path.is_file():
                        errors.append(f"{prefix}.artifact_path must be a file: {artifact_path}")
                    elif artifact_path is not None and artifact_path.is_file():
                        for error in obd_proxy_qualification.validate_artifact_file(artifact_path):
                            errors.append(f"{prefix}.artifact: {error}")
                        try:
                            artifact_payload = json.loads(artifact_path.read_text(encoding="utf-8"))
                        except (OSError, json.JSONDecodeError):
                            artifact_payload = None
                        if isinstance(artifact_payload, dict):
                            artifact_git_sha = str(
                                artifact_payload.get("release_git_sha", "")
                            ).strip().lower()
                            if artifact_git_sha != release_git_sha:
                                errors.append(
                                    f"{prefix}.artifact release_git_sha {artifact_git_sha or '<missing>'} "
                                    f"does not match manifest release_git_sha {release_git_sha or '<missing>'}"
                                )
            elif kind == "accepted-risk":
                if result != "ACCEPTED_RISK":
                    errors.append(f"{prefix}.result must be ACCEPTED_RISK for accepted-risk")
                if not rationale:
                    errors.append(f"{prefix}.rationale is required for accepted-risk")
                if canonical_risk is not None:
                    minimum = canonical_risk["minimum_rationale_characters"]
                    if len(rationale) < minimum:
                        errors.append(
                            f"{prefix}.rationale must contain at least {minimum} characters"
                        )
                    if item.get("scope") != canonical_risk["scope"]:
                        errors.append(f"{prefix}.scope must exactly match accepted-risk policy")
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
    unknown_ids = seen_ids - {CORE_BENCH_ID, OBD_PROXY_ID}
    if unknown_ids:
        errors.append(f"unknown evidence ids: {', '.join(sorted(unknown_ids))}")

    return errors


def main() -> int:
    args = parse_args()
    manifest_path = Path(args.manifest).expanduser()
    try:
        payload = load_json(manifest_path)
    except ValueError as exc:
        print(f"[release-evidence] {exc}")
        return 1

    errors = validate_manifest(
        payload,
        expected_git_sha=args.expected_git_sha,
    )
    if errors:
        print("[release-evidence] manifest validation failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(f"[release-evidence] manifest valid: {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
