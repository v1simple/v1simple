#!/usr/bin/env python3
"""Generate a commit-bound local release-evidence manifest atomically."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import check_obd_proxy_qualification as obd_qualification
import check_release_evidence_manifest as evidence_check


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_POLICY = ROOT / "config" / "release_evidence_policy.json"
DEFAULT_OUTPUT = ROOT / ".artifacts" / "release_evidence" / "manifest.json"
FULL_GIT_SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")


class EvidencePreparationError(RuntimeError):
    """Release evidence cannot be generated without weakening its contract."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench-result", required=True, help="Path to bench_result.json.")
    choice = parser.add_mutually_exclusive_group(required=True)
    choice.add_argument(
        "--obd-qualification",
        help="Path to a validated OBD/proxy qualification_result.json.",
    )
    choice.add_argument(
        "--accept-obd-risk",
        action="store_true",
        help="Explicitly accept the policy-scoped OBD/proxy release risk.",
    )
    rationale = parser.add_mutually_exclusive_group()
    rationale.add_argument("--risk-rationale", help="Accepted-risk rationale text.")
    rationale.add_argument(
        "--risk-rationale-file",
        help="Read the accepted-risk rationale from this text file.",
    )
    parser.add_argument("--out", default=str(DEFAULT_OUTPUT))
    parser.add_argument(
        "--release-git-sha",
        default="",
        help="Expected full release SHA; defaults to the clean checkout's HEAD.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate and print the generated manifest without writing it.",
    )
    return parser.parse_args()


