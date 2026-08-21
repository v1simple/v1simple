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
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_PATH = ROOT / "tools" / "bench_investigation.schema.json"
INSTRUCTION_PATH = ROOT / "tools" / "bench_investigator_prompt.md"
AGENTS_PATH = ROOT / "AGENTS.md"
VIDEO_SUFFIXES = {".mov", ".mp4", ".m4v", ".mkv"}
IGNORED_OUTPUTS = {"investigation.json", "investigation_debug.json"}
MAX_INITIAL_IMAGES = 8
MAX_ATTACHED_IMAGES = 12
MAX_SAMPLED_GAP_ANOMALIES = 32
MAX_VIDEOS_PER_PASS = 8
MAX_VIDEOS_PER_RUN = 12
LOCAL_DEFAULT_MODEL = "qwen3-vl:8b"
LOCAL_DEFAULT_PROVIDER = "ollama"
HOSTED_DEFAULT_MODEL = "gpt-5.6-sol"
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


def validate_report_schema(report: Mapping[str, Any]) -> list[str]:
    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise InvestigationError("schema_unavailable", f"Could not read report schema: {exc}") from exc
    return validate_schema_value(report, schema, schema)


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
        if path.name in IGNORED_OUTPUTS or path.name.startswith(".investigation."):
            continue
        if path.is_symlink() or not path.is_file():
            continue
        try:
            relative = path.relative_to(run_dir).as_posix()
            size = path.stat().st_size
        except OSError:
            continue
        artifacts.append({"path": relative, "size_bytes": size, "kind": artifact_kind(path)})
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


