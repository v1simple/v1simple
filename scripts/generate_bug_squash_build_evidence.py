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
    active_contracts: list[dict[str, Any]] = []
    for contract in contracts:
        if not isinstance(contract, dict):
            raise GenerationError("pinned profile has an invalid build contract")
        kind = contract.get("kind")
        implementation_status = contract.get("implementation_status")
        environment = contract.get("environment")
        command = contract.get("build_command")
        if not isinstance(kind, str) or kind in seen_kinds:
            raise GenerationError("pinned build kinds must be valid and unique")
        seen_kinds.add(kind)
        if implementation_status == "blocked":
            if environment is not None or command != [] or not contract.get("blocker_code"):
                raise GenerationError("blocked pinned build contract is inconsistent")
            continue
        if implementation_status != "active":
            raise GenerationError("pinned build implementation status is invalid")
        if not isinstance(kind, str) or not isinstance(environment, str):
            raise GenerationError("pinned build identity is invalid")
        if environment in seen_environments:
            raise GenerationError("pinned build environments must be unique")
        seen_environments.add(environment)
        if command != ["pio", "run", "-e", environment]:
            raise GenerationError("pinned build command must select its exact environment")
        if environment not in declared_environments:
            missing.append(environment)
        active_contracts.append(contract)
    if missing:
        raise GenerationError(
            "pinned PlatformIO environments are not implemented: "
            + ", ".join(sorted(missing))
        )
    if not active_contracts:
        raise GenerationError("pinned profile has no active build contracts")
    return active_contracts


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
        head_result = qualification.run_authoritative_git(["rev-parse", "HEAD"])
        if head_result.returncode != 0:
            raise GenerationError("repository target commit could not be resolved")
        head = head_result.stdout.strip()
        repository_state, state_errors = qualification.read_repository_state(head, head)
        if repository_state is None or state_errors:
            raise GenerationError("repository target must exist at a clean live HEAD")
        try:
            target_tree_sha = qualification.git_tree_sha256(head)
            tools = qualification.current_build_tool_identity()
            generator_sha = qualification.git_blob_sha256(
                head,
                "scripts/generate_bug_squash_build_evidence.py",
            )
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            raise GenerationError(f"build provenance could not be established: {exc}") from exc
        pio_raw = shutil.which("pio")
        if pio_raw is None or qualification.file_sha256(Path(pio_raw).resolve()) != tools[
            "platformio"
        ]["sha256"]:
            raise GenerationError("PlatformIO executable does not match tool provenance")
        pio_path = Path(pio_raw).resolve()
        pio_executable = str(pio_path)
        build_environment = qualification.authoritative_tool_environment(pio_path)
        contracts_sha = qualification.build_contracts_sha256(profile)

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
            process_command = [pio_executable, *command[1:]]
            contract_sha = qualification.canonical_commitment(
                "v1simple.hil.build-contract.v1",
                contract,
            )
            input_commitment = qualification.build_input_commitment(
                target_git_sha=repository_state.head_sha,
                target_tree_sha256=target_tree_sha,
                firmware_version=repository_state.firmware_version,
                contract=contract,
                tools=tools,
            )
            started_at = utc_now()
            process_output = ""
            try:
                result = subprocess.run(
                    process_command,
                    cwd=ROOT,
                    env=build_environment,
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
            log_header = qualification.expected_build_log_header(
                target_git_sha=repository_state.head_sha,
                target_tree_sha256=target_tree_sha,
                build_contract_sha256=contract_sha,
                tool_identity_sha256=tools["identity_sha256"],
                kind=kind,
                environment=contract["environment"],
                started_at_utc=started_at,
                completed_at_utc=completed_at,
            )
            exit_code = result.returncode if result is not None else -1
            log_header[-1] = f"exit_code={exit_code}"
            log_content = "\n".join(log_header) + "\n" + process_output
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
            qualification.current_build_tool_identity.cache_clear()
            try:
                post_build_tools = qualification.current_build_tool_identity()
            except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
                raise GenerationError(
                    f"pinned {kind} build tool identity could not be re-established"
                ) from exc
            if post_build_tools != tools:
                raise GenerationError(f"pinned {kind} build changed its tool identity")

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
            log_entry = artifacts[-1]
            output_commitment = qualification.build_output_commitment(
                binary_sha256=binary_entry["sha256"],
                log_sha256=log_entry["sha256"],
                started_at_utc=started_at,
                completed_at_utc=completed_at,
            )
            build_record = {
                "kind": kind,
                "firmware_version": repository_state.firmware_version,
                "environment": contract["environment"],
                "commit_sha": repository_state.head_sha,
                "build_command": command,
                "build_contract_sha256": contract_sha,
                "binary_artifact_id": binary_artifact_id,
                "binary_sha256": binary_entry["sha256"],
                "log_artifact_id": log_artifact_id,
                "log_sha256": log_entry["sha256"],
                "source_worktree_clean": True,
                "started_at_utc": started_at,
                "completed_at_utc": completed_at,
                "input_commitment_sha256": input_commitment,
                "output_commitment_sha256": output_commitment,
            }
            builds.append(
                qualification.with_provenance_commitment(
                    "v1simple.hil.build-provenance.v1",
                    build_record,
                )
            )

        manifest_payload = {
            "schema_version": 3,
            "target_git_sha": repository_state.head_sha,
            "target_tree_sha256": target_tree_sha,
            "build_contracts_sha256": contracts_sha,
            "observed_at_utc": utc_now(),
            "generator": {
                "path": "scripts/generate_bug_squash_build_evidence.py",
                "sha256": generator_sha,
            },
            "tools": tools,
            "builds": builds,
        }
        manifest = qualification.with_provenance_commitment(
            "v1simple.hil.build-manifest.v1",
            manifest_payload,
        )
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
        execution_provenance_path = manifests / "execution-provenance.json"
        execution_provenance_path.write_text(
            json.dumps(
                qualification.expected_execution_provenance(profile, head),
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        execution_provenance_entry = evidence_entry(
            "execution-provenance",
            "execution-provenance",
            "json",
            execution_provenance_path,
            output,
        )
        index_path = manifests / "build-evidence-index.json"
        index_path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "build_manifest_artifact_id": "build-manifest",
                    "execution_provenance_artifact_id": "execution-provenance",
                    "evidence_artifacts": [
                        manifest_entry,
                        execution_provenance_entry,
                        *artifacts,
                    ],
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
