#!/usr/bin/env python3
"""Use a read-only coding model to investigate one bench artifact directory."""

from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from functools import lru_cache
from pathlib import Path
from typing import Any, Mapping, Sequence


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_PATH = ROOT / "tools" / "bench_investigation.schema.json"
INSTRUCTION_PATH = ROOT / "tools" / "bench_investigator_prompt.md"
VIDEO_SUFFIXES = {".mov", ".mp4", ".m4v", ".mkv"}
IGNORED_OUTPUTS = {"investigation.json", "investigation_debug.json"}
AUXILIARY_OVERVIEW_VIDEO_NAMES = {".camera_preflight.mov"}
ATTACHMENT_DIRECTORY = "investigation_sheets"
INDEX_DIRECTORY = "investigation_index"
MAX_INITIAL_IMAGES = 8
MAX_ATTACHED_IMAGES = 12
MAX_SAMPLED_GAP_ANOMALIES = 32
MAX_VIDEOS_PER_PASS = 8
MAX_VIDEOS_PER_RUN = 12
LOCAL_DEFAULT_MODEL = "qwen3-vl:8b"
LOCAL_DEFAULT_PROVIDER = "ollama"
HOSTED_DEFAULT_MODEL = "gpt-5.6-sol"
BUNDLED_CODEX_EXECUTABLES = (
    Path("/Applications/ChatGPT.app/Contents/Resources/codex"),
    Path("/Applications/Codex.app/Contents/Resources/codex"),
    Path.home() / "Applications/ChatGPT.app/Contents/Resources/codex",
    Path.home() / "Applications/Codex.app/Contents/Resources/codex",
)
LOCAL_DEFAULT_CONTEXT_WINDOW = 98304
LOCAL_AUTO_COMPACT_TOKEN_LIMIT = 65536
LOCAL_TOOL_OUTPUT_TOKEN_LIMIT = 4000
FRAME_COUNT_UNAVAILABLE = -1
LOCAL_PRIVATE_ENVIRONMENT = {
    "ANTHROPIC_API_KEY",
    "ANTHROPIC_BASE_URL",
    "AZURE_OPENAI_API_KEY",
    "AZURE_OPENAI_ENDPOINT",
    "CODEX_API_KEY",
    "CODEX_OSS_BASE_URL",
    "CODEX_OSS_PORT",
    "HTTP_PROXY",
    "HTTPS_PROXY",
    "ALL_PROXY",
    "LMSTUDIO_API_HOST",
    "LMSTUDIO_BASE_URL",
    "LM_STUDIO_BASE_URL",
    "LM_API_TOKEN",
    "LMS_SERVER_HOST",
    "OLLAMA_API_KEY",
    "OLLAMA_HOST",
    "OPENAI_API_BASE",
    "OPENAI_API_KEY",
    "OPENAI_BASE_URL",
    "OPENAI_ORG_ID",
    "OPENAI_PROJECT_ID",
}
SOURCE_BASIS_RANK = {
    "unavailable": 0,
    "current_only": 1,
    "commit_reconstructed": 2,
    "exact": 3,
}


