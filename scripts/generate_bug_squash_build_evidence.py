#!/usr/bin/env python3
"""Generate retained, fail-closed firmware build evidence for bug-squash HIL."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any

import check_bug_squash_hil_qualification as qualification

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = ROOT / ".artifacts"


class GenerationError(RuntimeError):
    """Raised when provenance cannot be established without an assumption."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-directory",
        required=True,
        help="New or empty output directory below the ignored .artifacts directory.",
    )
    parser.add_argument(
        "--build-timeout-seconds",
        type=int,
        default=1800,
        help="Bound for each pinned PlatformIO build (60-3600; default: 1800).",
    )
    return parser.parse_args()


def run_checked(command: list[str], *, timeout: int = 30) -> str:
    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise GenerationError(f"required command could not complete: {command[0]}: {exc}") from exc
    if result.returncode != 0:
        raise GenerationError(f"required command failed: {command[0]}")
    return result.stdout.strip()


def require_ignored_output(raw: str) -> Path:
    output = Path(raw).expanduser().resolve()
    artifact_root = ARTIFACT_ROOT.resolve()
    if output == artifact_root or artifact_root not in output.parents:
        raise GenerationError(
            "output directory must be a child of the ignored .artifacts directory"
        )
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise GenerationError("output directory must be new or empty")
    output.mkdir(parents=True, exist_ok=True)
    return output


def declared_platformio_environments() -> set[str]:
    try:
        content = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    except OSError as exc:
        raise GenerationError(f"could not read platformio.ini: {exc}") from exc
    return set(re.findall(r"^\[env:([^\]]+)\]\s*$", content, flags=re.MULTILINE))


def preflight_build_contracts(
    contracts: Any,
    declared_environments: set[str],
) -> list[dict[str, Any]]:
    if not isinstance(contracts, list) or not contracts:
        raise GenerationError("pinned profile has no build contracts")
    seen_kinds: set[str] = set()
    seen_environments: set[str] = set()
    missing: list[str] = []
    for contract in contracts:
        if not isinstance(contract, dict):
            raise GenerationError("pinned profile has an invalid build contract")
        kind = contract.get("kind")
        implementation_status = contract.get("implementation_status")
        environment = contract.get("environment")
        command = contract.get("build_command")
        if implementation_status != "active":
            blocker = contract.get("blocker_code")
            raise GenerationError(
                f"pinned build kind {kind!s} is blocked: {blocker!s}"
            )
        if not isinstance(kind, str) or not isinstance(environment, str):
            raise GenerationError("pinned build identity is invalid")
        if kind in seen_kinds or environment in seen_environments:
            raise GenerationError("pinned build identities must be unique")
        seen_kinds.add(kind)
        seen_environments.add(environment)
        if command != ["pio", "run", "-e", environment]:
            raise GenerationError("pinned build command must select its exact environment")
        if environment not in declared_environments:
            missing.append(environment)
    if missing:
        raise GenerationError(
            "pinned PlatformIO environments are not implemented: "
            + ", ".join(sorted(missing))
        )
    return contracts


def evidence_entry(
    artifact_id: str,
    role: str,
    artifact_format: str,
    path: Path,
    output: Path,
) -> dict[str, Any]:
    return {
        "id": artifact_id,
        "scope": "qualification",
        "role": role,
        "format": artifact_format,
        "path": path.relative_to(output).as_posix(),
        "sha256": qualification.file_sha256(path),
    }