def ordered_attachment_manifest(attachments: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
    return [
        {"attachment_index": index, **item["manifest"]}
        for index, item in enumerate(attachments, 1)
    ]


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


def extract_video_evidence(
    run_dir: Path,
    artifacts: Sequence[Mapping[str, Any]],
    workspace: Path,
    requests: Sequence[Mapping[str, Any]],
    *,
    scan_overview: bool,
    pass_number: int,
    remaining_run_budget: int,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], int, list[str]]:
    inspect_video = load_video_helper()
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
    candidate_paths = (
        [str(item["path"]) for item in artifacts if item.get("kind") == "video"]
        if scan_overview
        else list(by_video)
    )
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
        output = workspace / f"video_{index:03d}"
        try:
            evidence = inspect_video(
                video,
                output,
                by_video.get(relative, []),
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
            attachment = (
                {
                    "file": image,
                    "manifest": {
                        "pass": pass_number,
                        "video_path": relative,
                        "filename": sheet["filename"],
                        "purpose": purpose,
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


def compact_context(
    run_dir: Path,
    artifacts: Sequence[Mapping[str, Any]],
    source: Mapping[str, Any],
    video: Sequence[Mapping[str, Any]],
    prior_report: Mapping[str, Any] | None,
    pass_number: int,
    image_attachments: Mapping[str, Any],
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
        "source_precheck": source,
        "video_extraction": compact_video,
        "investigation_pass": pass_number,
        "image_attachments": dict(image_attachments),
    }
    if prior_report is not None:
        context["prior_report_to_recheck_and_improve"] = prior_report
    return json.dumps(context, indent=2, sort_keys=True, allow_nan=False)


def build_prompt(context: str, image_count: int) -> str:
    return f"""Read AGENTS.md and tools/bench_investigator_prompt.md completely, then investigate the run below.

You are in a read-only repository session. Inspect files and Git directly with shell tools; do not rely only on this inventory. The {image_count} attached image(s), if any, are generic whole-video overview/change or requested-interval contact sheets. The one-based image_attachments.manifest maps their attachment order to root-owned video_extraction metadata and cell timestamps. A prior report means this is a fresh stateless follow-up: recheck it against all evidence and replace it with a better final report.

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


def sanitize_error(text: str, private_paths: Sequence[Path] = ()) -> str:
    replacements = [(str(ROOT), "<repo>"), (str(Path.home()), "<home>")]
    for path in private_paths:
        replacements.extend(((str(path), "<private>"), (str(path.resolve()), "<private>")))
    return _replace_private_paths(text, replacements)


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
        "--model",
        model,
        "--output-schema",
        str(SCHEMA_PATH),
        "--output-last-message",
        str(output_path),
        "--color",
        "never",
    ]
    if oss:
        command.append("--oss")
    if local_provider:
        command.extend(["--local-provider", local_provider])
    for image in images:
        command.extend(["--image", str(image)])
    command.extend(["-C", str(ROOT), "-"])
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
            sanitize_error(f"Codex executable not found: {executable}", private_paths),
        ) from exc
    except subprocess.TimeoutExpired as exc:
        raise InvestigationError("backend_timeout", f"Codex exceeded {timeout_seconds} seconds") from exc
    if process.returncode != 0:
        detail = "\n".join(line for line in process.stderr.splitlines()[-8:] if line.strip())
        raise InvestigationError(
            "backend_failed",
            sanitize_error(detail or f"Codex exited {process.returncode}", private_paths),
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
    schema_errors = validate_report_schema(report)
    if schema_errors:
        raise InvestigationError(
            "model_output_invalid",
            "Raw model report violates schema: " + "; ".join(schema_errors[:8]),
        )
    return report, process.stdout, process.stderr


def iter_evidence_selectors(value: Any) -> Iterable[dict[str, Any]]:
    if isinstance(value, dict):
        if "path" in value and "sha256" in value and value.get("kind") in {
            "file",
            "json_pointer",
            "ndjson",
            "csv",
            "log",
            "video",
        }:
            yield value
        for child in value.values():
            yield from iter_evidence_selectors(child)
    elif isinstance(value, list):
        for child in value:
            yield from iter_evidence_selectors(child)


def iter_code_selectors(value: Any) -> Iterable[Mapping[str, Any]]:
    if isinstance(value, dict):
        if {"revision", "path", "symbol", "line_start", "line_end"}.issubset(value):
            yield value
        for child in value.values():
            yield from iter_code_selectors(child)
    elif isinstance(value, list):
        for child in value:
            yield from iter_code_selectors(child)


def code_text(selector: Mapping[str, Any]) -> str | None:
    path = selector.get("path")
    revision = selector.get("revision")
    if not isinstance(path, str) or not isinstance(revision, str):
        return None
    current = safe_relative_file(ROOT, path)
    if revision.upper().startswith("WORKTREE"):
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


def probe_video_bounds(path: Path) -> tuple[float, int | None] | None:
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        return None
    try:
        process = subprocess.run(
            [
                ffprobe,
                "-v", "error",
                "-select_streams", "v:0",
                "-count_frames",
                "-show_entries", "format=duration:stream=duration,nb_frames,nb_read_frames",
                "-of", "json",
                str(path),
            ],
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
        raw_frame_count = stream.get("nb_read_frames") or stream.get("nb_frames")
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


def resolve_artifact_selector(
    run_dir: Path,
    selector: dict[str, Any],
    video_bounds_cache: dict[Path, tuple[float, int | None] | None] | None = None,
) -> str | None:
    path = safe_relative_file(run_dir, selector.get("path"))
    if path is None:
        return f"artifact path does not resolve: {selector.get('path')!r}"
    actual_hash = sha256_file(path)
    if selector.get("sha256") != actual_hash:
        return f"artifact hash does not match: {selector.get('path')}"
    selector_type = selector.get("kind")
    if selector_type in {"log", "ndjson"}:
        start = selector.get("line_start")
        end = selector.get("line_end")
        try:
            line_count = sum(1 for _line in path.open("r", encoding="utf-8", errors="replace"))
        except OSError:
            return f"artifact lines are unreadable: {selector.get('path')}"
        if (
            not isinstance(start, int)
            or isinstance(start, bool)
            or not isinstance(end, int)
            or isinstance(end, bool)
            or start < 1
            or start > end
            or end > line_count
        ):
            return f"artifact line range does not resolve: {selector.get('path')}:{start}-{end}"
        if selector_type == "ndjson" and selector.get("keys"):
            try:
                selected = path.read_text(encoding="utf-8").splitlines()[start - 1 : end]
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
            reader = csv.DictReader(lines)
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
            or (frame_count is not None and last_frame >= frame_count)
        ):
            return f"video frame range does not resolve: {selector.get('path')}:{first_frame}-{last_frame}"
    return None


def resolve_code_selector(selector: Mapping[str, Any]) -> str | None:
    content = code_text(selector)
    if content is None:
        return f"code selector does not resolve: {selector.get('revision')}:{selector.get('path')}"
    start = selector.get("line_start")
    end = selector.get("line_end")
    line_count = len(content.splitlines())
    if not isinstance(start, int) or not isinstance(end, int) or start > end or end > line_count:
        return f"code line range does not resolve: {selector.get('path')}:{start}-{end}"
    return None


def validate_conclusion(
    run_dir: Path,
    conclusion: dict[str, Any],
    video_bounds_cache: dict[Path, tuple[float, int | None] | None],
) -> list[str]:
    errors: list[str] = []
    for key in ("evidence", "counterevidence"):
        for selector in conclusion.get(key, []):
            if isinstance(selector, dict):
                error = resolve_artifact_selector(run_dir, selector, video_bounds_cache)
                if error:
                    errors.append(error)
    for selector in conclusion.get("code", []):
        if isinstance(selector, Mapping):
            error = resolve_code_selector(selector)
            if error:
                errors.append(error)
    for hypothesis in conclusion.get("hypotheses", []):
        if isinstance(hypothesis, dict):
            errors.extend(validate_conclusion(run_dir, hypothesis, video_bounds_cache))
    return errors


def index_clock_mappings(
    run_dir: Path,
    mappings: Sequence[Mapping[str, Any]],
    video_bounds_cache: dict[Path, tuple[float, int | None] | None],
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
        valid_evidence = 0
        mapping_evidence_errors: list[str] = []
        for selector in mapping.get("evidence", []):
            error = resolve_artifact_selector(run_dir, selector, video_bounds_cache)
            if error:
                mapping_evidence_errors.append(error)
                errors.append(f"clock mapping {mapping_id}: {error}")
            else:
                valid_evidence += 1
        if mapping_id in duplicates:
            continue
        status = mapping.get("status")
        if status == "unavailable":
            support[mapping_id] = f"clock mapping is unavailable: {mapping_id}"
        elif valid_evidence == 0:
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
    finding: Mapping[str, Any], primary_errors: Sequence[str]
) -> dict[str, Any] | None:
    evidence = finding.get("evidence")
    code = finding.get("code")
    if not isinstance(evidence, list) or not evidence or not isinstance(code, list) or not code:
        return None
    cause = str(finding.get("cause") or "The reported cause is not established.")
    return {
        "id": finding["id"],
        "title": finding["title"],
        "causal_status": "unknown",
        "observation": finding["observed_behavior"],
        "why_unknown": "Primary citation resolution failed: " + "; ".join(primary_errors),
        "evidence": evidence,
        "counterevidence": finding.get("counterevidence", []),
        "code": code,
        "clock_mapping_ids": finding.get("clock_mapping_ids", []),
        "hypotheses": [
            {
                "rank": 1,
                "description": cause,
                "evidence": evidence,
                "code": code,
            }
        ],
        "next_observation": {
            "description": "Resolve the primary artifact and owning code citations before acting on this cause.",
            "distinguishes": [cause, "The cited observation or code attribution is incorrect."],
            "minimal_evidence": "One resolvable primary run selector and one resolvable owning-code selector.",
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
) -> dict[str, Any]:
    raw_schema_errors = validate_report_schema(report)
    if raw_schema_errors:
        raise InvestigationError(
            "model_output_invalid",
            "Raw model report violates schema: " + "; ".join(raw_schema_errors[:8]),
        )
    model_execution = report["execution_status"]
    model_state = model_execution.get("state")
    model_summary = str(model_execution.get("summary") or "Model investigation returned no summary.")
    model_errors = model_execution.get("errors")

    source_report = report["source"]
    model_basis = source_report.get("basis")
    prechecked_basis = conservative_source_basis(source_context_value)
    effective_basis = min((str(model_basis), prechecked_basis), key=SOURCE_BASIS_RANK.__getitem__)
    runner_errors = list(extra_errors)
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
    clock_support, duplicate_clock_ids, clock_errors = index_clock_mappings(
        run_dir, coverage["clock_mappings"], video_bounds_cache
    )
    resolution_errors.extend(clock_errors)

    retained_findings: list[dict[str, Any]] = []
    converted_unresolved: list[dict[str, Any]] = []
    for finding in report["findings"]:
        evidence_results = [
            resolve_artifact_selector(run_dir, selector, video_bounds_cache)
            for selector in finding["evidence"]
        ]
        primary_errors = [error for error in evidence_results if error]
        primary_errors.extend(
            error
            for selector in finding["code"]
            if (error := resolve_code_selector(selector))
        )
        secondary_errors = [
            error
            for selector in finding["counterevidence"]
            if (error := resolve_artifact_selector(run_dir, selector, video_bounds_cache))
        ]
        secondary_errors.extend(
            clock_reference_errors(finding, clock_support, duplicate_clock_ids)
        )
        resolution_errors.extend(primary_errors)
        resolution_errors.extend(secondary_errors)
        if primary_errors:
            unresolved = unresolved_from_finding(
                finding, primary_errors + secondary_errors
            )
            if unresolved is None:
                runner_errors.append(
                    (
                        "finding_omitted",
                        f"{finding['id']}: primary evidence or code did not resolve",
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

    for unresolved in report["unresolved"]:
        errors = validate_conclusion(run_dir, unresolved, video_bounds_cache)
        errors.extend(clock_reference_errors(unresolved, clock_support, duplicate_clock_ids))
        if errors:
            unresolved["why_unknown"] = (
                str(unresolved.get("why_unknown", ""))
                + " Citation resolution: "
                + "; ".join(errors)
            ).strip()
            resolution_errors.extend(errors)
    report["unresolved"].extend(converted_unresolved)
    for selector in iter_evidence_selectors(report):
        error = resolve_artifact_selector(run_dir, selector, video_bounds_cache)
        if error:
            resolution_errors.append(error)
    for selector in iter_code_selectors(report.get("coverage", {})):
        error = resolve_code_selector(selector)
        if error:
            resolution_errors.append(error)

    artifact_coverage = report.get("coverage", {}).get("artifacts", [])
    for item in artifact_coverage:
        if not isinstance(item, dict) or item.get("sha256") is None:
            continue
        path = safe_relative_file(run_dir, item.get("path"))
        if path is None:
            resolution_errors.append(f"covered artifact does not resolve: {item.get('path')!r}")
            continue
        actual_hash = sha256_file(path)
        if item.get("sha256") != actual_hash:
            resolution_errors.append(f"covered artifact hash does not match: {item.get('path')}")

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
    report["schema_version"] = 1
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
            for path in (AGENTS_PATH, INSTRUCTION_PATH, SCHEMA_PATH)
        ],
    }
    report = redact_report_paths(report, run_dir)
    schema_errors = validate_report_schema(report)
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
) -> dict[str, Any]:
    recorded_revisions = sorted(
        {
            str(item["recorded_revision"])
            for item in source_context_value.get("identities", [])
            if isinstance(item, Mapping) and item.get("recorded_revision")
        }
    )
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
        "schema_version": 1,
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
                    "status": "skipped",
                    "sha256": None,
                    "size_bytes": int(item["size_bytes"]),
                    "role": str(item["kind"]),
                    "notes": [
                        "Discovered by the runner but not semantically reviewed.",
                        *video_notes.get(str(item["path"]), []),
                    ],
                }
                for item in artifacts
            ],
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
                for path in (AGENTS_PATH, INSTRUCTION_PATH, SCHEMA_PATH)
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
        default=os.environ.get("BENCH_INVESTIGATOR_CODEX") or shutil.which("codex") or "codex",
    )
    parser.add_argument("--max-video-passes", type=int, default=2)
    parser.add_argument("--timeout-seconds", type=int, default=1800)
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
        print(f"bench investigation: run directory does not exist: {run_dir}", file=sys.stderr)
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
    final_prompt = ""
    debug_events: list[dict[str, Any]] = []
    runner_errors: list[tuple[str, str]] = []
    video_history: list[dict[str, Any]] = []

    with tempfile.TemporaryDirectory(prefix="bench-investigation-") as temporary:
        workspace = Path(temporary)
        attachments: list[dict[str, Any]] = []
        initial_omitted_images = 0
        global_omitted_images = 0
        processed_videos = 0
        omitted_videos: list[tuple[int, str]] = []
        requests: list[Mapping[str, Any]] = []
        try:
            source = source_context(run_dir, artifacts)
            for pass_number in range(1, args.max_video_passes + 1):
                video, extracted_attachments, processed_count, omitted_paths = extract_video_evidence(
                    run_dir,
                    artifacts,
                    workspace / f"pass_{pass_number}",
                    requests,
                    scan_overview=pass_number == 1,
                    pass_number=pass_number,
                    remaining_run_budget=MAX_VIDEOS_PER_RUN - processed_videos,
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
                attachment_manifest = ordered_attachment_manifest(attachments)
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
                )
                final_prompt = build_prompt(context, len(attachments))
                pass_output = workspace / f"model_pass_{pass_number}.json"
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
                raw_requests = report.get("video_requests", [])
                requests = [item for item in raw_requests if isinstance(item, Mapping)]
                if not requests:
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
                prompt=final_prompt,
                extra_errors=runner_errors,
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
            )
            exit_status = 2
        report = redact_report_paths(report, requested_run_dir, (run_dir, workspace))
        final_schema_errors = validate_report_schema(report)
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
            )
            report = redact_report_paths(report, requested_run_dir, (run_dir, workspace))
            if validate_report_schema(report):
                print("bench investigation: could not construct a valid failure report", file=sys.stderr)
                return 2
            exit_status = 2
        atomic_write_json(output_path, report)
        if args.debug_transcript:
            atomic_write_json(run_dir / "investigation_debug.json", {"passes": debug_events})

    state = report["execution_status"]["state"]
    try:
        output_label = output_path.relative_to(ROOT).as_posix()
    except ValueError:
        output_label = output_path.name
    print(
        f"Bench investigation {state}: {len(report.get('findings', []))} finding(s), "
        f"{len(report.get('unresolved', []))} unresolved; {output_label}"
    )
    return exit_status


if __name__ == "__main__":
    raise SystemExit(main())