class InvestigationError(RuntimeError):
    """An execution problem that should be preserved in investigation.json."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def code_selection_sha256(content: str, start: int, end: int) -> str | None:
    lines = content.splitlines()
    if (
        not isinstance(start, int)
        or isinstance(start, bool)
        or not isinstance(end, int)
        or isinstance(end, bool)
        or start < 1
        or start > end
        or end > len(lines)
    ):
        return None
    normalized = "\n".join(lines[start - 1 : end]) + "\n"
    return sha256_bytes(normalized.encode("utf-8"))


def _schema_type_matches(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return (
            isinstance(value, (int, float))
            and not isinstance(value, bool)
            and math.isfinite(float(value))
        )
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    return False


def _json_equal(left: Any, right: Any) -> bool:
    if isinstance(left, bool) != isinstance(right, bool):
        return False
    return left == right


def validate_schema_value(
    value: Any,
    schema: Mapping[str, Any],
    root_schema: Mapping[str, Any],
    path: str = "$",
) -> list[str]:
    if "$ref" in schema:
        reference = schema["$ref"]
        if not isinstance(reference, str) or not reference.startswith("#/"):
            return [f"{path}: unsupported schema reference"]
        target: Any = root_schema
        try:
            for token in reference[2:].split("/"):
                target = target[token.replace("~1", "/").replace("~0", "~")]
        except (KeyError, TypeError):
            return [f"{path}: unresolved schema reference"]
        return validate_schema_value(value, target, root_schema, path)

    if "oneOf" in schema:
        matches = [
            not validate_schema_value(value, option, root_schema, path)
            for option in schema["oneOf"]
        ]
        return [] if sum(matches) == 1 else [f"{path}: does not match exactly one schema"]

    expected_types = schema.get("type")
    if expected_types is not None:
        choices = [expected_types] if isinstance(expected_types, str) else expected_types
        if not isinstance(choices, list) or not any(
            isinstance(item, str) and _schema_type_matches(value, item) for item in choices
        ):
            return [f"{path}: wrong JSON type"]

    if "const" in schema and not _json_equal(value, schema["const"]):
        return [f"{path}: value does not match const"]
    if "enum" in schema and not any(_json_equal(value, item) for item in schema["enum"]):
        return [f"{path}: value is outside enum"]

    errors: list[str] = []
    if isinstance(value, dict):
        properties = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in value:
                errors.append(f"{path}: missing required property {name}")
        if schema.get("additionalProperties") is False:
            errors.extend(
                f"{path}: unexpected property {name}"
                for name in value
                if name not in properties
            )
        for name, child in value.items():
            child_schema = properties.get(name)
            if isinstance(child_schema, Mapping):
                errors.extend(
                    validate_schema_value(child, child_schema, root_schema, f"{path}.{name}")
                )
    elif isinstance(value, list):
        if len(value) < int(schema.get("minItems", 0)):
            errors.append(f"{path}: too few items")
        item_schema = schema.get("items")
        if isinstance(item_schema, Mapping):
            for index, child in enumerate(value):
                errors.extend(
                    validate_schema_value(child, item_schema, root_schema, f"{path}[{index}]")
                )
    elif isinstance(value, str):
        if len(value) < int(schema.get("minLength", 0)):
            errors.append(f"{path}: string is too short")
        pattern = schema.get("pattern")
        if isinstance(pattern, str) and re.search(pattern, value) is None:
            errors.append(f"{path}: string does not match pattern")
        if schema.get("format") == "date-time":
            try:
                timestamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
                if timestamp.tzinfo is None:
                    raise ValueError
            except ValueError:
                errors.append(f"{path}: invalid date-time")
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{path}: number is below minimum")
        if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
            errors.append(f"{path}: number is not above exclusive minimum")
    return errors


def load_report_schema() -> dict[str, Any]:
    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise InvestigationError("schema_unavailable", f"Could not read report schema: {exc}") from exc
    if not isinstance(schema, dict):
        raise InvestigationError("schema_unavailable", "Report schema is not a JSON object")
    return schema


def validate_report_schema(report: Mapping[str, Any]) -> list[str]:
    schema = load_report_schema()
    return validate_schema_value(report, schema, schema)


def codex_output_schema() -> dict[str, Any]:
    """Return the canonical report schema tightened for Structured Outputs."""
    schema = copy.deepcopy(load_report_schema())

    def require_every_property(node: Any) -> None:
        if isinstance(node, dict):
            if "oneOf" in node:
                node["anyOf"] = node.pop("oneOf")
            if "const" in node:
                node["enum"] = [node.pop("const")]
            enum_values = node.get("enum")
            if "type" not in node and isinstance(enum_values, list) and enum_values:
                if all(isinstance(value, str) for value in enum_values):
                    node["type"] = "string"
                elif all(
                    isinstance(value, int) and not isinstance(value, bool)
                    for value in enum_values
                ):
                    node["type"] = "integer"
            properties = node.get("properties")
            if isinstance(properties, dict):
                canonical_required = set(node.get("required", []))
                for name, child in list(properties.items()):
                    if name not in canonical_required:
                        properties[name] = {"anyOf": [child, {"type": "null"}]}
                if len(canonical_required) != len(properties):
                    existing = str(node.get("description") or "").strip()
                    node["description"] = (
                        f"{existing} Set fields that are not applicable to null."
                    ).strip()
                node["required"] = list(properties)
            type_choices = node.get("type")
            if isinstance(type_choices, list):
                description = node.pop("description", None)
                node.pop("type")
                constraints = dict(node)
                node.clear()
                if description is not None:
                    node["description"] = description
                node["anyOf"] = [
                    (
                        {"type": choice}
                        if choice == "null"
                        else {"type": choice, **copy.deepcopy(constraints)}
                    )
                    for choice in type_choices
                ]
            for child in node.values():
                require_every_property(child)
        elif isinstance(node, list):
            for child in node:
                require_every_property(child)

    require_every_property(schema)
    return schema


def remove_transport_nulls(report: dict[str, Any]) -> None:
    """Remove hosted-only null placeholders before canonical v2 validation."""
    schema = load_report_schema()

    def resolve(node: Mapping[str, Any]) -> Mapping[str, Any]:
        reference = node.get("$ref")
        if not isinstance(reference, str) or not reference.startswith("#/"):
            return node
        target: Any = schema
        for token in reference[2:].split("/"):
            target = target[token.replace("~1", "/").replace("~0", "~")]
        return target if isinstance(target, Mapping) else node

    def normalize(value: Any, node: Mapping[str, Any]) -> None:
        node = resolve(node)
        if isinstance(value, dict):
            properties = node.get("properties")
            if not isinstance(properties, Mapping):
                return
            required = set(node.get("required", []))
            for name in list(value):
                child = properties.get(name)
                if not isinstance(child, Mapping):
                    continue
                if value[name] is None and name not in required:
                    del value[name]
                else:
                    normalize(value[name], child)
        elif isinstance(value, list):
            child = node.get("items")
            if isinstance(child, Mapping):
                for item in value:
                    normalize(item, child)

    normalize(report, schema)


def atomic_write_json(path: Path, payload: Mapping[str, Any]) -> None:
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True, allow_nan=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def safe_relative_file(base: Path, raw_path: Any) -> Path | None:
    if not isinstance(raw_path, str) or not raw_path or Path(raw_path).is_absolute():
        return None
    candidate = (base / raw_path).resolve()
    try:
        candidate.relative_to(base.resolve())
    except ValueError:
        return None
    return candidate if candidate.is_file() else None


def artifact_kind(path: Path) -> str:
    suffix = path.suffix.lower()
    if suffix in VIDEO_SUFFIXES:
        return "video"
    if suffix in {".jpg", ".jpeg", ".png", ".webp"}:
        return "image"
    if suffix in {".json", ".jsonl", ".ndjson", ".csv", ".tsv"}:
        return "structured_text"
    if suffix in {".log", ".txt", ".err", ".md"} or not suffix:
        return "text"
    return "binary"


def discover_artifacts(run_dir: Path) -> list[dict[str, Any]]:
    artifacts: list[dict[str, Any]] = []
    for path in sorted(run_dir.rglob("*")):
        if path.is_symlink():
            continue
        relative = path.relative_to(run_dir).as_posix()
        if (
            path.name in IGNORED_OUTPUTS
            or path.name.startswith(".investigation.")
            or ATTACHMENT_DIRECTORY in Path(relative).parts
            or INDEX_DIRECTORY in Path(relative).parts
        ):
            continue
        size: int | None = None
        digest: str | None = None
        status = "readable"
        try:
            metadata = path.stat()
        except OSError:
            status = "unreadable"
        else:
            if not stat.S_ISREG(metadata.st_mode):
                continue
            size = metadata.st_size
        if status == "readable":
            try:
                digest = sha256_file(path)
            except OSError:
                status = "unreadable"
        artifacts.append(
            {
                "path": relative,
                "sha256": digest,
                "size_bytes": size,
                "kind": artifact_kind(path),
                "status": status,
            }
        )
    return artifacts


def append_bounded_attachments(
    existing: list[dict[str, Any]],
    candidates: Sequence[dict[str, Any]],
    *,
    limit: int = MAX_ATTACHED_IMAGES,
) -> int:
    seen = {item["file"] for item in existing}
    unique: list[dict[str, Any]] = []
    for item in candidates:
        if item["file"] in seen:
            continue
        seen.add(item["file"])
        unique.append(item)
    available = max(0, min(limit, MAX_ATTACHED_IMAGES) - len(existing))
    existing.extend(unique[:available])
    return max(0, len(unique) - available)


def _atomic_copy(source: Path, destination: Path) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output, source.open("rb") as source_handle:
            shutil.copyfileobj(source_handle, output)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, destination)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def attachment_root(run_dir: Path, *, create: bool) -> Path | None:
    resolved_run = run_dir.resolve()
    root = resolved_run / ATTACHMENT_DIRECTORY
    if root.is_symlink():
        raise InvestigationError(
            "attachment_directory_invalid",
            f"{ATTACHMENT_DIRECTORY} must not be a symlink",
        )
    if create:
        try:
            root.mkdir(exist_ok=True)
        except OSError as exc:
            raise InvestigationError(
                "attachment_directory_invalid",
                f"Could not create {ATTACHMENT_DIRECTORY}: {type(exc).__name__}",
            ) from exc
    elif not root.exists():
        return None
    if not root.is_dir() or root.is_symlink() or root.resolve() != resolved_run / ATTACHMENT_DIRECTORY:
        raise InvestigationError(
            "attachment_directory_invalid",
            f"{ATTACHMENT_DIRECTORY} is not a contained directory",
        )
    return root


def persist_attachment_manifest(
    run_dir: Path, attachments: Sequence[Mapping[str, Any]]
) -> list[dict[str, Any]]:
    root = attachment_root(run_dir, create=True)
    assert root is not None
    manifest: list[dict[str, Any]] = []
    for index, item in enumerate(attachments, 1):
        image = item.get("file")
        metadata = item.get("manifest")
        if not isinstance(image, Path) or not image.is_file() or not isinstance(metadata, Mapping):
            raise InvestigationError("attachment_invalid", f"Attachment {index} is unavailable")
        digest = sha256_file(image)
        suffix = image.suffix.lower() if image.suffix.lower() in {".jpg", ".jpeg", ".png"} else ".jpg"
        relative = f"{ATTACHMENT_DIRECTORY}/{digest}{suffix}"
        destination = root / f"{digest}{suffix}"
        if not destination.is_file() or sha256_file(destination) != digest:
            _atomic_copy(image, destination)
        manifest.append(
            {
                **copy.deepcopy(dict(metadata)),
                "attachment_index": index,
                "sheet_path": relative,
                "sheet_sha256": digest,
            }
        )
    return manifest


def prune_attachment_files(
    run_dir: Path, attachment_manifest: Sequence[Mapping[str, Any]]
) -> list[str]:
    try:
        root = attachment_root(run_dir, create=False)
    except InvestigationError as exc:
        return [f"{exc.code}: {exc}"]
    if root is None:
        return []
    retained = {
        Path(str(item.get("sheet_path"))).name
        for item in attachment_manifest
        if Path(str(item.get("sheet_path"))).parent.as_posix() == ATTACHMENT_DIRECTORY
    }
    errors: list[str] = []
    try:
        paths = list(root.iterdir())
    except OSError as exc:
        return [f"attachment directory unreadable: {type(exc).__name__}"]
    for path in paths:
        try:
            if (
                path.is_symlink()
                or not path.is_file()
                or not re.fullmatch(r"[0-9a-f]{64}\.(?:jpg|jpeg|png)", path.name)
                or path.name in retained
            ):
                continue
            path.unlink()
        except OSError as exc:
            errors.append(f"{path.name}: {type(exc).__name__}")
    return errors


def bounded_video_paths(paths: Sequence[str], remaining_run_budget: int) -> tuple[list[str], list[str]]:
    limit = min(MAX_VIDEOS_PER_PASS, max(0, remaining_run_budget))
    return list(paths[:limit]), list(paths[limit:])


def round_robin_attachments(
    groups: Sequence[Sequence[dict[str, Any]]],
) -> list[dict[str, Any]]:
    return [
        group[index]
        for index in range(max((len(group) for group in groups), default=0))
        for group in groups
        if index < len(group)
    ]


def attachment_limit_for_pass(pass_number: int, max_video_passes: int) -> int:
    if pass_number == 1 and max_video_passes > 1:
        return MAX_INITIAL_IMAGES
    return MAX_ATTACHED_IMAGES


def image_attachment_limitations(
    initial_omitted: int,
    global_omitted: int,
    attached_count: int,
) -> list[tuple[str, str]]:
    limitations: list[tuple[str, str]] = []
    if initial_omitted:
        reserved = MAX_ATTACHED_IMAGES - MAX_INITIAL_IMAGES
        unused = max(0, MAX_ATTACHED_IMAGES - attached_count)
        message = (
            f"{initial_omitted} initial candidate image(s) were not attached after the "
            f"{MAX_INITIAL_IMAGES}-image first-pass allocation; {reserved} slots were "
            "reserved for model-requested intervals"
        )
        if unused:
            message += f", and {unused} reserved slot(s) remained unused at publication"
        limitations.append(("initial_image_allocation", message))
    if global_omitted:
        limitations.append(
            (
                "image_attachment_limit",
                f"{global_omitted} generated image(s) were not attached after the global "
                f"{MAX_ATTACHED_IMAGES}-image limit",
            )
        )
    return limitations


def git_text(*arguments: str) -> str | None:
    process = subprocess.run(
        ["git", *arguments],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return process.stdout.strip() if process.returncode == 0 else None


@lru_cache(maxsize=256)
def canonical_commit(revision: str) -> str | None:
    if (
        not revision
        or revision.startswith("-")
        or not re.fullmatch(r"[A-Za-z0-9._/-]{1,128}", revision)
    ):
        return None
    resolved = git_text("rev-parse", "--verify", f"{revision}^{{commit}}")
    return resolved if resolved and re.fullmatch(r"[0-9a-f]{40,64}", resolved) else None


def git_file_hash(revision: str, path: str) -> str | None:
    process = subprocess.run(
        ["git", "show", f"{revision}:{path}"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return sha256_bytes(process.stdout) if process.returncode == 0 else None


def recorded_files(payload: Any) -> dict[str, str]:
    found: dict[str, str] = {}

    def visit(value: Any) -> None:
        if isinstance(value, Mapping):
            files = value.get("files")
            if isinstance(files, list):
                for item in files:
                    if not isinstance(item, Mapping):
                        continue
                    path = item.get("path")
                    digest = item.get("sha256")
                    if isinstance(path, str) and isinstance(digest, str):
                        found[path] = digest
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(payload)
    return found


def source_context(run_dir: Path, artifacts: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    head = git_text("rev-parse", "HEAD")
    worktree_status = git_text("status", "--porcelain")
    identities: list[dict[str, Any]] = []
    hash_cache: dict[Path, str] = {}
    git_hash_cache: dict[tuple[str, str], str | None] = {}
    for artifact in artifacts:
        relative = str(artifact.get("path", ""))
        if Path(relative).name != "identity.json":
            continue
        identity_path = safe_relative_file(run_dir, relative)
        if identity_path is None:
            continue
        try:
            payload = json.loads(identity_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            identities.append({"path": relative, "error": type(exc).__name__})
            continue
        traceability = payload.get("traceability", {})
        expected_files = recorded_files(payload)
        matched = 0
        mismatched: list[str] = []
        unavailable: list[str] = []
        for source_path, expected_hash in expected_files.items():
            current = safe_relative_file(ROOT, source_path)
            if current is None:
                unavailable.append(source_path)
                continue
            actual_hash = hash_cache.setdefault(current, sha256_file(current))
            if actual_hash == expected_hash:
                matched += 1
            else:
                mismatched.append(source_path)
        recorded_revision = traceability.get("repository_sha")
        commit_available = bool(
            isinstance(recorded_revision, str)
            and git_text("cat-file", "-e", f"{recorded_revision}^{{commit}}") == ""
        )
        commit_matched = 0
        commit_mismatched: list[str] = []
        if commit_available and isinstance(recorded_revision, str):
            for source_path, expected_hash in expected_files.items():
                key = (recorded_revision, source_path)
                actual_hash = git_hash_cache.setdefault(
                    key, git_file_hash(recorded_revision, source_path)
                )
                if actual_hash == expected_hash:
                    commit_matched += 1
                else:
                    commit_mismatched.append(source_path)
        if expected_files and matched == len(expected_files):
            suggested_basis = "exact"
        elif expected_files and commit_matched == len(expected_files):
            suggested_basis = "commit_reconstructed"
        elif expected_files:
            suggested_basis = "current_only"
        else:
            suggested_basis = "current_only" if head else "unavailable"
        identities.append(
            {
                "path": relative,
                "recorded_revision": recorded_revision,
                "recorded_ref": traceability.get("repository_ref"),
                "recorded_worktree_clean": traceability.get("worktree_clean"),
                "recorded_product_fingerprint": payload.get("product_fingerprint"),
                "recorded_file_count": len(expected_files),
                "current_files_matched": matched,
                "current_files_mismatched": mismatched[:50],
                "current_files_unavailable": unavailable[:50],
                "recorded_commit_available": commit_available,
                "commit_files_matched": commit_matched,
                "commit_files_mismatched": commit_mismatched[:50],
                "suggested_basis": suggested_basis,
            }
        )
    return {
        "current_head": head,
        "current_worktree_clean": worktree_status == "" if worktree_status is not None else None,
        "identities": identities,
    }


def recorded_identity_revisions(source_context_value: Mapping[str, Any]) -> list[str]:
    revisions: list[str] = []
    for identity in source_context_value.get("identities", []):
        if not isinstance(identity, Mapping):
            continue
        recorded = identity.get("recorded_revision")
        canonical = canonical_commit(recorded) if isinstance(recorded, str) else None
        normalized = (
            canonical
            if canonical is not None
            else recorded
            if isinstance(recorded, str) and re.fullmatch(r"[0-9a-f]{40,64}", recorded)
            else None
        )
        if normalized is not None and normalized not in revisions:
            revisions.append(normalized)
    return revisions


def recorded_source_revisions(source_context_value: Mapping[str, Any]) -> list[str]:
    return [
        canonical
        for revision in recorded_identity_revisions(source_context_value)
        if (canonical := canonical_commit(revision)) is not None
    ]


def model_source_context(source_context_value: Mapping[str, Any]) -> dict[str, Any]:
    """Expose recorded identity trust without revealing later-worktree or history leads."""
    identities: list[dict[str, Any]] = []
    for identity in source_context_value.get("identities", []):
        if not isinstance(identity, Mapping):
            continue
        if identity.get("error") is not None:
            identities.append(
                {
                    "path": identity.get("path"),
                    "error": identity.get("error"),
                }
            )
            continue
        recorded = identity.get("recorded_revision")
        canonical = canonical_commit(recorded) if isinstance(recorded, str) else None
        recorded_identity = (
            canonical
            if canonical is not None
            else recorded
            if isinstance(recorded, str) and re.fullmatch(r"[0-9a-f]{40,64}", recorded)
            else None
        )
        commit_mismatched = identity.get("commit_files_mismatched")
        commit_unavailable = identity.get("commit_files_unavailable")
        identities.append(
            {
                "path": identity.get("path"),
                "recorded_revision": recorded_identity,
                "recorded_worktree_clean": identity.get("recorded_worktree_clean"),
                "recorded_product_fingerprint": identity.get(
                    "recorded_product_fingerprint"
                ),
                "recorded_file_count": identity.get("recorded_file_count"),
                "recorded_commit_available": identity.get("recorded_commit_available"),
                "recorded_commit_files_matched": identity.get("commit_files_matched"),
                "recorded_commit_files_mismatched": (
                    len(commit_mismatched) if isinstance(commit_mismatched, list) else None
                ),
                "recorded_commit_files_unavailable": (
                    len(commit_unavailable) if isinstance(commit_unavailable, list) else None
                ),
                "suggested_basis": identity.get("suggested_basis"),
            }
        )
    return {
        "recorded_revisions": recorded_identity_revisions(source_context_value),
        "identities": identities,
    }


def conservative_source_basis(source_context_value: Mapping[str, Any]) -> str:
    identities = [
        item
        for item in source_context_value.get("identities", [])
        if isinstance(item, Mapping)
    ]
    if not identities:
        return "current_only" if source_context_value.get("current_head") else "unavailable"
    fallback = "current_only" if source_context_value.get("current_head") else "unavailable"
    bases = [
        str(item.get("suggested_basis"))
        if item.get("suggested_basis") in SOURCE_BASIS_RANK
        else fallback
        for item in identities
    ]
    return min(bases, key=SOURCE_BASIS_RANK.__getitem__)


def load_video_helper() -> Any:
    bench_scripts = ROOT / "scripts" / "bench"
    sys.path.insert(0, str(bench_scripts))
    try:
        from investigation_video import inspect_video
    finally:
        try:
            sys.path.remove(str(bench_scripts))
        except ValueError:
            pass
    return inspect_video


def build_evidence_index(run_dir: Path) -> dict[str, Any]:
    import bench_evidence

    return bench_evidence.compact_manifest(bench_evidence.build_run_index(run_dir))


def extract_video_evidence(
    run_dir: Path,
    artifacts: Sequence[Mapping[str, Any]],
    workspace: Path,
    requests: Sequence[Mapping[str, Any]],
    *,
    scan_overview: bool,
    pass_number: int,
    remaining_run_budget: int,
    index_dir: Path | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], int, list[str]]:
    inspect_video = load_video_helper()
    artifact_hashes = {
        str(item.get("path")): item.get("sha256")
        for item in artifacts
        if isinstance(item.get("path"), str)
    }
    by_video: dict[str, list[dict[str, float]]] = {}
    for request in requests:
        path = request.get("path")
        start = request.get("start_pts_s")
        end = request.get("end_pts_s")
        rate = request.get("sample_fps")
        if not isinstance(path, str):
            continue
        if not all(isinstance(value, (int, float)) and not isinstance(value, bool) for value in (start, end, rate)):
            continue
        by_video.setdefault(path, []).append(
            {
                "start_seconds": float(start),
                "end_seconds": float(end),
                "sample_rate_hz": float(rate),
            }
        )

    results: list[dict[str, Any]] = []
    overview_attachments: list[dict[str, Any]] = []
    requested_attachment_groups: list[list[dict[str, Any]]] = []
    change_attachment_groups: list[list[dict[str, Any]]] = []
    if scan_overview:
        all_video_paths = [
            str(item["path"]) for item in artifacts if item.get("kind") == "video"
        ]
        auxiliary_paths = [
            path
            for path in all_video_paths
            if Path(path).name in AUXILIARY_OVERVIEW_VIDEO_NAMES
        ]
        candidate_paths = [
            path for path in all_video_paths if path not in auxiliary_paths
        ]
        results.extend(
            {
                "path": relative,
                "status": "not_processed",
                "error": "hidden_auxiliary_video",
            }
            for relative in auxiliary_paths
        )
    else:
        candidate_paths = list(by_video)
    video_paths, omitted_paths = bounded_video_paths(candidate_paths, remaining_run_budget)
    results.extend(
        {
            "path": relative,
            "status": "not_processed",
            "error": "video_limit",
        }
        for relative in omitted_paths
    )
    for index, relative in enumerate(video_paths):
        video = safe_relative_file(run_dir, relative)
        if video is None:
            results.append({"path": relative, "status": "failed", "error": "video_missing"})
            continue
        recorded_video_sha256 = artifact_hashes.get(relative)
        source_video_sha256 = sha256_file(video)
        if (
            not isinstance(recorded_video_sha256, str)
            or source_video_sha256 != recorded_video_sha256
        ):
            results.append(
                {"path": relative, "status": "failed", "error": "video_hash_changed"}
            )
            continue
        output = workspace / f"video_{index:03d}"
        try:
            evidence = inspect_video(
                video,
                output,
                by_video.get(relative, []),
                index_dir=index_dir,
                video_sha256=source_video_sha256,
                scan_overview=scan_overview,
            )
        except Exception as exc:  # preserve one broken video without losing the run
            results.append(
                {"path": relative, "status": "failed", "error": type(exc).__name__}
            )
            continue
        evidence["path"] = relative
        results.append(evidence)
        requested_attachments: list[dict[str, Any]] = []
        change_attachments: list[dict[str, Any]] = []
        sheets = [evidence.get("overview")]
        sheets.extend(evidence.get("requested_intervals", []))
        sheets.extend(evidence.get("change_images", []))
        for sheet in sheets:
            if not isinstance(sheet, Mapping) or sheet.get("status") not in {"complete", "partial"}:
                continue
            image = safe_relative_file(output, sheet.get("filename"))
            if image is None:
                continue
            purpose = sheet.get("purpose", "unspecified")
            interval = sheet.get("interval")
            layout = sheet.get("layout")
            cells = sheet.get("cells")
            if (
                not isinstance(interval, Mapping)
                or not isinstance(layout, Mapping)
                or not isinstance(cells, list)
            ):
                continue
            attachment = (
                {
                    "file": image,
                    "manifest": {
                        "pass": pass_number,
                        "source_video_path": relative,
                        "source_video_sha256": source_video_sha256,
                        "purpose": purpose,
                        "interval": copy.deepcopy(dict(interval)),
                        "layout": copy.deepcopy(dict(layout)),
                        "cells": copy.deepcopy(cells),
                    },
                }
            )
            if purpose == "whole_video_overview":
                overview_attachments.append(attachment)
            elif purpose == "requested_interval":
                requested_attachments.append(attachment)
            else:
                change_attachments.append(attachment)
        if requested_attachments:
            requested_attachment_groups.append(requested_attachments)
        if change_attachments:
            change_attachment_groups.append(change_attachments)
    attachments = (
        overview_attachments
        + round_robin_attachments(requested_attachment_groups)
        + round_robin_attachments(change_attachment_groups)
    )
    return results, attachments, len(video_paths), omitted_paths


def event_to_video_bridges(
    run_dir: Path,
    artifacts: Sequence[Mapping[str, Any]],
    evidence_index: Mapping[str, Any] | None,
) -> list[dict[str, Any]]:
    """Return only recorded, verified, one-to-one event-to-video bridges."""
    if not isinstance(evidence_index, Mapping):
        return []

    artifact_hashes = {
        str(item["path"]): item.get("sha256")
        for item in artifacts
        if isinstance(item, Mapping)
        and isinstance(item.get("path"), str)
        and item.get("status") == "readable"
    }

    def indexed_json(relative: str) -> Mapping[str, Any] | None:
        expected_sha256 = artifact_hashes.get(relative)
        unresolved = run_dir / relative
        path = safe_relative_file(run_dir, relative)
        if (
            not isinstance(expected_sha256, str)
            or unresolved.is_symlink()
            or path is None
        ):
            return None
        try:
            if sha256_file(path) != expected_sha256:
                return None
        except OSError:
            return None
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            return None
        return payload if isinstance(payload, Mapping) else None

    def finite_number(value: Any) -> float | None:
        if (
            not isinstance(value, (int, float))
            or isinstance(value, bool)
            or not math.isfinite(float(value))
        ):
            return None
        return float(value)

    def verified_timing(payload: Any, frame_count: int) -> bool:
        if not isinstance(payload, Mapping) or payload.get("status") != "verified":
            return False
        if any(
            not isinstance(payload.get(field), int)
            or isinstance(payload.get(field), bool)
            or payload.get(field) != frame_count
            for field in (
                "source_frame_count",
                "written_frame_count",
                "encoded_frame_count",
            )
        ):
            return False
        return all(
            isinstance(payload.get(field), int)
            and not isinstance(payload.get(field), bool)
            and payload.get(field) == 0
            for field in (
                "capture_drop_count",
                "writer_drop_count",
                "timestamp_error_count",
                "missing_encoded_frame_count",
                "extra_encoded_frame_count",
                "duration_mismatch_count",
            )
        )

    def sibling_path(parent: Path, raw: Any) -> str | None:
        if not isinstance(raw, str) or not raw:
            return None
        relative = Path(raw)
        if relative.is_absolute() or ".." in relative.parts:
            return None
        return (parent / relative).as_posix()

    videos = [
        item
        for item in evidence_index.get("videos", [])
        if isinstance(item, Mapping)
        and isinstance(item.get("path"), str)
        and isinstance(item.get("frame_index"), Mapping)
    ]
    timing_files = [
        item
        for item in evidence_index.get("files", [])
        if isinstance(item, Mapping)
        and isinstance(item.get("path"), str)
        and isinstance(item.get("recorded_clocks"), Mapping)
        and isinstance(item["recorded_clocks"].get("video_pts_s"), Mapping)
    ]
    candidates_by_video: dict[str, list[Mapping[str, Any]]] = {}
    videos_by_timing: dict[str, list[str]] = {}
    for video in videos:
        video_path = str(video["path"])
        frame_index = video["frame_index"]
        assert isinstance(frame_index, Mapping)
        frame_count = frame_index.get("frame_count")
        first_pts = finite_number(frame_index.get("first_pts_seconds"))
        last_pts = finite_number(frame_index.get("last_pts_seconds"))
        if (
            not isinstance(frame_count, int)
            or isinstance(frame_count, bool)
            or frame_count < 1
            or first_pts is None
            or last_pts is None
        ):
            candidates_by_video[video_path] = []
            continue
        matches: list[Mapping[str, Any]] = []
        for timing in timing_files:
            clocks = timing["recorded_clocks"]
            assert isinstance(clocks, Mapping)
            pts = clocks["video_pts_s"]
            assert isinstance(pts, Mapping)
            timing_first = finite_number(pts.get("minimum"))
            timing_last = finite_number(pts.get("maximum"))
            if (
                Path(str(timing["path"])).parent == Path(video_path).parent
                and pts.get("count") == frame_count
                and isinstance(timing.get("record_count"), int)
                and not isinstance(timing.get("record_count"), bool)
                and isinstance(timing.get("host_time_mapped_count"), int)
                and not isinstance(timing.get("host_time_mapped_count"), bool)
                and timing["host_time_mapped_count"] == timing["record_count"]
                and timing_first is not None
                and timing_last is not None
                and math.isclose(timing_first, first_pts, rel_tol=0, abs_tol=1e-6)
                and math.isclose(timing_last, last_pts, rel_tol=0, abs_tol=1e-6)
            ):
                matches.append(timing)
                videos_by_timing.setdefault(str(timing["path"]), []).append(video_path)
        candidates_by_video[video_path] = matches

    bridges: list[dict[str, Any]] = []
    for video in videos:
        video_path = str(video["path"])
        frame_index = video["frame_index"]
        assert isinstance(frame_index, Mapping)
        frame_count = frame_index.get("frame_count")
        matches = candidates_by_video.get(video_path, [])
        if (
            not isinstance(frame_count, int)
            or isinstance(frame_count, bool)
            or len(matches) != 1
            or len(videos_by_timing.get(str(matches[0]["path"]), [])) != 1
        ):
            bridges.append(
                {
                    "video_path": video_path,
                    "status": "unavailable",
                    "limitation": (
                        "no globally one-to-one raw timing match with equal video-PTS "
                        "count and bounds"
                    ),
                }
            )
            continue

        timing = matches[0]
        timing_path = str(timing["path"])
        parent = Path(video_path).parent
        camera_result_path = (parent / "camera_result.json").as_posix()
        camera_result = indexed_json(camera_result_path)
        verification_path = sibling_path(
            parent,
            camera_result.get("video_timing_verification")
            if isinstance(camera_result, Mapping)
            else None,
        )
        verification = indexed_json(verification_path) if verification_path else None
        declared_video = sibling_path(
            parent, camera_result.get("video") if isinstance(camera_result, Mapping) else None
        )
        declared_timing = sibling_path(
            parent,
            camera_result.get("frame_timing")
            if isinstance(camera_result, Mapping)
            else None,
        )
        embedded_verification = (
            camera_result.get("video_timing_verification_result")
            if isinstance(camera_result, Mapping)
            else None
        )
        frame_margin_ns = (
            verification.get("maximum_source_interval_ns")
            if isinstance(verification, Mapping)
            else None
        )
        embedded_frame_margin_ns = (
            embedded_verification.get("maximum_source_interval_ns")
            if isinstance(embedded_verification, Mapping)
            else None
        )
        if (
            declared_video != video_path
            or declared_timing != timing_path
            or verification_path is None
            or not verified_timing(embedded_verification, frame_count)
            or not verified_timing(verification, frame_count)
            or not isinstance(frame_margin_ns, int)
            or isinstance(frame_margin_ns, bool)
            or frame_margin_ns < 1
            or embedded_frame_margin_ns != frame_margin_ns
        ):
            bridges.append(
                {
                    "video_path": video_path,
                    "timing_path": timing_path,
                    "status": "unavailable",
                    "limitation": (
                        "recorded camera pairing or zero-error video timing verification "
                        "is unavailable"
                    ),
                }
            )
            continue

        bridges.append(
            {
                "video_path": video_path,
                "timing_path": timing_path,
                "status": "verified",
                "verification_path": verification_path,
                "frame_count": frame_count,
                "first_pts_seconds": frame_index.get("first_pts_seconds"),
                "last_pts_seconds": frame_index.get("last_pts_seconds"),
                "frame_margin_ns": frame_margin_ns,
                "pairing_basis": (
                    "globally one-to-one frame count and PTS bounds plus recorded "
                    "zero-error timing verification"
                ),
                "event_clock_to_host_query": (
                    "records --path EVENT_PATH --clock EVENT_CLOCK --start VALUE "
                    "--end VALUE [--clock-segment ID] [--segment-instance N] --limit 100"
                ),
                "host_interval_basis": (
                    "query.clock_conversion.host_earliest_ns through "
                    "query.clock_conversion.host_latest_ns"
                ),
                "timing_host_window_query": (
                    "records --path TIMING_PATH --clock host_monotonic_ns "
                    "--start HOST_EARLIEST_NS_MINUS_FRAME_MARGIN --end "
                    "HOST_LATEST_NS_PLUS_FRAME_MARGIN --limit 100"
                ),
                "pagination": (
                    "for event, timing, and native-frame queries, while truncated is "
                    "true, repeat the same bounded query with --offset next_offset; "
                    "do not derive or cite an interval until next_offset is null"
                ),
                "returned_time_basis": "query.clock_conversion and results[].host_time",
                "returned_video_pts_basis": (
                    "PTS-bearing rows only: results[].record.video_pts_value / "
                    "results[].record.video_pts_timescale"
                ),
                "native_frame_query": (
                    "frames --path VIDEO_PATH --start PTS --end PTS --limit 100"
                ),
            }
        )
    return bridges


def compact_context(
    run_dir: Path,
    artifacts: Sequence[Mapping[str, Any]],
    source: Mapping[str, Any],
    video: Sequence[Mapping[str, Any]],
    prior_report: Mapping[str, Any] | None,
    pass_number: int,
    image_attachments: Mapping[str, Any],
    evidence_index: Mapping[str, Any] | None = None,
) -> str:
    try:
        run_label = run_dir.relative_to(ROOT).as_posix()
    except ValueError:
        run_label = str(run_dir)
    compact_video = copy.deepcopy(list(video))
    for item in compact_video:
        if not isinstance(item, dict):
            continue
        containers = [item.get("temporal_scan")]
        coverage = item.get("coverage")
        if isinstance(coverage, dict):
            containers.append(coverage.get("full_frame_scan"))
        for container in containers:
            if not isinstance(container, dict):
                continue
            points = container.pop("sampled_pts_seconds", None)
            if not isinstance(points, list):
                continue
            numeric = sorted(
                {
                    float(value)
                    for value in points
                    if isinstance(value, (int, float))
                    and not isinstance(value, bool)
                    and math.isfinite(float(value))
                }
            )
            cadence: float | None = None
            raw_cadence = container.get("nominal_cadence_seconds")
            if (
                isinstance(raw_cadence, (int, float))
                and not isinstance(raw_cadence, bool)
                and math.isfinite(float(raw_cadence))
                and float(raw_cadence) > 0
            ):
                cadence = float(raw_cadence)
            raw_rate = container.get("sample_rate_hz")
            if cadence is None and (
                isinstance(raw_rate, (int, float))
                and not isinstance(raw_rate, bool)
                and math.isfinite(float(raw_rate))
                and float(raw_rate) > 0
            ):
                cadence = 1.0 / float(raw_rate)
            gaps = [
                (start, end, end - start)
                for start, end in zip(numeric, numeric[1:])
                if end > start
            ]
            if cadence is None and gaps:
                cadence = min(gap for _start, _end, gap in gaps)
            threshold = cadence * 1.5 if cadence is not None else math.inf
            anomalies = [
                {"start_seconds": start, "end_seconds": end, "gap_seconds": gap}
                for start, end, gap in gaps
                if gap > threshold + 1e-9
            ]
            retained_anomalies = sorted(
                sorted(
                    anomalies,
                    key=lambda item: (-item["gap_seconds"], item["start_seconds"]),
                )[:MAX_SAMPLED_GAP_ANOMALIES],
                key=lambda item: item["start_seconds"],
            )
            for anomaly in retained_anomalies:
                for key in ("start_seconds", "end_seconds", "gap_seconds"):
                    anomaly[key] = round(anomaly[key], 6)
            container["sampled_pts_summary"] = {
                "count": len(numeric),
                "first_seconds": round(numeric[0], 6) if numeric else None,
                "last_seconds": round(numeric[-1], 6) if numeric else None,
                "nominal_cadence_seconds": round(cadence, 6) if cadence is not None else None,
                "gap_anomaly_count": len(anomalies),
                "gap_anomalies": retained_anomalies,
                "gap_anomalies_omitted_from_prompt": max(
                    0, len(anomalies) - len(retained_anomalies)
                ),
                "explicit_regular_points_omitted_from_prompt": len(numeric),
            }

    context: dict[str, Any] = {
        "run_directory": run_label,
        "artifact_inventory": list(artifacts),
        "source_precheck": model_source_context(source),
        "video_extraction": compact_video,
        "investigation_pass": pass_number,
        "image_attachments": dict(image_attachments),
        "evidence_query": {
            "argv_prefix": ["python3", "tools/bench_evidence.py"],
            "run_directory": run_label,
            "query_subcommands": ["list", "records", "frames", "source"],
            "index_is_finding_aid_only": True,
            "record_query_capabilities": {
                "literal_predicate": "--where field=value",
                "same_record_field_comparison": "--compare left!=right",
                "adjacent_raw_context": "--context N",
                "time_window": (
                    "--clock CLOCK --start VALUE --end VALUE "
                    "[--clock-segment ID] [--segment-instance N]"
                ),
                "common_host_output": "query.clock_conversion and results[].host_time",
            },
            "event_to_video_bridges": event_to_video_bridges(
                run_dir, artifacts, evidence_index
            ),
            "investigation_priority": {
                "lead_ranking": "raw_cross_source_ordering_before_video",
                "class_selector_minimum": "access_check_only",
                "video_change_score": "whole_frame_pixel_difference_only_not_defect_priority",
            },
            "summary": dict(evidence_index or {}),
        },
    }
    if prior_report is not None:
        context["prior_report_to_recheck_and_improve"] = prior_report
    return json.dumps(context, indent=2, sort_keys=True, allow_nan=False)


def build_prompt(context: str, image_count: int, pass_number: int) -> str:
    bridge_direction = (
        "Before synthesis, complete one bounded event-to-frame bridge for the "
        "highest-ranked time-localized candidate, even if you ultimately reject it. "
        "Use only a status=verified event_to_video_bridges entry. First query the "
        "candidate's own EVENT_PATH in its EVENT_CLOCK to obtain the complete host "
        "interval from query.clock_conversion.host_earliest_ns through host_latest_ns "
        "and preserve the conversion and its uncertainty. Then query "
        "the raw timing_path in host_monotonic_ns across that interval expanded on each "
        "side by exactly the entry's recorded frame_margin_ns, preserve every "
        "results[].host_time interval, and consume every page through next_offset. "
        "Never derive an enclosing video PTS from truncated results; report truncation "
        "as an exact missing edge instead. Derive PTS only from the returned raw timing "
        "records, then query every native frame row across that PTS interval, consuming "
        "next_offset from the native frames query until null too, and request a bounded "
        "video interval "
        "when supplied cells do not cover it. Cite the candidate record, clock mapping, "
        "recorded timing verification, raw timing records, and reviewed attached cells. "
        "If an edge is unavailable, name that exact missing edge in coverage notes. This "
        "is one top-candidate causal bridge, not an artifact-review quota. "
    )
    if pass_number == 1:
        pass_direction = (
            "This is the evidence-mapping pass. Use the bounded evidence query CLI to "
            "inspect the recorded timeline, trace, commits, stimulus, metrics, and logs; "
            "before synthesis, obtain a bounded raw record and resolvable raw selector "
            "from every available record-backed evidence class, obtain a resolvable "
            "attached-cell video selector for physical evidence, and explicitly name "
            "any unavailable class in coverage notes. "
            "Treat that per-class selector floor only as an access check. Complete "
            "run-spanning raw-record ordering triage before selecting the strongest "
            "cross-source candidates; copy each returned record selector unchanged. "
            "Compare generic magnitudes, outliers, adjacent-stage ordering, and raw-versus-"
            "derived agreement across those sources. "
            + bridge_direction
            + "For the strongest cross-source "
            "anomalies, reconstruct the supported stimulus-to-physical-frame chain and "
            "inspect owning code at the recorded revision. Semantically review every "
            "supplied top-change window, or identify each unreviewed rank and interval in "
            "coverage notes. Do not transcribe the inventory: the runner will add every "
            "model-omitted artifact as skipped and mark the published report partial."
        )
    else:
        pass_direction = (
            "This is the synthesis pass. Recheck the prior report, inspect the additional "
            "queried raw artifacts, native-rate frames, and owning code needed to test its "
            "leads. Complete run-spanning raw-record ordering triage before selecting the "
            "strongest cross-source anomalies, and copy each returned record selector "
            "unchanged. Complete their causal chains, "
            + bridge_direction
            + "Recheck and cite the completed bridge, "
            "check counterevidence, and account for every indexed top-change window. "
            "Improve or reject prior leads and return one replacement schema-valid report "
            "without transcribing the artifact inventory."
        )
    return f"""Read tools/bench_investigator_prompt.md completely, then investigate the run below.