def main() -> int:
    args = parse_args()
    if args.build_timeout_seconds < 60 or args.build_timeout_seconds > 3600:
        print(
            "[bug-squash-build-evidence] build timeout must be between 60 and 3600",
            file=sys.stderr,
        )
        return 2
    try:
        profile, profile_errors = qualification.load_pinned_profile()
        if profile is None or profile_errors:
            raise GenerationError("pinned qualification profile is invalid")
        contracts = preflight_build_contracts(
            profile["build_contracts"],
            declared_platformio_environments(),
        )
        head = run_checked(["git", "rev-parse", "HEAD"])
        repository_state, state_errors = qualification.read_repository_state(head, head)
        if repository_state is None or state_errors:
            raise GenerationError("repository target must exist at a clean live HEAD")

        platformio_version = run_checked(["pio", "--version"])
        esptool_lines = run_checked([sys.executable, "-m", "esptool", "version"]).splitlines()
        esptool_version = next(
            (line.strip() for line in esptool_lines if line.startswith("esptool v")),
            "",
        )
        if not esptool_version:
            raise GenerationError("could not establish the esptool version")

        output = require_ignored_output(args.output_directory)
        binaries = output / "binaries"
        logs = output / "logs"
        manifests = output / "manifests"
        for directory in (binaries, logs, manifests):
            directory.mkdir(parents=True, exist_ok=True)

        artifacts: list[dict[str, Any]] = []
        builds: list[dict[str, Any]] = []
        for contract in contracts:
            kind = contract["kind"]
            command = list(contract["build_command"])
            started_at = utc_now()
            process_output = ""
            try:
                result = subprocess.run(
                    command,
                    cwd=ROOT,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    check=False,
                    timeout=args.build_timeout_seconds,
                )
                process_output = result.stdout
            except (OSError, subprocess.TimeoutExpired) as exc:
                process_output = f"build process did not complete: {type(exc).__name__}\n"
                result = None
            completed_at = utc_now()
            log_content = (
                f"target_git_sha={repository_state.head_sha}\n"
                f"build_kind={kind}\n"
                f"environment={contract['environment']}\n"
                f"started_at_utc={started_at}\n"
                f"completed_at_utc={completed_at}\n"
                + process_output
            )
            log_path = logs / f"{kind}.log"
            log_path.write_text(log_content, encoding="utf-8")
            if result is None or result.returncode != 0:
                raise GenerationError(f"pinned {kind} build failed; retained its local log")

            source_binary = ROOT / ".pio" / "build" / contract["environment"] / "firmware.bin"
            if not source_binary.is_file() or source_binary.stat().st_size <= 0:
                raise GenerationError(f"pinned {kind} build did not produce firmware.bin")
            binary_path = binaries / f"{kind}.bin"
            shutil.copyfile(source_binary, binary_path)
            image_errors: list[str] = []
            qualification.validate_firmware_image(
                binary_path,
                kind,
                image_errors,
                repository_state.firmware_version,
            )
            if image_errors:
                raise GenerationError(f"pinned {kind} build did not produce a valid ESP32-S3 image")
            post_build_state, post_build_errors = qualification.read_repository_state(head, head)
            if post_build_state is None or post_build_errors:
                raise GenerationError(f"pinned {kind} build changed tracked source state")

            binary_artifact_id = f"firmware-{kind}"
            log_artifact_id = f"build-log-{kind}"
            binary_entry = evidence_entry(
                binary_artifact_id,
                contract["binary_role"],
                "binary",
                binary_path,
                output,
            )
            artifacts.extend(
                [
                    binary_entry,
                    evidence_entry(log_artifact_id, "build-log", "text", log_path, output),
                ]
            )
            builds.append(
                {
                    "kind": kind,
                    "firmware_version": repository_state.firmware_version,
                    "environment": contract["environment"],
                    "commit_sha": repository_state.head_sha,
                    "build_command": command,
                    "binary_artifact_id": binary_artifact_id,
                    "binary_sha256": binary_entry["sha256"],
                    "log_artifact_id": log_artifact_id,
                    "source_worktree_clean": True,
                    "started_at_utc": started_at,
                    "completed_at_utc": completed_at,
                }
            )

        manifest = {
            "schema_version": 2,
            "target_git_sha": repository_state.head_sha,
            "observed_at_utc": utc_now(),
            "generator_sha256": qualification.file_sha256(Path(__file__)),
            "platformio_version": platformio_version,
            "esptool_version": esptool_version,
            "builds": builds,
        }
        manifest_path = manifests / "build-manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        manifest_entry = evidence_entry(
            "build-manifest",
            "build-manifest",
            "json",
            manifest_path,
            output,
        )
        index_path = manifests / "build-evidence-index.json"
        index_path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "build_manifest_artifact_id": "build-manifest",
                    "evidence_artifacts": [manifest_entry, *artifacts],
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
    except GenerationError as exc:
        print(f"[bug-squash-build-evidence] failed closed: {exc}", file=sys.stderr)
        return 1

    print(
        "[bug-squash-build-evidence] generated local evidence: "
        f"{index_path.relative_to(ROOT).as_posix()}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