def load_json(path: Path, label: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise EvidencePreparationError(f"{label} not found: {path}") from exc
    except OSError as exc:
        raise EvidencePreparationError(f"could not read {label} {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise EvidencePreparationError(
            f"{label} is invalid JSON: {path}:{exc.lineno}:{exc.colno}: {exc.msg}"
        ) from exc


def git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown Git error"
        raise EvidencePreparationError(f"git {' '.join(args)} failed: {detail}")
    return result.stdout.strip()


def resolve_release_git_sha(root: Path, override: str = "") -> str:
    if git(root, "status", "--porcelain"):
        raise EvidencePreparationError(
            "release evidence generation requires a clean public worktree"
        )
    resolved = override.strip().lower() or git(root, "rev-parse", "HEAD").lower()
    if not FULL_GIT_SHA_RE.fullmatch(resolved):
        raise EvidencePreparationError("release Git SHA must be a full 40-character SHA")
    head = git(root, "rev-parse", "HEAD").lower()
    if resolved != head:
        raise EvidencePreparationError(
            f"requested release Git SHA {resolved} does not match checkout HEAD {head}"
        )
    return resolved


def require_inside_root(root: Path, path: Path, label: str) -> tuple[Path, str]:
    resolved = path.expanduser().resolve()
    try:
        relative = resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise EvidencePreparationError(f"{label} must be inside the public repository") from exc
    if not resolved.is_file():
        raise EvidencePreparationError(f"{label} is not a file: {resolved}")
    return resolved, relative.as_posix()


def validate_policy(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict) or payload.get("schema_version") != 1:
        raise EvidencePreparationError("release evidence policy schema_version must be 1")
    risk = payload.get("accepted_risk")
    if not isinstance(risk, dict) or risk.get("id") != evidence_check.OBD_PROXY_ID:
        raise EvidencePreparationError(
            f"policy accepted_risk.id must be {evidence_check.OBD_PROXY_ID}"
        )
    scope = risk.get("scope")
    if not evidence_check.has_nonempty_scope(scope) or not isinstance(scope, list):
        raise EvidencePreparationError("policy accepted_risk.scope must be a non-empty string array")
    minimum = risk.get("minimum_rationale_characters")
    if type(minimum) is not int or minimum < 1:
        raise EvidencePreparationError(
            "policy minimum_rationale_characters must be a positive integer"
        )
    return risk


def load_rationale(text: str | None, file_name: str | None) -> str:
    if file_name:
        try:
            return Path(file_name).expanduser().read_text(encoding="utf-8").strip()
        except OSError as exc:
            raise EvidencePreparationError(f"could not read risk rationale file: {exc}") from exc
    return (text or "").strip()


def generate_manifest(
    *,
    root: Path,
    release_git_sha: str,
    bench_result: Path,
    policy: dict[str, Any],
    obd_qualification_path: Path | None = None,
    accepted_risk_rationale: str = "",
    generated_at_utc: str | None = None,
) -> dict[str, Any]:
    root = root.resolve()
    release_git_sha = release_git_sha.strip().lower()
    if not FULL_GIT_SHA_RE.fullmatch(release_git_sha):
        raise EvidencePreparationError("release_git_sha must be a full 40-character SHA")

    bench_path, bench_relative = require_inside_root(root, bench_result, "bench result")
    bench_payload = load_json(bench_path, "bench result")
    if not isinstance(bench_payload, dict):
        raise EvidencePreparationError("bench result root must be an object")
    if bench_payload.get("result") != "PASS":
        raise EvidencePreparationError("bench result must be PASS")
    if str(bench_payload.get("git_sha", "")).strip().lower() != release_git_sha:
        raise EvidencePreparationError("bench result Git SHA does not match release Git SHA")
    if bench_payload.get("git_worktree_clean") is not True:
        raise EvidencePreparationError("bench result must prove a clean collection worktree")

    risk_policy = validate_policy(policy)
    evidence: list[dict[str, Any]] = [
        {
            "id": evidence_check.CORE_BENCH_ID,
            "kind": "bench",
            "result": "PASS",
            "artifact_path": bench_relative,
        }
    ]

    if obd_qualification_path is not None:
        if accepted_risk_rationale:
            raise EvidencePreparationError(
                "hardware qualification and accepted-risk rationale are mutually exclusive"
            )
        qualification_path, qualification_relative = require_inside_root(
            root, obd_qualification_path, "OBD/proxy qualification artifact"
        )
        qualification_errors = obd_qualification_errors(qualification_path)
        if qualification_errors:
            raise EvidencePreparationError(
                "OBD/proxy qualification is invalid: " + "; ".join(qualification_errors)
            )
        qualification_payload = load_json(
            qualification_path, "OBD/proxy qualification artifact"
        )
        artifact_sha = str(qualification_payload.get("release_git_sha", "")).strip().lower()
        if artifact_sha != release_git_sha:
            raise EvidencePreparationError(
                "OBD/proxy qualification Git SHA does not match release Git SHA"
            )
        evidence.append(
            {
                "id": evidence_check.OBD_PROXY_ID,
                "kind": "hardware-qualification",
                "result": "PASS",
                "artifact_path": qualification_relative,
            }
        )
    else:
        rationale = accepted_risk_rationale.strip()
        minimum = risk_policy["minimum_rationale_characters"]
        if len(rationale) < minimum:
            raise EvidencePreparationError(
                f"accepted-risk rationale must contain at least {minimum} characters"
            )
        evidence.append(
            {
                "id": risk_policy["id"],
                "kind": "accepted-risk",
                "result": "ACCEPTED_RISK",
                "rationale": rationale,
                "scope": list(risk_policy["scope"]),
            }
        )

    generated_at_utc = generated_at_utc or datetime.now(timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )
    payload = {
        "schema_version": 2,
        "release_git_sha": release_git_sha,
        "generated_at_utc": generated_at_utc,
        "evidence": evidence,
    }
    errors = evidence_check.validate_manifest(
        payload,
        expected_git_sha=release_git_sha,
        root=root,
        risk_policy=risk_policy,
    )
    if errors:
        raise EvidencePreparationError("generated manifest is invalid: " + "; ".join(errors))
    return payload


def obd_qualification_errors(path: Path) -> list[str]:
    return obd_qualification.validate_artifact_file(path)


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(payload, indent=2) + "\n"
    temporary_name = ""
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary.write(text)
            temporary.flush()
            os.fsync(temporary.fileno())
            temporary_name = temporary.name
        os.replace(temporary_name, path)
    except OSError as exc:
        raise EvidencePreparationError(f"could not atomically write {path}: {exc}") from exc
    finally:
        if temporary_name:
            try:
                Path(temporary_name).unlink(missing_ok=True)
            except OSError:
                pass


def main() -> int:
    args = parse_args()
    try:
        release_git_sha = resolve_release_git_sha(ROOT, args.release_git_sha)
        policy = load_json(DEFAULT_POLICY, "release evidence policy")
        rationale = load_rationale(args.risk_rationale, args.risk_rationale_file)
        if args.accept_obd_risk and not rationale:
            raise EvidencePreparationError(
                "--accept-obd-risk requires --risk-rationale or --risk-rationale-file"
            )
        if not args.accept_obd_risk and rationale:
            raise EvidencePreparationError(
                "risk rationale may only be supplied with --accept-obd-risk"
            )
        payload = generate_manifest(
            root=ROOT,
            release_git_sha=release_git_sha,
            bench_result=Path(args.bench_result),
            policy=policy,
            obd_qualification_path=Path(args.obd_qualification)
            if args.obd_qualification
            else None,
            accepted_risk_rationale=rationale,
        )
        if args.dry_run:
            print(json.dumps(payload, indent=2))
        else:
            output = Path(args.out).expanduser().resolve()
            try:
                output.relative_to(ROOT.resolve())
            except ValueError as exc:
                raise EvidencePreparationError(
                    "output manifest must be inside the public repository"
                ) from exc
            write_json_atomic(output, payload)
            print(f"[release-evidence] generated and validated: {output}")
        return 0
    except EvidencePreparationError as exc:
        print(f"[release-evidence] generation failed: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