You are in a purpose-specific read-only evidence session. Do not modify files or run repository setup, index builds, tests, CI, release, or publication workflows. For large recorded artifacts, run `python3 tools/bench_evidence.py list <run>`, `python3 tools/bench_evidence.py records <run>`, or `python3 tools/bench_evidence.py frames <run>` with the exact runner-provided run directory; do not stream those files with cat, sed, tail, or equivalent commands. Run `python3 tools/bench_evidence.py source --revision <recorded-revision> --path <path> --line-start <line> --line-end <line>` for exact-revision source slices. Do not run Git commands directly or inspect the current worktree, later revisions, commit messages, or later history. Index rows and summaries are finding aids only: every published artifact selector must target the raw run-relative artifact returned by a query. The {image_count} attached image(s), if any, are generic whole-video overview/change or requested-interval contact sheets. The one-based image_attachments.manifest is runner-owned and directly records each durable sheet hash, canonical source-video hash, represented interval, cells, measured source positions where available, and timing uncertainty. A prior report means this is a fresh stateless follow-up: recheck it against all evidence and replace it with a better final report.

{pass_direction}

Runner-provided context:
{context}

Return only the final JSON matching tools/bench_investigation.schema.json. Do not claim exhaustive coverage or completeness, add a verdict, or turn missing evidence into a defect. Use video_requests only for bounded intervals that another pass could answer.
"""


def codex_version(executable: str) -> str:
    try:
        process = subprocess.run(
            [executable, "--version"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    return process.stdout.strip() or "unknown"


def resolve_codex_executable() -> str:
    configured = os.environ.get("BENCH_INVESTIGATOR_CODEX")
    if configured:
        return configured
    discovered = shutil.which("codex")
    if discovered:
        return discovered
    for candidate in BUNDLED_CODEX_EXECUTABLES:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return "codex"


def is_ollama_cloud_model(model: str) -> bool:
    lowered = model.casefold()
    return lowered.endswith(":cloud") or lowered.endswith("-cloud")


def _replace_private_paths(text: str, replacements: Sequence[tuple[str, str]]) -> str:
    for private, label in sorted(set(replacements), key=lambda item: -len(item[0])):
        if private:
            text = text.replace(private, label)
    return text


def redact_report_paths(
    value: Any,
    run_dir: Path,
    private_paths: Sequence[Path] = (),
) -> Any:
    replacements_by_path: dict[str, str] = {}
    for path, label in (
        (run_dir, "<run>"),
        (ROOT, "<repo>"),
        (Path.home(), "<home>"),
        (Path(tempfile.gettempdir()), "<temporary>"),
        (Path("/tmp"), "<temporary>"),
        *((path, "<temporary>") for path in private_paths),
    ):
        replacements_by_path.setdefault(str(path), label)
        replacements_by_path.setdefault(str(path.resolve()), label)
    replacements = list(replacements_by_path.items())

    def visit(item: Any) -> Any:
        if isinstance(item, str):
            return _replace_private_paths(item, replacements)
        if isinstance(item, list):
            return [visit(child) for child in item]
        if isinstance(item, dict):
            return {key: visit(child) for key, child in item.items()}
        return item

    return visit(value)


def sanitize_investigation_report(value: Any, run_dir: Path) -> Any:
    bench_scripts = ROOT / "scripts" / "bench"
    sys.path.insert(0, str(bench_scripts))
    try:
        from artifact_privacy import sanitize_artifact_value
    finally:
        try:
            sys.path.remove(str(bench_scripts))
        except ValueError:
            pass
    return sanitize_artifact_value(value, run_dir=run_dir)


def validate_published_attachments(run_dir: Path, report: Mapping[str, Any]) -> list[str]:
    """Re-resolve durable visual evidence after the final privacy transform."""
    coverage = report.get("coverage")
    attachments = coverage.get("attachments") if isinstance(coverage, Mapping) else None
    if not isinstance(attachments, list):
        return ["published attachment inventory is missing"]
    errors: list[str] = []
    for index, attachment in enumerate(attachments, start=1):
        if not isinstance(attachment, Mapping):
            errors.append(f"published attachment {index} is malformed")
            continue
        for path_field, hash_field, label in (
            ("sheet_path", "sheet_sha256", "sheet"),
            ("source_video_path", "source_video_sha256", "source video"),
        ):
            path = safe_relative_file(run_dir, attachment.get(path_field))
            try:
                actual_hash = sha256_file(path) if path is not None else None
            except OSError:
                actual_hash = None
            if actual_hash != attachment.get(hash_field):
                errors.append(
                    f"published attachment {index} {label} no longer resolves"
                )
    return errors


def sanitize_error(text: str, private_paths: Sequence[Path] = ()) -> str:
    replacements = [(str(ROOT), "<repo>"), (str(Path.home()), "<home>")]
    for path in private_paths:
        replacements.extend(((str(path), "<private>"), (str(path.resolve()), "<private>")))
    return _replace_private_paths(text, replacements)


def _codex_error_message(value: Any) -> str | None:
    if isinstance(value, str):
        stripped = value.strip()
        if not stripped:
            return None
        try:
            decoded = json.loads(stripped)
        except json.JSONDecodeError:
            return stripped
        nested = _codex_error_message(decoded)
        return nested or stripped
    if isinstance(value, Mapping):
        if "error" in value:
            nested = _codex_error_message(value.get("error"))
            if nested:
                return nested
        if "message" in value:
            return _codex_error_message(value.get("message"))
    return None


def codex_jsonl_failure(stdout: str) -> str | None:
    stream_error: str | None = None
    turn_failure: str | None = None
    for line in stdout.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(event, Mapping):
            continue
        if event.get("type") == "error":
            stream_error = _codex_error_message(event.get("message")) or stream_error
        elif event.get("type") == "turn.failed":
            turn_failure = _codex_error_message(event.get("error")) or turn_failure
    return turn_failure or stream_error


def codex_environment(local_provider: str | None) -> dict[str, str]:
    environment = os.environ.copy()
    if local_provider is None:
        return environment
    for name in LOCAL_PRIVATE_ENVIRONMENT:
        environment.pop(name, None)
        environment.pop(name.lower(), None)
    environment["NO_PROXY"] = "localhost,127.0.0.1,::1"
    environment["no_proxy"] = environment["NO_PROXY"]
    if local_provider == "ollama":
        environment["OLLAMA_HOST"] = "127.0.0.1:11434"
        environment["CODEX_OSS_BASE_URL"] = "http://127.0.0.1:11434/v1"
    else:
        environment["LMS_SERVER_HOST"] = "127.0.0.1"
        environment["CODEX_OSS_BASE_URL"] = "http://127.0.0.1:1234/v1"
    return environment


def invoke_codex(
    *,
    executable: str,
    model: str,
    oss: bool,
    local_provider: str | None,
    prompt: str,
    images: Sequence[Path],
    output_path: Path,
    timeout_seconds: int,
    private_paths: Sequence[Path] = (),
) -> tuple[dict[str, Any], str, str]:
    output_schema_path = SCHEMA_PATH
    temporary_output_schema: Path | None = None
    if not oss:
        schema_payload = codex_output_schema()
        try:
            descriptor, output_schema_name = tempfile.mkstemp(
                prefix=".codex-output-schema-",
                suffix=".json",
                dir=output_path.parent,
            )
            temporary_output_schema = Path(output_schema_name)
            output_schema_path = temporary_output_schema
            with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
                json.dump(schema_payload, handle, allow_nan=False)
                handle.flush()
                os.fsync(handle.fileno())
        except (OSError, TypeError, ValueError) as exc:
            if temporary_output_schema is not None:
                try:
                    temporary_output_schema.unlink()
                except OSError:
                    pass
            raise InvestigationError(
                "schema_unavailable",
                f"Could not prepare output schema: {type(exc).__name__}",
            ) from exc
    command = [
        executable,
        "exec",
        "--ignore-user-config",
        "--sandbox",
        "read-only",
        "--ephemeral",
        "--json",
        "-c",
        'shell_environment_policy.inherit="core"',
        "-c",
        "shell_environment_policy.ignore_default_excludes=false",
        "-c",
        "analytics.enabled=false",
        "-c",
        "check_for_update_on_startup=false",
        "-c",
        'web_search="disabled"',
        "-c",
        "project_doc_max_bytes=0",
        "-c",
        "skills.include_instructions=false",
        "--model",
        model,
        "--output-schema",
        str(output_schema_path),
        "--output-last-message",
        str(output_path),
        "--color",
        "never",
    ]
    if oss and local_provider == LOCAL_DEFAULT_PROVIDER and model == LOCAL_DEFAULT_MODEL:
        command.extend(
            [
                "-c",
                f"model_context_window={LOCAL_DEFAULT_CONTEXT_WINDOW}",
                "-c",
                f"model_auto_compact_token_limit={LOCAL_AUTO_COMPACT_TOKEN_LIMIT}",
                "-c",
                f"tool_output_token_limit={LOCAL_TOOL_OUTPUT_TOKEN_LIMIT}",
            ]
        )
    if oss:
        command.append("--oss")
    if local_provider:
        command.extend(["--local-provider", local_provider])
    for image in images:
        command.extend(["--image", str(image)])
    command.extend(["-C", str(ROOT), "-"])
    try:
        try:
            process = subprocess.run(
                command,
                input=prompt,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=ROOT,
                env=codex_environment(local_provider if oss else None),
                check=False,
                timeout=timeout_seconds,
            )
        except FileNotFoundError as exc:
            raise InvestigationError(
                "backend_missing",
                "Codex executable is unavailable",
            ) from exc
        except subprocess.TimeoutExpired as exc:
            raise InvestigationError(
                "backend_timeout", f"Codex exceeded {timeout_seconds} seconds"
            ) from exc
    finally:
        if temporary_output_schema is not None:
            try:
                temporary_output_schema.unlink()
            except OSError:
                pass
    if process.returncode != 0:
        stderr_detail = "\n".join(
            line for line in process.stderr.splitlines()[-8:] if line.strip()
        )
        detail = (
            codex_jsonl_failure(process.stdout)
            or stderr_detail
            or f"Codex exited {process.returncode}"
        )
        raise InvestigationError(
            "backend_failed",
            sanitize_error(detail, private_paths),
        )
    try:
        report = json.loads(output_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise InvestigationError(
            "model_output_invalid",
            sanitize_error(f"Could not read schema JSON: {exc}", private_paths),
        ) from exc
    if not isinstance(report, dict):
        raise InvestigationError("model_output_invalid", "Model output is not a JSON object")
    if not oss:
        remove_transport_nulls(report)
    raw_coverage = report.get("coverage")
    if isinstance(raw_coverage, dict):
        raw_coverage["attachments"] = []
    schema_errors = validate_report_schema(report)
    if schema_errors:
        raise InvestigationError(
            "model_output_invalid",
            "Raw model report violates schema: " + "; ".join(schema_errors[:8]),
        )
    return report, process.stdout, process.stderr


def code_text(selector: Mapping[str, Any]) -> str | None:
    path = selector.get("path")
    revision = selector.get("revision")
    if not isinstance(path, str) or not isinstance(revision, str):
        return None
    current = safe_relative_file(ROOT, path)
    if revision.upper() == "WORKTREE":
        try:
            return current.read_text(encoding="utf-8") if current else None
        except (OSError, UnicodeError):
            return None
    process = subprocess.run(
        ["git", "show", f"{revision}:{path}"],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return process.stdout if process.returncode == 0 else None


def probe_video_bounds(
    path: Path, *, count_frames: bool = False
) -> tuple[float, int | None] | None:
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        return None
    try:
        command = [
            ffprobe,
            "-v", "error",
            "-select_streams", "v:0",
        ]
        if count_frames:
            command.append("-count_frames")
        stream_entries = "duration"
        if count_frames:
            stream_entries += ",nb_frames,nb_read_frames"
        command += [
            "-show_entries", f"format=duration:stream={stream_entries}",
            "-of", "json",
            str(path),
        ]
        process = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=30,
        )
        payload = json.loads(process.stdout) if process.returncode == 0 else {}
        stream = payload.get("streams", [])[0]
        duration = float(payload.get("format", {}).get("duration") or stream.get("duration"))
        if not math.isfinite(duration) or duration <= 0:
            return None
        raw_frame_count = (
            stream.get("nb_read_frames") or stream.get("nb_frames")
            if count_frames
            else None
        )
        frame_count = int(raw_frame_count) if str(raw_frame_count).isdigit() else None
        return duration, frame_count
    except (
        IndexError,
        KeyError,
        OSError,
        TypeError,
        ValueError,
        json.JSONDecodeError,
        subprocess.TimeoutExpired,
    ):
        return None


def _video_attachment_error(
    run_dir: Path,
    selector: Mapping[str, Any],
    attachments_by_index: Mapping[int, Mapping[str, Any]] | None,
    *,
    required: bool,
) -> str | None:
    attachment_index = selector.get("attachment_index")
    cell_indices = selector.get("cell_indices")
    if attachment_index is None and not required:
        return None
    if (
        not isinstance(attachment_index, int)
        or isinstance(attachment_index, bool)
        or attachments_by_index is None
        or attachment_index not in attachments_by_index
    ):
        return f"video attachment does not resolve: {attachment_index!r}"
    if (
        not isinstance(cell_indices, list)
        or not cell_indices
        or any(not isinstance(value, int) or isinstance(value, bool) for value in cell_indices)
        or len(set(cell_indices)) != len(cell_indices)
    ):
        return f"video attachment cells do not resolve: {attachment_index!r}"

    attachment = attachments_by_index[attachment_index]
    if (
        attachment.get("source_video_path") != selector.get("path")
        or attachment.get("source_video_sha256") != selector.get("sha256")
    ):
        return f"video attachment source does not resolve: {attachment_index}"
    sheet = safe_relative_file(run_dir, attachment.get("sheet_path"))
    try:
        sheet_hash = sha256_file(sheet) if sheet is not None else None
    except OSError:
        sheet_hash = None
    if sheet_hash != attachment.get("sheet_sha256"):
        return f"video attachment sheet does not resolve: {attachment_index}"

    represented = attachment.get("interval")
    if not isinstance(represented, Mapping):
        return f"video attachment interval does not resolve: {attachment_index}"
    represented_start = represented.get("start_pts_seconds")
    represented_end = represented.get("end_pts_seconds")
    if not all(
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
        for value in (represented_start, represented_end)
    ):
        return f"video attachment interval does not resolve: {attachment_index}"
    selected_start = float(selector["start_pts_s"])
    selected_end = float(selector["end_pts_s"])
    if (
        selected_start < float(represented_start) - 0.000_001
        or selected_end > float(represented_end) + 0.000_001
    ):
        return f"video selector is outside attached sheet: {attachment_index}"

    cells = attachment.get("cells")
    if not isinstance(cells, list):
        return f"video attachment cells do not resolve: {attachment_index}"
    cells_by_index = {
        item.get("cell_index"): item for item in cells if isinstance(item, Mapping)
    }
    selected_cells = [cells_by_index.get(index) for index in cell_indices]
    if any(cell is None for cell in selected_cells):
        return f"video attachment cells do not resolve: {attachment_index}"
    for cell in selected_cells:
        if not isinstance(cell, Mapping) or cell.get("source_pts_measured") is not True:
            continue
        measured_pts = cell.get("source_pts_seconds")
        source_pts = cell.get("source_pts_value")
        source_frame = cell.get("source_frame_index")
        time_base = cell.get("source_pts_time_base")
        if (
            not isinstance(measured_pts, (int, float))
            or isinstance(measured_pts, bool)
            or not math.isfinite(float(measured_pts))
            or not isinstance(source_pts, int)
            or isinstance(source_pts, bool)
            or not isinstance(source_frame, int)
            or isinstance(source_frame, bool)
            or source_frame < 0
            or not isinstance(time_base, Mapping)
        ):
            return f"video measured source position does not resolve: {attachment_index}"
        numerator, denominator = time_base.get("numerator"), time_base.get("denominator")
        if (
            not isinstance(numerator, int)
            or isinstance(numerator, bool)
            or not isinstance(denominator, int)
            or isinstance(denominator, bool)
            or numerator <= 0
            or denominator <= 0
            or not math.isclose(
                float(measured_pts),
                source_pts * numerator / denominator,
                rel_tol=0,
                abs_tol=0.000_000_001,
            )
        ):
            return f"video measured source position does not resolve: {attachment_index}"
    if all(
        isinstance(cell, Mapping) and cell.get("source_pts_measured") is True
        for cell in selected_cells
    ):
        measured_positions = [float(cell["source_pts_seconds"]) for cell in selected_cells]
        if (
            not math.isclose(
                selected_start,
                min(measured_positions),
                rel_tol=0,
                abs_tol=0.000_001,
            )
            or not math.isclose(
                selected_end,
                max(measured_positions),
                rel_tol=0,
                abs_tol=0.000_001,
            )
        ):
            return f"video selector does not match attached measured PTS: {attachment_index}"
    uncertainty_intervals = [
        cell.get("pts_uncertainty_interval")
        for cell in selected_cells
        if isinstance(cell, Mapping)
    ]
    if any(not isinstance(interval, Mapping) for interval in uncertainty_intervals):
        return f"video attachment uncertainty does not resolve: {attachment_index}"
    uncertainty_starts = [interval.get("start_pts_seconds") for interval in uncertainty_intervals]
    uncertainty_ends = [interval.get("end_pts_seconds") for interval in uncertainty_intervals]
    if not all(
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
        for value in (*uncertainty_starts, *uncertainty_ends)
    ):
        return f"video attachment uncertainty does not resolve: {attachment_index}"
    if (
        selected_start > min(float(value) for value in uncertainty_starts) + 0.000_001
        or selected_end < max(float(value) for value in uncertainty_ends) - 0.000_001
    ):
        return f"video selector narrows attached PTS uncertainty: {attachment_index}"
    for cell, interval in zip(selected_cells, uncertainty_intervals):
        if not isinstance(cell, Mapping) or cell.get("source_pts_measured") is not True:
            continue
        measured_pts = float(cell["source_pts_seconds"])
        if (
            cell.get("pts_uncertainty_seconds") != 0
            or not math.isclose(
                float(interval["start_pts_seconds"]), measured_pts, rel_tol=0, abs_tol=1e-9
            )
            or not math.isclose(
                float(interval["end_pts_seconds"]), measured_pts, rel_tol=0, abs_tol=1e-9
            )
        ):
            return f"video measured PTS uncertainty does not resolve: {attachment_index}"
    if selector.get("start_frame") is not None:
        source_frames = [
            cell.get("source_frame_index")
            for cell in selected_cells
            if isinstance(cell, Mapping)
        ]
        if any(
            cell.get("source_pts_measured") is not True
            for cell in selected_cells
            if isinstance(cell, Mapping)
        ) or any(
            not isinstance(frame, int) or isinstance(frame, bool)
            for frame in source_frames
        ):
            return f"video frame indices are not measured for attachment: {attachment_index}"
        if (
            int(selector["start_frame"]) != min(source_frames)
            or int(selector["end_frame"]) != max(source_frames)
        ):
            return f"video selector does not match attached frame positions: {attachment_index}"
    return None


def resolve_artifact_selector(
    run_dir: Path,
    selector: dict[str, Any],
    video_bounds_cache: dict[Path, tuple[float, int | None] | None] | None = None,
    attachments_by_index: Mapping[int, Mapping[str, Any]] | None = None,
    *,
    require_video_attachment: bool = True,
) -> str | None:
    path = safe_relative_file(run_dir, selector.get("path"))
    if path is None:
        return f"artifact path does not resolve: {selector.get('path')!r}"
    try:
        actual_hash = sha256_file(path)
    except OSError:
        return f"artifact is unreadable: {selector.get('path')}"
    if selector.get("sha256") != actual_hash:
        return f"artifact hash does not match: {selector.get('path')}"
    selector_type = selector.get("kind")
    if INDEX_DIRECTORY in Path(str(selector.get("path"))).parts:
        return f"investigation index is a finding aid, not evidence: {selector.get('path')}"
    if path.suffix.lower() in VIDEO_SUFFIXES and selector_type != "video":
        return f"video artifact requires an attached video selector: {selector.get('path')}"
    if selector_type != "video" and ATTACHMENT_DIRECTORY in Path(str(selector.get("path"))).parts:
        return f"attached sheet requires a video selector: {selector.get('path')}"
    if selector_type in {"log", "ndjson"}:
        start = selector.get("line_start")
        end = selector.get("line_end")
        if (
            not isinstance(start, int)
            or isinstance(start, bool)
            or not isinstance(end, int)
            or isinstance(end, bool)
            or start < 1
            or start > end
        ):
            return f"artifact line range does not resolve: {selector.get('path')}:{start}-{end}"
        selected: list[str] = []
        try:
            with path.open("r", encoding="utf-8", errors="replace") as handle:
                line_count = 0
                for line_count, line in enumerate(handle, 1):
                    if start <= line_count <= end:
                        selected.append(line)
        except OSError:
            return f"artifact lines are unreadable: {selector.get('path')}"
        if end > line_count:
            return f"artifact line range does not resolve: {selector.get('path')}:{start}-{end}"
        if selector_type == "ndjson" and selector.get("keys"):
            try:
                records = [json.loads(line) for line in selected]
                pairs = [str(item).split("=", 1) for item in selector["keys"]]
                if any(len(pair) != 2 for pair in pairs) or not any(
                    all(str(record.get(key)) == expected for key, expected in pairs)
                    for record in records
                    if isinstance(record, Mapping)
                ):
                    return f"NDJSON keys do not resolve: {selector.get('path')}:{start}-{end}"
            except (OSError, UnicodeError, json.JSONDecodeError, TypeError):
                return f"NDJSON selector is unreadable: {selector.get('path')}:{start}-{end}"
    if selector_type == "json_pointer":
        try:
            document: Any = json.loads(path.read_text(encoding="utf-8"))
            pointer = selector.get("json_pointer")
            if not isinstance(pointer, str):
                raise ValueError("pointer is not a string")
            if pointer and not pointer.startswith("/"):
                raise ValueError("pointer is not absolute")
            if pointer:
                for token in pointer.lstrip("/").split("/"):
                    token = token.replace("~1", "/").replace("~0", "~")
                    if isinstance(document, list):
                        if not token.isdigit():
                            raise ValueError("array index is not non-negative")
                        document = document[int(token)]
                    else:
                        document = document[token]
        except (OSError, UnicodeError, json.JSONDecodeError, KeyError, IndexError, TypeError, ValueError):
            return f"JSON pointer does not resolve: {selector.get('path')}#{selector.get('json_pointer')}"
    if selector_type == "csv":
        start, end = selector.get("row_start"), selector.get("row_end")
        try:
            with path.open("r", encoding="utf-8", errors="replace", newline="") as handle:
                lines = [line for line in handle if line.strip() and not line.startswith("#")]
            reader = csv.DictReader(
                lines, delimiter="\t" if path.suffix.casefold() == ".tsv" else ","
            )
            rows = list(reader)
            if reader.fieldnames is None:
                raise ValueError("CSV header is missing")
        except OSError:
            return f"CSV selector is unreadable: {selector.get('path')}"
        except (csv.Error, ValueError):
            return f"CSV selector is invalid: {selector.get('path')}"
        data_count = len(rows)
        if (
            not isinstance(start, int)
            or isinstance(start, bool)
            or not isinstance(end, int)
            or isinstance(end, bool)
            or start < 1
            or start > end
            or end > data_count
        ):
            return f"CSV row range does not resolve: {selector.get('path')}:{start}-{end}"
        if selector.get("keys"):
            try:
                pairs = [str(item).split("=", 1) for item in selector["keys"]]
                selected = rows[start - 1 : end]
                if any(len(pair) != 2 for pair in pairs) or not any(
                    all(str(row.get(key)) == expected for key, expected in pairs)
                    for row in selected
                ):
                    return f"CSV keys do not resolve: {selector.get('path')}:{start}-{end}"
            except TypeError:
                return f"CSV selector is invalid: {selector.get('path')}:{start}-{end}"
    if selector_type == "video":
        start, end = selector.get("start_pts_s"), selector.get("end_pts_s")
        if (
            not isinstance(start, (int, float))
            or isinstance(start, bool)
            or not isinstance(end, (int, float))
            or isinstance(end, bool)
            or not math.isfinite(float(start))
            or not math.isfinite(float(end))
            or start < 0
            or start > end
        ):
            return f"video PTS range does not resolve: {selector.get('path')}:{start}-{end}"
        cache = video_bounds_cache if video_bounds_cache is not None else {}
        if path not in cache:
            cache[path] = probe_video_bounds(path)
        bounds = cache[path]
        if bounds is None:
            return f"video bounds are unavailable: {selector.get('path')}"
        duration, frame_count = bounds
        if float(end) > duration + 0.000_001:
            return f"video PTS range exceeds duration: {selector.get('path')}:{start}-{end}"
        first_frame, last_frame = selector.get("start_frame"), selector.get("end_frame")
        if (first_frame is None) != (last_frame is None):
            return f"video frame range does not resolve: {selector.get('path')}:{first_frame}-{last_frame}"
        if first_frame is not None and (
            not isinstance(first_frame, int)
            or isinstance(first_frame, bool)
            or not isinstance(last_frame, int)
            or isinstance(last_frame, bool)
            or first_frame < 0
            or first_frame > last_frame
        ):
            return f"video frame range does not resolve: {selector.get('path')}:{first_frame}-{last_frame}"
        attachment_error = _video_attachment_error(
            run_dir,
            selector,
            attachments_by_index,
            required=require_video_attachment,
        )
        if attachment_error:
            return attachment_error
        frame_position_is_attached = selector.get("attachment_index") is not None
        if (
            first_frame is not None
            and frame_count not in (None, FRAME_COUNT_UNAVAILABLE)
            and last_frame >= frame_count
        ):
            return f"video frame range does not resolve: {selector.get('path')}:{first_frame}-{last_frame}"
        if first_frame is not None and not frame_position_is_attached:
            if frame_count == FRAME_COUNT_UNAVAILABLE:
                return f"video frame bounds are unavailable: {selector.get('path')}"
            if frame_count is None:
                counted_bounds = probe_video_bounds(path, count_frames=True)
                if counted_bounds is None or counted_bounds[1] is None:
                    cache[path] = (duration, FRAME_COUNT_UNAVAILABLE)
                    return f"video frame bounds are unavailable: {selector.get('path')}"
                duration, frame_count = counted_bounds
                cache[path] = counted_bounds
            if last_frame >= frame_count:
                return f"video frame range does not resolve: {selector.get('path')}:{first_frame}-{last_frame}"
    return None


def resolve_code_selector(
    selector: Mapping[str, Any],
    *,
    allowed_revisions: set[str] | None = None,
    inspected_revision: str | None = None,
) -> str | None:
    revision = selector.get("revision")
    canonical_revision = canonical_commit(revision) if isinstance(revision, str) else None
    if canonical_revision is None or revision != canonical_revision:
        return f"code revision is not a canonical commit: {revision}"
    if allowed_revisions is not None and canonical_revision not in allowed_revisions:
        return f"code revision is not recorded for this run: {revision}"
    if allowed_revisions is not None and canonical_revision != inspected_revision:
        return f"code revision does not match the inspected revision: {revision}"
    content = code_text(selector)
    if content is None:
        return f"code selector does not resolve: {selector.get('revision')}:{selector.get('path')}"
    start, end = selector.get("line_start"), selector.get("line_end")
    selection_hash = code_selection_sha256(content, start, end)
    if selection_hash is None:
        return f"code line range does not resolve: {selector.get('path')}:{start}-{end}"
    if selector.get("selection_sha256") != selection_hash:
        return f"code selection hash does not match: {selector.get('path')}:{start}-{end}"
    return None


def partition_artifact_selectors(
    run_dir: Path,
    selectors: Sequence[dict[str, Any]],
    video_bounds_cache: dict[Path, tuple[float, int | None] | None],
    attachments_by_index: Mapping[int, Mapping[str, Any]],
    *,
    require_video_attachment: bool = True,
) -> tuple[list[dict[str, Any]], list[str]]:
    valid: list[dict[str, Any]] = []
    errors: list[str] = []
    for selector in selectors:
        error = resolve_artifact_selector(
            run_dir,
            selector,
            video_bounds_cache,
            attachments_by_index,
            require_video_attachment=require_video_attachment,
        )
        if error:
            errors.append(error)
        else:
            valid.append(selector)
    return valid, errors


def partition_code_selectors(
    selectors: Sequence[Mapping[str, Any]],
    *,
    allowed_revisions: set[str] | None = None,
    inspected_revision: str | None = None,
) -> tuple[list[Mapping[str, Any]], list[str]]:
    valid: list[Mapping[str, Any]] = []
    errors: list[str] = []
    for selector in selectors:
        error = resolve_code_selector(
            selector,
            allowed_revisions=allowed_revisions,
            inspected_revision=inspected_revision,
        )
        if error:
            errors.append(error)
        else:
            valid.append(selector)
    return valid, errors


def validate_published_selectors(
    run_dir: Path,
    report: Mapping[str, Any],
) -> list[str]:
    """Re-resolve every published evidence and code selector after redaction."""
    errors: list[str] = []
    video_bounds_cache: dict[Path, tuple[float, int | None] | None] = {}
    coverage = report.get("coverage")
    coverage = coverage if isinstance(coverage, Mapping) else {}
    attachments = coverage.get("attachments")
    attachments_by_index: dict[int, Mapping[str, Any]] = {}
    if isinstance(attachments, list):
        for attachment in attachments:
            if not isinstance(attachment, Mapping):
                continue
            index = attachment.get("attachment_index")
            if isinstance(index, int) and not isinstance(index, bool):
                attachments_by_index[index] = attachment

    source = report.get("source")
    allowed_code_revisions: set[str] = set()
    inspected_code_revision: str | None = None
    if isinstance(source, Mapping):
        recorded = source.get("recorded_revisions")
        if isinstance(recorded, list):
            for revision in recorded:
                if not isinstance(revision, str) or not re.fullmatch(
                    r"[0-9a-f]{40,64}", revision
                ):
                    errors.append(f"source recorded revision is not canonical: {revision}")
                    continue
                canonical = canonical_commit(revision)
                if canonical == revision:
                    allowed_code_revisions.add(canonical)
        inspected = source.get("inspected_revision")
        if isinstance(inspected, str):
            inspected_code_revision = canonical_commit(inspected)
            if inspected_code_revision is None or inspected_code_revision != inspected:
                errors.append(f"source inspected revision is not canonical: {inspected}")
                inspected_code_revision = None
            elif inspected_code_revision not in allowed_code_revisions:
                errors.append(
                    f"source inspected revision was not recorded for this run: {inspected}"
                )

    def check_artifacts(
        selectors: Any,
        owner: str,
        *,
        require_video_attachment: bool = True,
    ) -> None:
        if not isinstance(selectors, list):
            return
        for index, selector in enumerate(selectors, start=1):
            if not isinstance(selector, dict):
                continue
            error = resolve_artifact_selector(
                run_dir,
                selector,
                video_bounds_cache,
                attachments_by_index,
                require_video_attachment=require_video_attachment,
            )
            if error:
                errors.append(f"{owner}[{index}]: {error}")

    def check_code(selectors: Any, owner: str) -> None:
        if not isinstance(selectors, list):
            return
        for index, selector in enumerate(selectors, start=1):
            if not isinstance(selector, Mapping):
                continue
            error = resolve_code_selector(
                selector,
                allowed_revisions=allowed_code_revisions,
                inspected_revision=inspected_code_revision,
            )
            if error:
                errors.append(f"{owner}[{index}]: {error}")

    if isinstance(source, Mapping):
        check_artifacts(source.get("identity_evidence"), "source.identity_evidence")
        binary_identities = source.get("binary_identities")
        if isinstance(binary_identities, list):
            for identity_index, identity in enumerate(binary_identities, start=1):
                if isinstance(identity, Mapping):
                    check_artifacts(
                        identity.get("evidence"),
                        f"source.binary_identities[{identity_index}].evidence",
                    )

    artifacts = coverage.get("artifacts")
    if isinstance(artifacts, list):
        for artifact_index, artifact in enumerate(artifacts, start=1):
            if not isinstance(artifact, Mapping):
                continue
            selectors = artifact.get("selectors")
            check_artifacts(
                selectors,
                f"coverage.artifacts[{artifact_index}].selectors",
            )
            if isinstance(selectors, list):
                for selector_index, selector in enumerate(selectors, start=1):
                    if not isinstance(selector, Mapping):
                        continue
                    if (
                        selector.get("path") != artifact.get("path")
                        or selector.get("sha256") != artifact.get("sha256")
                    ):
                        errors.append(
                            "coverage.artifacts"
                            f"[{artifact_index}].selectors[{selector_index}]: "
                            "selector targets another artifact"
                        )
    check_code(coverage.get("code"), "coverage.code")

    video_intervals = coverage.get("video_intervals")
    if isinstance(video_intervals, list):
        for interval_index, interval in enumerate(video_intervals, start=1):
            if isinstance(interval, Mapping):
                check_artifacts(
                    [interval.get("selector")],
                    f"coverage.video_intervals[{interval_index}].selector",
                    require_video_attachment=interval.get("status") != "unsampled",
                )
    clock_mappings = coverage.get("clock_mappings")
    if isinstance(clock_mappings, list):
        for mapping_index, mapping in enumerate(clock_mappings, start=1):
            if isinstance(mapping, Mapping):
                check_artifacts(
                    mapping.get("evidence"),
                    f"coverage.clock_mappings[{mapping_index}].evidence",
                )

    findings = report.get("findings")
    if isinstance(findings, list):
        for finding_index, finding in enumerate(findings, start=1):
            if not isinstance(finding, Mapping):
                continue
            owner = f"findings[{finding_index}]"
            check_artifacts(finding.get("evidence"), f"{owner}.evidence")
            check_artifacts(finding.get("counterevidence"), f"{owner}.counterevidence")
            check_code(finding.get("code"), f"{owner}.code")

    unresolved_items = report.get("unresolved")
    if isinstance(unresolved_items, list):
        for unresolved_index, unresolved in enumerate(unresolved_items, start=1):
            if not isinstance(unresolved, Mapping):
                continue
            owner = f"unresolved[{unresolved_index}]"
            check_artifacts(unresolved.get("evidence"), f"{owner}.evidence")
            check_artifacts(unresolved.get("counterevidence"), f"{owner}.counterevidence")
            check_code(unresolved.get("code"), f"{owner}.code")
            hypotheses = unresolved.get("hypotheses")
            if isinstance(hypotheses, list):
                for hypothesis_index, hypothesis in enumerate(hypotheses, start=1):
                    if not isinstance(hypothesis, Mapping):
                        continue
                    hypothesis_owner = (
                        f"{owner}.hypotheses[{hypothesis_index}]"
                    )
                    check_artifacts(
                        hypothesis.get("evidence"),
                        f"{hypothesis_owner}.evidence",
                    )
                    check_code(hypothesis.get("code"), f"{hypothesis_owner}.code")
    return errors


def index_clock_mappings(
    run_dir: Path,
    mappings: Sequence[Mapping[str, Any]],
    video_bounds_cache: dict[Path, tuple[float, int | None] | None],
    attachments_by_index: Mapping[int, Mapping[str, Any]],
) -> tuple[dict[str, str | None], set[str], list[str]]:
    counts: dict[str, int] = {}
    for mapping in mappings:
        mapping_id = str(mapping.get("id"))
        counts[mapping_id] = counts.get(mapping_id, 0) + 1
    duplicates = {mapping_id for mapping_id, count in counts.items() if count > 1}
    support: dict[str, str | None] = {}
    errors: list[str] = [
        f"clock mapping id is not unique: {mapping_id}" for mapping_id in sorted(duplicates)
    ]
    for mapping in mappings:
        mapping_id = str(mapping.get("id"))
        valid_evidence, mapping_evidence_errors = partition_artifact_selectors(
            run_dir,
            mapping.get("evidence", []),
            video_bounds_cache,
            attachments_by_index,
        )
        if isinstance(mapping, dict):
            mapping["evidence"] = valid_evidence
        errors.extend(f"clock mapping {mapping_id}: {error}" for error in mapping_evidence_errors)
        if mapping_id in duplicates:
            continue
        status = mapping.get("status")
        if status == "unavailable":
            support[mapping_id] = f"clock mapping is unavailable: {mapping_id}"
        elif not valid_evidence:
            if isinstance(mapping, dict):
                mapping["status"] = "unavailable"
                mapping["uncertainty_s"] = None
                mapping.setdefault("limitations", []).append(
                    "Runner removed all mapping evidence because no selector resolved."
                )
            support[mapping_id] = f"clock mapping has no valid evidence: {mapping_id}"
            errors.append(support[mapping_id])
        elif mapping_evidence_errors:
            support[mapping_id] = f"clock mapping has unresolved evidence: {mapping_id}"
        else:
            support[mapping_id] = None
    return support, duplicates, errors


def clock_reference_errors(
    conclusion: Mapping[str, Any],
    support: Mapping[str, str | None],
    duplicates: set[str],
) -> list[str]:
    errors: list[str] = []
    for mapping_id in conclusion.get("clock_mapping_ids", []):
        if mapping_id in duplicates:
            errors.append(f"clock mapping id is not unique: {mapping_id}")
        elif mapping_id not in support:
            errors.append(f"clock mapping id does not resolve: {mapping_id}")
        elif support[mapping_id]:
            errors.append(str(support[mapping_id]))
    return errors


def unresolved_from_finding(
    finding: Mapping[str, Any],
    primary_errors: Sequence[str],
    evidence: Sequence[dict[str, Any]],
    code: Sequence[Mapping[str, Any]],
) -> dict[str, Any] | None:
    if not evidence:
        return None
    cause = str(finding.get("cause") or "The reported cause is not established.")
    return {
        "id": finding["id"],
        "title": finding["title"],
        "causal_status": "unknown",
        "observation": finding["observed_behavior"],
        "why_unknown": "Primary citation resolution failed: " + "; ".join(primary_errors),
        "evidence": list(evidence),
        "counterevidence": finding.get("counterevidence", []),
        "code": list(code),
        "clock_mapping_ids": finding.get("clock_mapping_ids", []),
        "hypotheses": [
            {
                "rank": 1,
                "description": cause,
                "evidence": list(evidence),
                "code": list(code),
            }
        ],
        "next_observation": {
            "description": "Resolve the primary artifact and owning code citations before acting on this cause.",
            "distinguishes": [cause, "The cited observation or code attribution is incorrect."],
            "minimal_evidence": "One additional resolvable selector that establishes or rejects the proposed cause.",
        },
    }


def finish_report(
    report: dict[str, Any],
    *,
    run_dir: Path,
    source_context_value: Mapping[str, Any],
    model: str,
    backend: str,
    tool_version: str,
    prompt: str,
    extra_errors: Sequence[tuple[str, str]],
    attachment_manifest: Sequence[Mapping[str, Any]] = (),
) -> dict[str, Any]:
    raw_coverage = report.get("coverage")
    if isinstance(raw_coverage, dict):
        raw_coverage["attachments"] = []
    raw_schema_errors = validate_report_schema(report)
    if raw_schema_errors:
        raise InvestigationError(
            "model_output_invalid",
            "Raw model report violates schema: " + "; ".join(raw_schema_errors[:8]),
        )
    # Resolve only the privacy-safe representation. A selector whose path is
    # changed by the local blocklist must lose its grounding claim rather than
    # retain a private path solely so it can still resolve.
    report = sanitize_investigation_report(report, run_dir)
    model_execution = report["execution_status"]
    model_state = model_execution.get("state")
    model_summary = str(model_execution.get("summary") or "Model investigation returned no summary.")
    model_errors = model_execution.get("errors")

    source_report = report["source"]
    runner_errors = list(extra_errors)
    allowed_code_revisions = set(recorded_source_revisions(source_context_value))
    recorded_revision_list = sorted(recorded_identity_revisions(source_context_value))
    reported_revisions = source_report.get("recorded_revisions")
    if reported_revisions != recorded_revision_list:
        source_report["recorded_revisions"] = recorded_revision_list
        limitation = "Runner replaced model-reported revisions with recorded run identity."
        source_report.setdefault("limitations", []).append(limitation)
        runner_errors.append(("source_revisions_corrected", limitation))
    raw_inspected_revision = source_report.get("inspected_revision")
    inspected_revision = (
        canonical_commit(raw_inspected_revision)
        if isinstance(raw_inspected_revision, str)
        else None
    )
    if inspected_revision not in allowed_code_revisions:
        inspected_revision = None
        if raw_inspected_revision is not None:
            limitation = (
                "Runner removed an inspected revision that was not recorded for this run."
            )
            source_report.setdefault("limitations", []).append(limitation)
            runner_errors.append(("source_revision_unrecorded", limitation))
    source_report["inspected_revision"] = inspected_revision
    model_basis = source_report.get("basis")
    prechecked_basis = conservative_source_basis(source_context_value)
    effective_basis = min((str(model_basis), prechecked_basis), key=SOURCE_BASIS_RANK.__getitem__)
    if effective_basis != model_basis:
        limitation = (
            f"Runner source precheck limited basis from {model_basis} to {effective_basis}."
        )
        source_report["basis"] = effective_basis
        source_report.setdefault("mismatches", []).append(limitation)
        source_report.setdefault("limitations", []).append(limitation)
        runner_errors.append(("source_basis_limited", limitation))
    resolution_errors: list[str] = []
    video_bounds_cache: dict[Path, tuple[float, int | None] | None] = {}
    coverage = report["coverage"]
    attachments_by_index: dict[int, Mapping[str, Any]] = {}
    for attachment in attachment_manifest:
        index = attachment.get("attachment_index")
        if not isinstance(index, int) or isinstance(index, bool) or index in attachments_by_index:
            raise InvestigationError(
                "attachment_manifest_invalid", "Runner attachment indices are not unique"
            )
        attachments_by_index[index] = attachment
    coverage["attachments"] = copy.deepcopy(list(attachment_manifest))

    def filter_evidence(owner: dict[str, Any], key: str) -> list[str]:
        valid, selector_errors = partition_artifact_selectors(
            run_dir,
            owner.get(key, []),
            video_bounds_cache,
            attachments_by_index,
        )
        owner[key] = valid
        return selector_errors

    resolution_errors.extend(filter_evidence(source_report, "identity_evidence"))
    for identity in source_report.get("binary_identities", []):
        if isinstance(identity, dict):
            identity_errors = filter_evidence(identity, "evidence")
            resolution_errors.extend(identity_errors)
            if identity.get("basis") != "unavailable" and not identity["evidence"]:
                identity["basis"] = "unavailable"
                identity["sha256"] = None
                identity.setdefault("limitations", []).append(
                    "Runner removed the identity claim because no evidence selector resolved."
                )
                runner_errors.append(
                    (
                        "binary_identity_ungrounded",
                        f"{identity.get('name')}: no identity evidence resolved",
                    )
                )

    clock_support, duplicate_clock_ids, clock_errors = index_clock_mappings(
        run_dir,
        coverage["clock_mappings"],
        video_bounds_cache,
        attachments_by_index,
    )
    resolution_errors.extend(clock_errors)

    valid_coverage_code, coverage_code_errors = partition_code_selectors(
        coverage["code"],
        allowed_revisions=allowed_code_revisions,
        inspected_revision=inspected_revision,
    )
    coverage["code"] = valid_coverage_code
    resolution_errors.extend(coverage_code_errors)

    retained_video_coverage: list[dict[str, Any]] = []
    for item in coverage["video_intervals"]:
        selector = item["selector"]
        if selector.get("kind") != "video":
            resolution_errors.append("video coverage selector is not video evidence")
            continue
        valid, selector_errors = partition_artifact_selectors(
            run_dir,
            [selector],
            video_bounds_cache,
            attachments_by_index,
            require_video_attachment=item.get("status") != "unsampled",
        )
        resolution_errors.extend(selector_errors)
        if valid:
            retained_video_coverage.append(item)
        elif item.get("status") != "unsampled":
            runner_errors.append(
                (
                    "video_coverage_omitted",
                    "A reviewed or partially reviewed video interval did not resolve to an attached sheet and cell.",
                )
            )
    coverage["video_intervals"] = retained_video_coverage

    retained_findings: list[dict[str, Any]] = []
    converted_unresolved: list[dict[str, Any]] = []
    for finding in report["findings"]:
        valid_evidence, evidence_errors = partition_artifact_selectors(
            run_dir,
            finding["evidence"],
            video_bounds_cache,
            attachments_by_index,
        )
        valid_code, code_errors = partition_code_selectors(
            finding["code"],
            allowed_revisions=allowed_code_revisions,
            inspected_revision=inspected_revision,
        )
        valid_counterevidence, secondary_errors = partition_artifact_selectors(
            run_dir,
            finding["counterevidence"],
            video_bounds_cache,
            attachments_by_index,
        )
        primary_errors = evidence_errors + code_errors
        secondary_errors.extend(
            clock_reference_errors(finding, clock_support, duplicate_clock_ids)
        )
        finding["evidence"] = valid_evidence
        finding["code"] = valid_code
        finding["counterevidence"] = valid_counterevidence
        resolution_errors.extend(primary_errors)
        resolution_errors.extend(secondary_errors)
        if primary_errors:
            unresolved = unresolved_from_finding(
                finding,
                primary_errors + secondary_errors,
                valid_evidence,
                valid_code,
            )
            if unresolved is None:
                runner_errors.append(
                    (
                        "unresolved_omitted_zero_grounding",
                        f"{finding['id']}: no primary artifact evidence resolved",
                    )
                )
            else:
                converted_unresolved.append(unresolved)
                runner_errors.append(
                    (
                        "finding_moved_unresolved",
                        f"{finding['id']}: primary evidence or code did not resolve",
                    )
                )
            continue
        if secondary_errors:
            if finding.get("causal_status") == "confirmed":
                finding["causal_status"] = "probable"
            unknowns = finding.setdefault("remaining_unknowns", [])
            unknowns.extend(f"Citation resolution: {error}" for error in secondary_errors)
        retained_findings.append(finding)
    report["findings"] = retained_findings

    if effective_basis in {"current_only", "unavailable"}:
        for finding in retained_findings:
            if finding.get("causal_status") == "confirmed":
                finding["causal_status"] = "probable"
                finding.setdefault("remaining_unknowns", []).append(
                    f"Source attribution is {effective_basis}; the cited code is a hypothesis."
                )

    retained_unresolved: list[dict[str, Any]] = []
    for unresolved in [*report["unresolved"], *converted_unresolved]:
        valid_evidence, primary_errors = partition_artifact_selectors(
            run_dir,
            unresolved["evidence"],
            video_bounds_cache,
            attachments_by_index,
        )
        valid_counterevidence, errors = partition_artifact_selectors(
            run_dir,
            unresolved["counterevidence"],
            video_bounds_cache,
            attachments_by_index,
        )
        valid_code, code_errors = partition_code_selectors(
            unresolved["code"],
            allowed_revisions=allowed_code_revisions,
            inspected_revision=inspected_revision,
        )
        errors.extend(code_errors)
        for hypothesis in unresolved["hypotheses"]:
            valid_hypothesis_evidence, hypothesis_errors = partition_artifact_selectors(
                run_dir,
                hypothesis["evidence"],
                video_bounds_cache,
                attachments_by_index,
            )
            valid_hypothesis_code, hypothesis_code_errors = partition_code_selectors(
                hypothesis["code"],
                allowed_revisions=allowed_code_revisions,
                inspected_revision=inspected_revision,
            )
            hypothesis["evidence"] = valid_hypothesis_evidence
            hypothesis["code"] = valid_hypothesis_code
            errors.extend(hypothesis_errors)
            errors.extend(hypothesis_code_errors)
        errors.extend(clock_reference_errors(unresolved, clock_support, duplicate_clock_ids))
        unresolved["evidence"] = valid_evidence
        unresolved["counterevidence"] = valid_counterevidence
        unresolved["code"] = valid_code
        resolution_errors.extend(primary_errors)
        resolution_errors.extend(errors)
        if not valid_evidence:
            runner_errors.append(
                (
                    "unresolved_omitted_zero_grounding",
                    f"{unresolved['id']}: no primary artifact evidence resolved",
                )
            )
            continue
        if primary_errors or errors:
            unresolved["why_unknown"] = (
                str(unresolved.get("why_unknown", ""))
                + " Citation resolution: "
                + "; ".join(primary_errors + errors)
            ).strip()
        retained_unresolved.append(unresolved)
    report["unresolved"] = retained_unresolved

    canonical_artifacts = {
        item["path"]: item for item in discover_artifacts(run_dir)
    }
    retained_artifact_coverage: list[dict[str, Any]] = []
    for item in coverage["artifacts"]:
        canonical = canonical_artifacts.get(item.get("path"))
        valid_selectors, selector_errors = partition_artifact_selectors(
            run_dir,
            item.get("selectors", []),
            video_bounds_cache,
            attachments_by_index,
        )
        matching_selectors: list[dict[str, Any]] = []
        for selector in valid_selectors:
            if (
                selector.get("path") == item.get("path")
                and selector.get("sha256") == item.get("sha256")
            ):
                matching_selectors.append(selector)
        if len(matching_selectors) != len(valid_selectors):
            selector_errors.append(
                f"coverage selector targets another artifact: {item.get('path')}"
            )
        resolution_errors.extend(selector_errors)
        claimed_review = item.get("status") in {"reviewed", "partially_reviewed"}
        hash_matches = bool(
            canonical is not None
            and item.get("sha256") is not None
            and item.get("sha256") == canonical.get("sha256")
        )
        if claimed_review and (not hash_matches or not matching_selectors):
            reason = "reviewed coverage lacked a matching hash and resolvable selector"
            retained_artifact_coverage.append(
                {
                    "path": str(item.get("path")),
                    "status": "skipped",
                    "sha256": None,
                    "size_bytes": canonical.get("size_bytes") if canonical else None,
                    "role": canonical.get("kind", item.get("role", "unknown"))
                    if canonical
                    else str(item.get("role", "unknown")),
                    "selectors": [],
                    "notes": [*item.get("notes", []), f"Runner correction: {reason}."],
                }
            )
            runner_errors.append(
                ("artifact_coverage_downgraded", f"{item.get('path')}: {reason}")
            )
            if not hash_matches:
                resolution_errors.append(
                    f"covered artifact hash does not match: {item.get('path')}"
                )
            continue
        item["selectors"] = matching_selectors
        if selector_errors and item.get("status") == "reviewed":
            item["status"] = "partially_reviewed"
            item.setdefault("notes", []).append(
                "Runner removed one or more selectors that did not resolve."
            )
        if not claimed_review and item.get("sha256") is not None and not hash_matches:
            item["sha256"] = None
            resolution_errors.append(
                f"covered artifact hash does not match: {item.get('path')}"
            )
        retained_artifact_coverage.append(item)

    coverage_by_path: dict[str, list[dict[str, Any]]] = {}
    for item in retained_artifact_coverage:
        coverage_by_path.setdefault(str(item.get("path")), []).append(item)
    retained_artifact_coverage = []
    for path in sorted(coverage_by_path):
        rows = coverage_by_path[path]
        if len(rows) == 1:
            retained_artifact_coverage.append(rows[0])
            continue
        canonical = canonical_artifacts.get(path)
        reviewed_rows = [
            item
            for item in rows
            if item.get("status") in {"reviewed", "partially_reviewed"}
            and item.get("selectors")
        ]
        selectors: list[dict[str, Any]] = []
        seen_selectors: set[str] = set()
        for item in reviewed_rows:
            for selector in item.get("selectors", []):
                key = json.dumps(selector, sort_keys=True, separators=(",", ":"))
                if key not in seen_selectors:
                    seen_selectors.add(key)
                    selectors.append(selector)
        statuses = {str(item.get("status")) for item in rows}
        if reviewed_rows:
            status = (
                "reviewed"
                if statuses == {"reviewed"}
                else "partially_reviewed"
            )
            digest = reviewed_rows[0].get("sha256")
        else:
            status = next(
                candidate
                for candidate in ("unreadable", "unfamiliar", "skipped")
                if candidate in statuses
            )
            digests = {item.get("sha256") for item in rows}
            digest = next(iter(digests)) if len(digests) == 1 else None
        notes: list[str] = []
        for item in rows:
            for note in item.get("notes", []):
                if note not in notes:
                    notes.append(note)
        notes.append(
            "Runner merged duplicate coverage rows conservatively; "
            f"reported statuses were {', '.join(sorted(statuses))}."
        )
        retained_artifact_coverage.append(
            {
                "path": path,
                "status": status,
                "sha256": digest,
                "size_bytes": canonical.get("size_bytes") if canonical else rows[0].get("size_bytes"),
                "role": canonical.get("kind") if canonical else str(rows[0].get("role", "unknown")),
                "selectors": selectors,
                "notes": notes,
            }
        )
        runner_errors.append(
            (
                "artifact_coverage_duplicate",
                f"{path}: merged {len(rows)} coverage rows",
            )
        )
    canonicalized_coverage: list[dict[str, Any]] = []
    for item in retained_artifact_coverage:
        path = str(item.get("path"))
        canonical = canonical_artifacts.get(path)
        if canonical is None:
            runner_errors.append(
                (
                    "artifact_coverage_nonexistent",
                    f"{path}: model coverage path is not in the runner inventory",
                )
            )
            continue
        canonicalized_coverage.append(
            {
                "path": canonical["path"],
                "status": (
                    "unreadable"
                    if canonical.get("status") == "unreadable"
                    or not isinstance(canonical.get("sha256"), str)
                    else item["status"]
                ),
                "sha256": canonical["sha256"],
                "size_bytes": canonical["size_bytes"],
                "role": canonical["kind"],
                "selectors": item.get("selectors", []),
                "notes": item.get("notes", []),
            }
        )
    retained_artifact_coverage = canonicalized_coverage
    represented_paths = {
        item.get("path")
        for item in retained_artifact_coverage
        if isinstance(item.get("path"), str)
    }
    for path, canonical in canonical_artifacts.items():
        if path in represented_paths:
            continue
        retained_artifact_coverage.append(
            {
                "path": path,
                "status": (
                    "unreadable"
                    if canonical.get("status") == "unreadable"
                    or not isinstance(canonical.get("sha256"), str)
                    else "skipped"
                ),
                "sha256": canonical["sha256"],
                "size_bytes": canonical["size_bytes"],
                "role": canonical["kind"],
                "selectors": [],
                "notes": [
                    "Discovered and hashed by the runner but not semantically reviewed by the model."
                ],
            }
        )
        runner_errors.append(
            ("artifact_coverage_missing", f"{path}: model report omitted this run artifact")
        )
    retained_artifact_coverage.sort(key=lambda item: str(item.get("path")))
    coverage["artifacts"] = retained_artifact_coverage

    errors = list(model_errors)
    errors.extend(f"{code}: {message}" for code, message in runner_errors)
    errors.extend(f"citation_unresolved: {message}" for message in sorted(set(resolution_errors)))
    if runner_errors:
        coverage.setdefault("notes", []).extend(
            f"Runner limitation: {code}: {message}" for code, message in runner_errors
        )
    if resolution_errors:
        coverage.setdefault("notes", []).extend(
            f"Citation resolution: {message}" for message in sorted(set(resolution_errors))
        )
    report["schema_version"] = 2
    report["kind"] = "bench_investigation"
    report["generated_at_utc"] = utc_now()
    final_state = (
        "failed"
        if model_state == "failed"
        else "partial"
        if model_state == "partial" or model_errors or runner_errors or resolution_errors
        else "completed"
    )
    report["execution_status"] = {
        "state": final_state,
        "summary": (
            model_summary + (" Runner recorded additional limitations." if runner_errors or resolution_errors else "")
        ),
        "errors": errors,
    }
    report["model"] = {
        "backend": backend,
        "name": model,
        "tool_version": tool_version,
        "prompt_sha256": sha256_bytes(prompt.encode("utf-8")),
        "instruction_hashes": [
            {"path": path.relative_to(ROOT).as_posix(), "sha256": sha256_file(path)}
            for path in (INSTRUCTION_PATH, SCHEMA_PATH)
        ],
    }
    report = redact_report_paths(report, run_dir)
    report = sanitize_investigation_report(report, run_dir)
    schema_errors = validate_report_schema(report)
    schema_errors.extend(validate_published_selectors(run_dir, report))
    if schema_errors:
        raise InvestigationError(
            "model_output_invalid",
            "Post-processed report violates schema: " + "; ".join(schema_errors[:8]),
        )
    return report


def failure_report(
    *,
    code: str,
    message: str,
    model: str,
    backend: str,
    tool_version: str,
    prompt: str,
    artifacts: Sequence[Mapping[str, Any]],
    source_context_value: Mapping[str, Any],
    video_history: Sequence[Mapping[str, Any]],
    attachments: Sequence[Mapping[str, Any]] = (),
) -> dict[str, Any]:
    recorded_revisions = sorted(recorded_identity_revisions(source_context_value))
    video_notes: dict[str, list[str]] = {}
    for item in video_history:
        path = item.get("path")
        if not isinstance(path, str):
            continue
        scan = item.get("temporal_scan") if isinstance(item.get("temporal_scan"), Mapping) else {}
        video_notes.setdefault(path, []).append(
            "Local visual preparation before model execution: "
            f"status={item.get('status', 'unknown')}, "
            f"scan={scan.get('status', 'unknown')}, "
            f"samples={scan.get('sample_count', 0)}, "
            f"change_candidates={len(scan.get('change_candidates', []))}."
        )
    return {
        "schema_version": 2,
        "kind": "bench_investigation",
        "generated_at_utc": utc_now(),
        "execution_status": {
            "state": "failed",
            "summary": "The investigation backend did not produce usable results.",
            "errors": [f"{code}: {message}"],
        },
        "source": {
            "basis": "unavailable",
            "summary": "Source attribution was not completed.",
            "recorded_revisions": recorded_revisions,
            "inspected_revision": None,
            "identity_evidence": [],
            "mismatches": [],
            "binary_identities": [],
            "limitations": ["Model execution failed; runner source prechecks are not code attribution."],
        },
        "coverage": {
            "artifacts": [
                {
                    "path": str(item["path"]),
                    "status": (
                        "unreadable"
                        if item.get("status") == "unreadable"
                        or not isinstance(item.get("sha256"), str)
                        else "skipped"
                    ),
                    "sha256": (
                        item.get("sha256")
                        if isinstance(item.get("sha256"), str)
                        else None
                    ),
                    "size_bytes": (
                        item.get("size_bytes")
                        if isinstance(item.get("size_bytes"), int)
                        and not isinstance(item.get("size_bytes"), bool)
                        else None
                    ),
                    "role": str(item["kind"]),
                    "notes": [
                        "Discovered by the runner but not semantically reviewed.",
                        *video_notes.get(str(item["path"]), []),
                    ],
                }
                for item in artifacts
            ],
            "attachments": copy.deepcopy(list(attachments)),
            "code": [],
            "video_intervals": [],
            "clock_mappings": [],
            "notes": ["No artifact coverage is claimed because model execution failed."],
        },
        "findings": [],
        "unresolved": [],
        "model": {
            "backend": backend,
            "name": model,
            "tool_version": tool_version,
            "prompt_sha256": sha256_bytes(prompt.encode("utf-8")),
            "instruction_hashes": [
                {"path": path.relative_to(ROOT).as_posix(), "sha256": sha256_file(path)}
                for path in (INSTRUCTION_PATH, SCHEMA_PATH)
                if path.is_file()
            ],
        },
        "video_requests": [],
    }


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    raw_arguments = list(argv) if argv is not None else sys.argv[1:]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", help="Completed or partial bench run directory")
    parser.add_argument(
        "--model",
        default=os.environ.get("BENCH_INVESTIGATOR_MODEL"),
        help=(
            f"Model name (local default: {LOCAL_DEFAULT_MODEL}; hosted default: "
            f"{HOSTED_DEFAULT_MODEL}; must accept image input when the run contains video)"
        ),
    )
    parser.add_argument(
        "--hosted",
        action="store_true",
        help="Explicitly allow hosted inference; local Ollama is used otherwise",
    )
    parser.add_argument(
        "--local-provider",
        choices=("ollama", "lmstudio"),
        default=os.environ.get("BENCH_INVESTIGATOR_LOCAL_PROVIDER") or LOCAL_DEFAULT_PROVIDER,
        help=f"Local provider (default: {LOCAL_DEFAULT_PROVIDER}; incompatible with --hosted)",
    )
    parser.add_argument(
        "--codex-executable",
        default=resolve_codex_executable(),
    )
    parser.add_argument("--max-video-passes", type=int, default=2)
    parser.add_argument("--timeout-seconds", type=int, default=3600)
    parser.add_argument("--debug-transcript", action="store_true")
    args = parser.parse_args(argv)
    if not 1 <= args.max_video_passes <= 3:
        parser.error("--max-video-passes must be between 1 and 3")
    if args.timeout_seconds < 1:
        parser.error("--timeout-seconds must be positive")
    if args.local_provider not in {"ollama", "lmstudio"}:
        parser.error("--local-provider must be ollama or lmstudio")
    if args.hosted:
        if any(
            argument == "--local-provider" or argument.startswith("--local-provider=")
            for argument in raw_arguments
        ):
            parser.error("--hosted cannot be combined with --local-provider")
        args.local_provider = None
        if not args.model:
            args.model = HOSTED_DEFAULT_MODEL
    elif not args.model:
        args.model = LOCAL_DEFAULT_MODEL
    if (
        not args.hosted
        and args.local_provider == "ollama"
        and is_ollama_cloud_model(args.model)
    ):
        parser.error("Ollama cloud model tags are not allowed in local mode; use --hosted")
    return args


def main() -> int:
    args = parse_args()
    requested_run_dir = Path(args.run_dir).absolute()
    run_dir = requested_run_dir.resolve()
    if not run_dir.is_dir():
        detail = sanitize_error(
            f"bench investigation: run directory does not exist: {run_dir}",
            (requested_run_dir, run_dir),
        )
        print(detail, file=sys.stderr)
        return 3
    output_path = run_dir / "investigation.json"
    artifacts = discover_artifacts(run_dir)
    source: dict[str, Any] = {"current_head": None, "identities": []}
    tool_version = codex_version(args.codex_executable)
    local_execution = not args.hosted
    backend = (
        f"codex exec --oss --local-provider {args.local_provider}"
        if local_execution
        else "codex exec"
    )
    prior_report: dict[str, Any] | None = None
    prior_prompt = ""
    final_prompt = ""
    debug_events: list[dict[str, Any]] = []
    runner_errors: list[tuple[str, str]] = []
    video_history: list[dict[str, Any]] = []
    evidence_index: dict[str, Any] = {}

    with tempfile.TemporaryDirectory(prefix="bench-investigation-") as temporary:
        workspace = Path(temporary)
        attachments: list[dict[str, Any]] = []
        attachment_manifest: list[dict[str, Any]] = []
        initial_omitted_images = 0
        global_omitted_images = 0
        processed_videos = 0
        omitted_videos: list[tuple[int, str]] = []
        requests: list[Mapping[str, Any]] = []
        try:
            evidence_index = build_evidence_index(run_dir)
            source = source_context(run_dir, artifacts)
            model_pass_limit = max(2, args.max_video_passes)
            for pass_number in range(1, model_pass_limit + 1):
                if pass_number <= args.max_video_passes:
                    video, extracted_attachments, processed_count, omitted_paths = (
                        extract_video_evidence(
                            run_dir,
                            artifacts,
                            workspace / f"pass_{pass_number}",
                            requests,
                            scan_overview=pass_number == 1,
                            pass_number=pass_number,
                            remaining_run_budget=MAX_VIDEOS_PER_RUN - processed_videos,
                            index_dir=run_dir / INDEX_DIRECTORY,
                        )
                    )
                else:
                    if requests:
                        runner_errors.append(
                            (
                                "video_request_limit",
                                f"{len(requests)} video request(s) from pass "
                                f"{pass_number - 1} were not extracted because video "
                                f"evidence extraction ended after "
                                f"{args.max_video_passes} pass(es)",
                            )
                        )
                    video, extracted_attachments, processed_count, omitted_paths = (
                        [],
                        [],
                        0,
                        [],
                    )
                processed_videos += processed_count
                omitted_videos.extend((pass_number, path) for path in omitted_paths)
                video_history.extend(video)
                attachment_limit = attachment_limit_for_pass(
                    pass_number, args.max_video_passes
                )
                omitted_now = append_bounded_attachments(
                    attachments,
                    extracted_attachments,
                    limit=attachment_limit,
                )
                if pass_number == 1 and attachment_limit < MAX_ATTACHED_IMAGES:
                    initial_omitted_images += omitted_now
                else:
                    global_omitted_images += omitted_now
                attachment_manifest = persist_attachment_manifest(run_dir, attachments)
                context = compact_context(
                    run_dir,
                    artifacts,
                    source,
                    video_history,
                    prior_report,
                    pass_number,
                    {
                        "limit": MAX_ATTACHED_IMAGES,
                        "current_pass_limit": attachment_limit,
                        "reserved_for_follow_up": max(
                            0, MAX_ATTACHED_IMAGES - attachment_limit
                        ),
                        "attached_count": len(attachments),
                        "omitted_count": initial_omitted_images
                        + global_omitted_images,
                        "omitted_by_reason": {
                            "initial_allocation": initial_omitted_images,
                            "global_limit": global_omitted_images,
                        },
                        "manifest": attachment_manifest,
                    },
                    evidence_index,
                )
                final_prompt = build_prompt(context, len(attachments), pass_number)
                pass_output = workspace / f"model_pass_{pass_number}.json"
                try:
                    report, stdout, stderr = invoke_codex(
                        executable=args.codex_executable,
                        model=args.model,
                        oss=local_execution,
                        local_provider=args.local_provider,
                        prompt=final_prompt,
                        images=[item["file"] for item in attachments],
                        output_path=pass_output,
                        timeout_seconds=args.timeout_seconds,
                        private_paths=(requested_run_dir, run_dir, workspace),
                    )
                except Exception as exc:
                    if prior_report is None:
                        raise
                    failure_code = (
                        exc.code
                        if isinstance(exc, InvestigationError)
                        else type(exc).__name__
                    )
                    failure_detail = sanitize_error(
                        str(exc), (requested_run_dir, run_dir, workspace)
                    )
                    runner_errors.append(
                        (
                            "followup_backend_failed",
                            f"pass {pass_number} {failure_code}: {failure_detail}",
                        )
                    )
                    debug_events.append(
                        {
                            "pass": pass_number,
                            "backend_status": "failed",
                            "error_code": failure_code,
                            "attached_images": len(attachments),
                        }
                    )
                    break
                debug_events.append(
                    {
                        "pass": pass_number,
                        "backend_status": "completed",
                        "stdout_bytes": len(stdout.encode("utf-8")),
                        "stderr_bytes": len(stderr.encode("utf-8")),
                        "attached_images": len(attachments),
                    }
                )
                prior_report = redact_report_paths(
                    report, requested_run_dir, (run_dir, workspace)
                )
                prior_prompt = final_prompt
                raw_requests = report.get("video_requests", [])
                requests = [item for item in raw_requests if isinstance(item, Mapping)]
                if not requests and pass_number > 1:
                    break
            if prior_report is None:
                raise InvestigationError("model_output_missing", "No model pass completed")
            runner_errors.extend(
                image_attachment_limitations(
                    initial_omitted_images,
                    global_omitted_images,
                    len(attachments),
                )
            )
            if omitted_videos:
                omitted = ", ".join(
                    f"pass {pass_number}:{path}" for pass_number, path in omitted_videos
                )
                runner_errors.append(
                    (
                        "video_file_limit",
                        f"{len(omitted_videos)} video extraction(s) were omitted before processing: {omitted}",
                    )
                )
            if requests:
                runner_errors.append(
                    (
                        "video_request_limit",
                        f"{len(requests)} video request(s) remain after {args.max_video_passes} pass(es)",
                    )
                )
            fresh_source = source_context(run_dir, artifacts)
            report = finish_report(
                prior_report,
                run_dir=run_dir,
                source_context_value=fresh_source,
                model=args.model,
                backend=backend,
                tool_version=tool_version,
                prompt=prior_prompt,
                extra_errors=runner_errors,
                attachment_manifest=attachment_manifest,
            )
            exit_status = 2 if report["execution_status"]["state"] == "failed" else 0
        except InvestigationError as exc:
            report = failure_report(
                code=exc.code,
                message=str(exc),
                model=args.model,
                backend=backend,
                tool_version=tool_version,
                prompt=final_prompt,
                artifacts=artifacts,
                source_context_value=source,
                video_history=video_history,
                attachments=attachment_manifest,
            )
            exit_status = 2
        except Exception as exc:
            report = failure_report(
                code="postprocessing_failed",
                message=sanitize_error(
                    f"{type(exc).__name__}: {exc}",
                    (requested_run_dir, run_dir, workspace),
                ),
                model=args.model,
                backend=backend,
                tool_version=tool_version,
                prompt=final_prompt,
                artifacts=artifacts,
                source_context_value=source,
                video_history=video_history,
                attachments=attachment_manifest,
            )
            exit_status = 2
        report = redact_report_paths(report, requested_run_dir, (run_dir, workspace))
        report = sanitize_investigation_report(report, requested_run_dir)
        final_schema_errors = validate_report_schema(report)
        final_schema_errors.extend(validate_published_attachments(run_dir, report))
        final_schema_errors.extend(validate_published_selectors(run_dir, report))
        if final_schema_errors:
            report = failure_report(
                code="postprocessing_invalid",
                message="Postprocessed report violated schema: " + "; ".join(final_schema_errors[:8]),
                model=args.model,
                backend=backend,
                tool_version=tool_version,
                prompt=final_prompt,
                artifacts=artifacts,
                source_context_value=source,
                video_history=video_history,
                attachments=attachment_manifest,
            )
            report = redact_report_paths(report, requested_run_dir, (run_dir, workspace))
            report = sanitize_investigation_report(report, requested_run_dir)
            failure_errors = validate_report_schema(report)
            failure_errors.extend(validate_published_attachments(run_dir, report))
            failure_errors.extend(validate_published_selectors(run_dir, report))
            if failure_errors:
                print("bench investigation: could not construct a valid failure report", file=sys.stderr)
                return 2
            exit_status = 2
        atomic_write_json(output_path, report)
        prune_errors = prune_attachment_files(run_dir, report["coverage"]["attachments"])
        if prune_errors:
            report["execution_status"]["errors"].extend(
                f"attachment_prune_failed: {error}" for error in prune_errors
            )
            if report["execution_status"]["state"] != "failed":
                report["execution_status"]["state"] = "partial"
            report["coverage"]["notes"].append(
                "Runner limitation: one or more stale contact sheets could not be removed."
            )
            atomic_write_json(output_path, report)
        if args.debug_transcript:
            atomic_write_json(run_dir / "investigation_debug.json", {"passes": debug_events})

    state = report["execution_status"]["state"]
    output_label = output_path.name
    print(
        f"Bench investigation {state}: {len(report.get('findings', []))} finding(s), "
        f"{len(report.get('unresolved', []))} unresolved; {output_label}"
    )
    if state == "failed":
        execution_status = report.get("execution_status", {})
        summary = execution_status.get("summary")
        errors = execution_status.get("errors", [])
        if isinstance(summary, str) and summary:
            print(f"Reason: {summary}", file=sys.stderr)
        if isinstance(errors, list) and errors and isinstance(errors[0], str):
            print(f"Detail: {errors[0]}", file=sys.stderr)
    return exit_status


if __name__ == "__main__":
    raise SystemExit(main())
