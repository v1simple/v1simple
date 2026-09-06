#!/usr/bin/env python3
"""Build static-reader and blind temporal-reader qualification evidence.

``reanalyze-static`` rereads an immutable blind static bundle with the current
reader, keeps the original labels and source artifacts unchanged, and emits a
new static-only manifest only after the complete verifier accepts it. An explicit
independent frequency supplement can adjudicate a label while retaining its original.

The three stages keep the useful boundary explicit:

* ``freeze`` records the exact unallowlisted classifiers, reader runtime and
  observer rubric without reading a camera recording.
* ``prepare`` runs the frozen reader/classifiers once on a capture made with
  ``bench.sh --qualification-capture`` and creates opaque observer packets for
  every admission and the frozen selection of rejections.
* ``finalize`` consumes completed blind observations, derives the qualification
  matrices, verifies a complete bundle, and publishes the policy and manifest
  together.  It never asks a person to manufacture provenance JSON.
"""

from __future__ import annotations

import argparse
import base64
from collections import Counter
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import random
import secrets
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Callable

from encounter_arrow_acquisition import CLASSIFIER_ID as ARROW_ACQUISITION_CLASSIFIER_ID
from encounter_arrow_transition import CLASSIFIER_ID as ARROW_CLASSIFIER_ID
from encounter_frequency_context import CLASSIFIER_ID as FREQUENCY_CONTEXT_CLASSIFIER_ID
from encounter_secondary_context import CLASSIFIER_ID as SECONDARY_CONTEXT_CLASSIFIER_ID
from encounter_secondary_optical_bridge import CLASSIFIER_ID as SECONDARY_OPTICAL_CLASSIFIER_ID
from encounter_qualification import (
    ACQUISITION_OBSERVER_SCOPE_FIELDS, temporal_acquisition_observer_scope,
    _validate_temporal_v2_acquisition_scope_binding,
    TEMPORAL_CANDIDATE_SELECTION, select_temporal_rejections,
    temporal_observer_clip_source_indices, temporal_selection_document,
)


BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parents[1]
POLICY_PATH = BENCH_DIR / "visible_event_policies.json"
BENCH_PATH = REPO_ROOT / "bench.sh"
def _default_manifest_path() -> Path:
    artifact_root = Path(os.environ.get(
        "BENCH_ARTIFACT_ROOT", REPO_ROOT / ".artifacts/bench"))
    return Path(os.environ.get(
        "BENCH_ENCOUNTER_QUALIFICATION", artifact_root / "qualification/encounter-reader.json"))


DEFAULT_MANIFEST = _default_manifest_path()
TARGET_CLASSIFIERS = (
    ARROW_CLASSIFIER_ID,
    ARROW_ACQUISITION_CLASSIFIER_ID,
    FREQUENCY_CONTEXT_CLASSIFIER_ID,
    SECONDARY_CONTEXT_CLASSIFIER_ID,
)
FIELD_BY_CLASSIFIER = {
    "v1-main-bar-adjacent-redraw-v2": "main_bars",
    "v1-muted-badge-rising-fill-v2": "muted_badge",
    ARROW_CLASSIFIER_ID: "main_arrows",
    ARROW_ACQUISITION_CLASSIFIER_ID: "main_arrows",
    FREQUENCY_CONTEXT_CLASSIFIER_ID: "primary_frequency",
    SECONDARY_CONTEXT_CLASSIFIER_ID: "secondary",
    SECONDARY_OPTICAL_CLASSIFIER_ID: "secondary",
}
SPEC_BY_CLASSIFIER = {
    classifier: BENCH_DIR / "temporal_specs" / f"{classifier}.json"
    for classifier in FIELD_BY_CLASSIFIER
}
BLIND_PROTOCOL = {
    "observations_completed_before_key_access": True,
    "observer_received_machine_output": False,
    "observer_received_hidden_key": False,
}
BLIND_PROTOCOL_TEMPLATE = {name: None for name in BLIND_PROTOCOL}
CAMPAIGN_NAME = "blind_temporal_qualification_campaign"
PREPARED_NAME = "blind_temporal_qualification_prepared"
CONSUMED_NAME = "CONSUMED.json"
_SHA256 = __import__("re").compile(r"[0-9a-f]{64}")


class WorkflowError(ValueError):
    """The requested qualification stage cannot preserve the blind contract."""


def _require(condition: bool, reason: str) -> None:
    if not condition:
        raise WorkflowError(reason)


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        _require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path: Path) -> Any:
    def reject_nonfinite(value: str) -> None:
        raise WorkflowError(f"non-finite JSON number: {value}")

    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_pairs,
                          parse_constant=reject_nonfinite)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise WorkflowError(f"unreadable JSON: {path}") from exc


def json_bytes(value: Any) -> bytes:
    try:
        return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise WorkflowError("qualification document is not finite JSON") from exc


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(json_bytes(value))


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _durable_replace(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}-", dir=path.parent)
    temporary_path = Path(temporary)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
        _fsync_directory(path.parent)
    finally:
        temporary_path.unlink(missing_ok=True)


def _durable_unlink(path: Path) -> None:
    path.unlink(missing_ok=True)
    _fsync_directory(path.parent)


def _write_exclusive_json(path: Path, value: Any) -> None:
    content = json_bytes(value)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}-", dir=path.parent)
    temporary_path = Path(temporary)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            os.link(temporary_path, path)
        except FileExistsError as exc:
            raise WorkflowError(
                "blind campaign was already consumed; a fresh campaign and capture are required") from exc
        _fsync_directory(path.parent)
    finally:
        temporary_path.unlink(missing_ok=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise WorkflowError(f"unreadable evidence file: {path}") from exc
    return digest.hexdigest()


def canonical_sha256(value: Any) -> str:
    try:
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                             allow_nan=False).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise WorkflowError("qualification value is not canonical JSON") from exc
    return hashlib.sha256(encoded).hexdigest()


def reference(path: Path, root: Path) -> dict[str, str]:
    resolved, resolved_root = path.resolve(), root.resolve()
    try:
        relative = resolved.relative_to(resolved_root)
    except ValueError as exc:
        raise WorkflowError(f"evidence leaves qualification root: {path}") from exc
    _require(resolved.is_file(), f"missing evidence file: {path}")
    return {"path": str(relative), "sha256": sha256(resolved)}


def resolve_reference(root: Path, value: Any, name: str) -> Path:
    _require(isinstance(value, dict), f"missing {name} reference")
    relative, expected = value.get("path"), value.get("sha256")
    _require(isinstance(relative, str) and relative and not Path(relative).is_absolute(),
             f"invalid {name} path")
    _require(isinstance(expected, str) and _SHA256.fullmatch(expected) is not None,
             f"invalid {name} hash")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as exc:
        raise WorkflowError(f"{name} leaves its qualification root") from exc
    _require(candidate.is_file() and sha256(candidate) == expected,
             f"{name} is missing or changed")
    return candidate


def _run_text(command: list[str], *, cwd: Path = REPO_ROOT) -> str:
    try:
        completed = subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise WorkflowError(f"command failed: {command[0]}") from exc
    return completed.stdout.strip()


def git_identity() -> tuple[str, bool]:
    commit = _run_text(["git", "rev-parse", "HEAD"])
    clean = not _run_text(["git", "status", "--porcelain"])
    _require(len(commit) == 40 and all(value in "0123456789abcdef" for value in commit),
             "source commit identity is invalid")
    return commit, clean


def method_hashes(policy_bytes: bytes | None = None) -> dict[str, str]:
    files = list(BENCH_DIR.glob("*.py"))
    files += list(BENCH_DIR.glob("encounter_*.swift"))
    files += list(BENCH_DIR.glob("encounter_*.png"))
    files += list(BENCH_DIR.glob("encounter_*.b64"))
    result = {path.name: sha256(path) for path in files}
    result[POLICY_PATH.name] = (hashlib.sha256(policy_bytes).hexdigest()
                                       if policy_bytes is not None else sha256(POLICY_PATH))
    return dict(sorted(result.items()))


def static_method_hashes(inventory: dict[str, str] | None = None) -> dict[str, str]:
    """Select the exact implementation inventory exercised by static evidence."""
    from encounter_qualification import STATIC_READER_IMPLEMENTATION_FILES

    inventory = method_hashes() if inventory is None else inventory
    _require(all(name in inventory for name in STATIC_READER_IMPLEMENTATION_FILES),
             "static reader implementation inventory is incomplete")
    return {name: inventory[name] for name in STATIC_READER_IMPLEMENTATION_FILES}


def reader_runtime(cache: Path) -> dict[str, Any]:
    from encounter_reader import prepare_reader
    from encounter_runtime_probe import probe_ocr_runtime

    runtime = prepare_reader(cache)
    probe = probe_ocr_runtime(runtime)
    runtime["ocr_compiled"] = runtime.get("ocr_available") is True
    runtime["ocr_runtime_probe"] = probe
    runtime["ocr_available"] = runtime["ocr_compiled"] and probe.get("status") == "operational"
    _require(
        runtime["ocr_available"],
        "Apple Vision OCR runtime probe failed: "
        f"{probe.get('reason', probe.get('status', 'unavailable'))}; "
        "run qualification from a native macOS shell with Vision access",
    )
    return runtime


def classifier_identity() -> dict[str, tuple[str, str]]:
    import encounter_arrow_acquisition as arrow_acquisition
    import encounter_arrow_transition as arrow
    import encounter_bar_transition as bar
    import encounter_frequency_context as frequency_context
    import encounter_mute_redraw_transition as mute
    import encounter_secondary_context as secondary_context
    import encounter_secondary_optical_bridge as secondary_optical

    return {
        arrow_acquisition.CLASSIFIER_ID: (
            arrow_acquisition.CLASSIFIER_ID,
            arrow_acquisition.CLASSIFIER_SPEC_SHA256),
        arrow.CLASSIFIER_ID: (arrow.CLASSIFIER_ID, arrow.CLASSIFIER_SPEC_SHA256),
        bar.CLASSIFIER_ID: (bar.CLASSIFIER_ID, bar.CLASSIFIER_SPEC_SHA256),
        frequency_context.CLASSIFIER_ID: (
            frequency_context.CLASSIFIER_ID,
            frequency_context.CLASSIFIER_SPEC_SHA256),
        mute.BADGE_CLASSIFIER_ID: (mute.BADGE_CLASSIFIER_ID, mute.BADGE_CLASSIFIER_SPEC_SHA256),
        secondary_context.CLASSIFIER_ID: (
            secondary_context.CLASSIFIER_ID,
            secondary_context.CLASSIFIER_SPEC_SHA256),
        secondary_optical.CLASSIFIER_ID: (
            secondary_optical.CLASSIFIER_ID,
            secondary_optical.CLASSIFIER_SPEC_SHA256),
    }


def _selected_classifiers(values) -> tuple[str, ...]:
    _require(isinstance(values, (list, tuple)) and bool(values)
             and all(isinstance(value, str) and value in FIELD_BY_CLASSIFIER for value in values)
             and len(values) == len(set(values)), "qualification classifier selection is invalid")
    return tuple(sorted(values))


def _campaign_classifiers(campaign: dict[str, Any]) -> tuple[str, ...]:
    classifiers = campaign.get("classifiers")
    _require(isinstance(classifiers, dict), "campaign classifier binding is missing")
    return _selected_classifiers(list(classifiers))


def _atomic_directory(destination: Path, builder: Callable[[Path], None]) -> None:
    _require(not destination.exists(), f"destination already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{destination.name}-", dir=destination.parent))
    try:
        builder(temporary)
        temporary.rename(destination)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def _campaign(path: Path) -> tuple[Path, dict[str, Any]]:
    root = path.resolve()
    document = read_json(root / "campaign.json")
    _require(isinstance(document, dict) and document.get("schema_version") == 1
             and document.get("kind") == CAMPAIGN_NAME,
             "unsupported qualification campaign")
    _campaign_classifiers(document)
    _require(document.get("candidate_selection") == TEMPORAL_CANDIDATE_SELECTION,
             "campaign candidate selection differs from the frozen workflow")
    return root, document


def _verify_frozen_source(root: Path, campaign: dict[str, Any], *, require_unallowlisted: bool,
                          runtime_cache: Path | None = None) -> None:
    from encounter_product import DEFAULT_POLICY_ID, load_policy

    commit, clean = git_identity()
    _require(clean, "qualification workflow requires a clean source tree")
    _require(commit == campaign.get("source_git_sha"), "source commit changed after qualification freeze")
    _require(sha256(BENCH_PATH) == campaign.get("bench_source_sha256"),
             "bench implementation changed after qualification freeze")
    frozen_method = campaign.get("implementation_sha256")
    current_method = method_hashes()
    _require(isinstance(frozen_method, dict), "campaign implementation binding is malformed")
    for name, digest in frozen_method.items():
        _require(current_method.get(name) == digest, f"implementation changed after freeze: {name}")
    if runtime_cache is not None:
        runtime = reader_runtime(runtime_cache)
        _require(runtime == campaign.get("reader_runtime"), "reader runtime changed after freeze")
    if require_unallowlisted:
        policy = load_policy(DEFAULT_POLICY_ID, POLICY_PATH)
        allowed = set(policy.get("qualified_temporal_classifier_ids", []))
        _require(not allowed.intersection(_campaign_classifiers(campaign)),
                 "candidate classifier was allowlisted before blind finalization")


def freeze(destination: Path, base_manifest: Path | None = None, *,
           classifier_ids: list[str] | tuple[str, ...] = TARGET_CLASSIFIERS) -> dict[str, Any]:
    """Verify the retained base, then freeze without opening a new capture."""
    from camera_contract import EXPECTED_CAMERA_NAME, EXPECTED_CAMERA_PROFILE
    from encounter_product import DEFAULT_POLICY_ID, load_policy
    from encounter_qualification import (
        CLASSIFIER_IMPLEMENTATION_FILES, TEMPORAL_V2_OBSERVER_RUBRICS,
        temporal_v2_observer_instructions,
    )

    commit, clean = git_identity()
    _require(clean, "qualification freeze requires a clean source tree")
    targets = _selected_classifiers(classifier_ids)
    policy = load_policy(DEFAULT_POLICY_ID, POLICY_PATH)
    allowed = set(policy.get("qualified_temporal_classifier_ids", []))
    _require(not allowed.intersection(targets),
             "candidate classifier is already allowlisted")
    method = method_hashes()
    identities = classifier_identity()
    _require(set(targets) <= set(identities), "classifier identities differ from workflow")

    summary: dict[str, Any] = {}

    def build(root: Path) -> None:
        runtime = reader_runtime(root / "reader-cache")
        camera = {"name": EXPECTED_CAMERA_NAME, "profile": EXPECTED_CAMERA_PROFILE}
        base_path = (base_manifest or _default_manifest_path()).resolve()
        validate_base_evidence(base_path, method, runtime, camera, policy)
        reader_binding = {
            "method_version": runtime.get("method_version"),
            "source_sha256": method["encounter_reader.py"],
            "runtime_sha256": canonical_sha256(runtime),
            "bench_source_sha256": sha256(BENCH_PATH),
        }
        classifiers: dict[str, Any] = {}
        for classifier_id in targets:
            seed = root / "frozen" / classifier_id
            spec_source = SPEC_BY_CLASSIFIER[classifier_id]
            spec = read_json(spec_source)
            spec_hash = sha256(spec_source)
            _require(identities[classifier_id] == (classifier_id, spec_hash),
                     f"classifier/spec identity differs: {classifier_id}")
            implementation = {
                name: method[name] for name in CLASSIFIER_IMPLEMENTATION_FILES[classifier_id]}
            rubric_hash = canonical_sha256(TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id])
            instructions = temporal_v2_observer_instructions(classifier_id)
            _require(isinstance(instructions, str) and instructions.strip(),
                     f"observer instructions are empty: {classifier_id}")
            spec_copy = seed / "spec.json"
            spec_copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(spec_source, spec_copy)
            instructions_path = seed / "observer-readme.txt"
            instructions_path.write_text(instructions, encoding="utf-8")
            pre_pixel = {
                "allowlist_status": "NOT_ALLOWLISTED",
                "classifier": {"id": classifier_id, "spec_sha256": spec_hash,
                               "implementation_sha256": implementation},
                "reader_binding": deepcopy(reader_binding),
                "observer_rubric_sha256": rubric_hash,
                "candidate_selection": deepcopy(TEMPORAL_CANDIDATE_SELECTION),
            }
            pre_path = seed / "pre-pixel-freeze.json"
            write_json(pre_path, pre_pixel)
            classifiers[classifier_id] = {
                "spec": reference(spec_copy, root),
                "implementation_sha256": implementation,
                "observer_rubric_sha256": rubric_hash,
                "observer_readme": reference(instructions_path, root),
                "pre_pixel_freeze": reference(pre_path, root),
            }
        document = {
            "schema_version": 1,
            "kind": CAMPAIGN_NAME,
            "source_git_sha": commit,
            "policy_id": DEFAULT_POLICY_ID,
            "candidate_selection": deepcopy(TEMPORAL_CANDIDATE_SELECTION),
            "bench_source_sha256": sha256(BENCH_PATH),
            "implementation_sha256": method,
            "reader_runtime": runtime,
            "reader_binding": reader_binding,
            "camera": camera,
            "base_manifest_sha256": sha256(base_path),
            "classifiers": classifiers,
        }
        write_json(root / "campaign.json", document)
        summary.update(campaign=str(destination.resolve()), source_git_sha=commit,
                       classifiers=list(targets),
                       base_qualification="VERIFIED", reserved_capture_pixel_files_opened=0)

    _atomic_directory(destination.resolve(), build)
    return summary


def _validate_capture(run_dir: Path, campaign: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    from encounter_qualification import _validate_temporal_v2_capture

    capture_path = run_dir / "qualification_capture.json"
    window_path = run_dir / "window_result.json"
    capture, window = read_json(capture_path), read_json(window_path)
    _validate_temporal_v2_capture(
        capture, window, capture_path, window_path,
        {"window_result": {"sha256": sha256(window_path)}},
        campaign["bench_source_sha256"])
    _require(capture.get("source_git_sha") == campaign.get("source_git_sha"),
             "qualification capture came from a different frozen commit")
    camera = window.get("camera", {})
    _require({"name": camera.get("camera_name"), "profile": camera.get("profile")} ==
             campaign.get("camera"), "qualification capture camera differs from freeze")
    return capture, window


def _candidate_records(temporal: dict[str, Any], classifier_id: str) -> list[dict[str, Any]]:
    admitted = []
    for record in temporal.get("classifications", []):
        if not isinstance(record, dict) or record.get("classifier_id") != classifier_id:
            continue
        target = deepcopy(record.get("video_frame_indices"))
        if classifier_id == "v1-unmute-stable-frequency-sweep-v2":
            full = deepcopy(record.get("full_field_run_indices"))
        elif classifier_id == ARROW_ACQUISITION_CLASSIFIER_ID:
            full = deepcopy(record.get("full_transition_indices"))
        elif classifier_id == FREQUENCY_CONTEXT_CLASSIFIER_ID:
            full = deepcopy(record.get("context_frame_indices"))
        elif classifier_id == SECONDARY_CONTEXT_CLASSIFIER_ID:
            full = deepcopy(record.get("full_context_indices"))
        elif classifier_id == SECONDARY_OPTICAL_CLASSIFIER_ID:
            left = record.get("left_support")
            right = record.get("right_support")
            full = ([point.get("video_frame_index") for point in [*left, *right]]
                    if isinstance(left, list) and isinstance(right, list) else [])
            if isinstance(target, list) and len(target) == 1 and len(full) == 4:
                full = [*full[:2], target[0], *full[2:]]
        else:
            full = deepcopy(target)
        admitted.append({"decision": "ADMITTED", "record": deepcopy(record),
                         "indices": target, "full_indices": full})
    field = FIELD_BY_CLASSIFIER[classifier_id]
    rejected = []
    for record in temporal.get("rejected_runs", []):
        _require(isinstance(record, dict)
                 and isinstance(record.get("classifier_id"), str)
                 and bool(record["classifier_id"]),
                 "temporal rejection lacks classifier provenance")
        if record.get("classifier_id") != classifier_id or record.get("field") != field:
            continue
        first = record.get("first", {}).get("video_frame_index")
        last = record.get("last", {}).get("video_frame_index")
        _require(type(first) is int and type(last) is int and 0 <= first <= last,
                 f"malformed rejected candidate: {classifier_id}")
        rejected.append({"decision": "REJECTED", "record": deepcopy(record),
                         "indices": list(range(first, last + 1)),
                         "full_indices": (deepcopy(record.get("full_transition_indices"))
                                          if classifier_id == ARROW_ACQUISITION_CLASSIFIER_ID
                                          else list(range(first, last + 1)))})
    candidates = [*admitted, *rejected]
    seen: set[tuple[int, ...]] = set()
    for item in candidates:
        indices = item["indices"]
        _require(isinstance(indices, list) and bool(indices)
                 and all(type(value) is int and value >= 0 for value in indices)
                 and all(right == left + 1 for left, right in zip(indices, indices[1:])),
                 f"nonconsecutive classifier candidate: {classifier_id}")
        key = tuple(indices)
        _require(key not in seen, f"duplicate classifier candidate: {classifier_id}")
        seen.add(key)
        full_indices = item["full_indices"]
        _require(isinstance(full_indices, list) and bool(full_indices)
                 and all(type(value) is int and value >= 0 for value in full_indices)
                 and all(right == left + 1 for left, right in zip(full_indices, full_indices[1:]))
                 and all(value in full_indices for value in indices),
                 f"invalid full classifier candidate: {classifier_id}")
    return candidates


def _observer_candidates(candidates, classifier_id, capture_id):
    rejected = [item["record"] for item in candidates if item["decision"] == "REJECTED"]
    selected = {canonical_sha256(record) for record in
                select_temporal_rejections(classifier_id, capture_id, rejected)}
    return [item for item in candidates if item["decision"] == "ADMITTED"
            or canonical_sha256(item["record"]) in selected]


def _raw_box(logical: tuple[int, int, int, int], registration: dict[str, Any],
             width: int, height: int) -> tuple[int, int, int, int]:
    x1, y1, x2, y2 = registration["landmark_bounds"]
    scale = ((x2 - x1 + 1) / 220 * width / 1280,
             (y2 - y1 + 1) / 79 * height / 720)
    anchor = (x1 * width / 960, y1 * height / 540)
    origin = (376 * 4 / 3, 192 * 4 / 3)
    values = [round(anchor[index % 2] + (value - origin[index % 2]) * scale[index % 2])
              for index, value in enumerate(logical)]
    left, top, right, bottom = values
    left, top = max(0, left), max(0, top)
    right, bottom = min(width, right), min(height, bottom)
    _require(left < right and top < bottom, "observer inset leaves source image")
    return left, top, right, bottom


def _logical_inset(classifier_id: str) -> tuple[int, int, int, int]:
    return {
        "v1-main-bar-adjacent-redraw-v2": (860, 185, 980, 440),
        "v1-muted-badge-rising-fill-v2": (480, 155, 710, 285),
        "v1-unmute-stable-frequency-sweep-v2": (425, 225, 845, 390),
        ARROW_CLASSIFIER_ID: (990, 190, 1165, 400),
        ARROW_ACQUISITION_CLASSIFIER_ID: (990, 190, 1165, 400),
        FREQUENCY_CONTEXT_CLASSIFIER_ID: (425, 225, 845, 390),
        SECONDARY_CONTEXT_CLASSIFIER_ID: (385, 360, 880, 460),
        SECONDARY_OPTICAL_CLASSIFIER_ID: (385, 360, 880, 460),
    }[classifier_id]


def _observer_clip_source_indices(
        target_indices: list[int], source_rows: dict[int, dict[str, int]],
        scope_indices: list[int] | None = None) -> list[int]:
    """Return the shared decision-independent observer window for a target run."""
    try:
        return temporal_observer_clip_source_indices(target_indices, source_rows, scope_indices)
    except Exception as exc:
        raise WorkflowError(str(exc)) from exc


def _build_clip(video: Path, destination: Path, classifier_id: str,
                target_indices: list[int], source_rows: dict[int, dict[str, int]],
                registration: dict[str, Any],
                width: int, height: int,
                scope_indices: list[int] | None = None) -> tuple[list[int], int]:
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    _require(ffmpeg is not None and ffprobe is not None, "ffmpeg and ffprobe are required")
    source_indices = _observer_clip_source_indices(target_indices, source_rows, scope_indices)
    first, last = source_indices[0], source_indices[-1]
    left, top, right, bottom = _raw_box(
        _logical_inset(classifier_id), registration, width, height)
    crop_width, crop_height = right - left, bottom - top
    graph = (
        f"trim=start_frame={first}:end_frame={last + 1},setpts=N/(25*TB),"
        "format=rgb24,split=2[full][detail];"
        f"[detail]crop={crop_width}:{crop_height}:{left}:{top},"
        "scale=440:680:force_original_aspect_ratio=decrease:flags=neighbor,"
        "pad=440:720:(ow-iw)/2:(oh-ih)/2:black[inset];"
        "[full][inset]hstack=inputs=2,format=rgb24"
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    command = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(video),
               "-filter_complex", graph, "-an", "-c:v", "png", "-pix_fmt", "rgb24",
               "-frames:v", str(len(source_indices)),
               "-movflags", "+faststart", str(destination)]
    try:
        subprocess.run(command, check=True)
        probe = json.loads(subprocess.check_output([
            ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
            "stream=codec_name,pix_fmt,width,height,nb_frames,r_frame_rate", "-of", "json",
            str(destination)
        ], text=True))["streams"][0]
    except (OSError, subprocess.CalledProcessError, KeyError, json.JSONDecodeError) as exc:
        raise WorkflowError(f"could not build observer clip: {destination.name}") from exc
    _require((probe.get("codec_name"), probe.get("pix_fmt"), probe.get("width"),
              probe.get("height"), probe.get("r_frame_rate")) ==
             ("png", "rgb24", width + 440, height, "25/1")
             and int(probe.get("nb_frames", -1)) == len(source_indices),
             f"observer clip probe differs: {destination.name}")
    return source_indices, destination.stat().st_size


def _opaque_ids(count: int) -> list[str]:
    alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
    values: set[str] = set()
    while len(values) < count:
        values.add("".join(secrets.choice(alphabet) for _ in range(12)))
    result = list(values)
    random.SystemRandom().shuffle(result)
    return result


def _link_or_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)
    _require(sha256(source) == sha256(destination),
             f"retained capture changed while copying: {source.name}")


def _retain_capture_tree(run_dir: Path, destination: Path) -> dict[str, Any]:
    from camera_artifacts import load_capture_manifest, verify_capture_files

    source_root = run_dir / "camera"
    source_manifest = source_root / "capture_manifest.json"
    # Reject duplicate keys before the shared camera validator parses the file.
    read_json(source_manifest)
    manifest = load_capture_manifest(source_manifest)
    verify_capture_files(source_root, manifest)
    destination.mkdir(parents=True)
    for entry in manifest["identity"]["artifacts"].values():
        _link_or_copy(source_root / entry["path"], destination / entry["path"])
    _link_or_copy(source_manifest, destination / source_manifest.name)
    retained = load_capture_manifest(destination / source_manifest.name)
    verify_capture_files(destination, retained)
    return retained


def _retain_replay_run(source: Path, destination: Path) -> Path:
    """Retain one self-contained replay input tree and a digest inventory."""
    source = source.resolve()
    destination.mkdir(parents=True)
    files: list[dict[str, Any]] = []
    for path in sorted(source.rglob("*")):
        _require(not path.is_symlink(), f"replay input contains a symbolic link: {path}")
        if not path.is_file():
            continue
        relative = path.relative_to(source)
        retained = destination / relative
        _link_or_copy(path, retained)
        files.append({"path": relative.as_posix(), "sha256": sha256(retained),
                      "size_bytes": retained.stat().st_size})
    _require(bool(files), "replay input tree is empty")
    inventory = destination.parent / "replay-input-manifest.json"
    write_json(inventory, {
        "schema_version": 1,
        "kind": "retained_qualification_replay_inputs",
        "files": files,
    })
    _verify_replay_inputs(destination, inventory)
    return inventory


def _verify_replay_inputs(replay: Path, inventory_path: Path) -> None:
    inventory = read_json(inventory_path)
    files = inventory.get("files") if isinstance(inventory, dict) else None
    _require(isinstance(inventory, dict)
             and inventory.get("schema_version") == 1
             and inventory.get("kind") == "retained_qualification_replay_inputs"
             and isinstance(files, list) and bool(files),
             "retained replay input manifest is malformed")
    retained_paths: set[str] = set()
    for entry in files:
        relative = entry.get("path") if isinstance(entry, dict) else None
        _require(isinstance(relative, str) and relative
                 and not Path(relative).is_absolute() and ".." not in Path(relative).parts
                 and relative not in retained_paths,
                 "retained replay input path is malformed")
        retained_paths.add(relative)
        path = (replay / relative).resolve()
        try:
            path.relative_to(replay.resolve())
        except ValueError as exc:
            raise WorkflowError("retained replay input leaves its root") from exc
        _require(path.is_file() and path.stat().st_size == entry.get("size_bytes")
                 and sha256(path) == entry.get("sha256"),
                 f"retained replay input changed: {relative}")
    actual = {path.relative_to(replay).as_posix()
              for path in replay.rglob("*") if path.is_file()}
    _require(actual == retained_paths, "retained replay input inventory is incomplete")


def _link_capture_view(retained_root: Path, destination: Path) -> dict[str, Any]:
    manifest = read_json(retained_root / "capture_manifest.json")
    destination.mkdir(parents=True)
    for entry in manifest["identity"]["artifacts"].values():
        _link_or_copy(retained_root / entry["path"], destination / entry["path"])
    _link_or_copy(retained_root / "capture_manifest.json", destination / "capture_manifest.json")
    return manifest


def _copy_frozen(root: Path, campaign: dict[str, Any], classifier_id: str,
                 name: str, destination: Path) -> None:
    source = resolve_reference(root, campaign["classifiers"][classifier_id][name],
                               f"frozen {classifier_id} {name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    _require(sha256(source) == sha256(destination), "frozen evidence changed while copying")


def _classifier_context(campaign: dict[str, Any], classifier_id: str,
                        window: dict[str, Any], analysis_selection_sha: str) -> dict[str, Any]:
    spec = read_json(SPEC_BY_CLASSIFIER[classifier_id])
    identity = spec["identity"]
    timing = window["camera"]["video_timing_verification_result"]
    context = {
        "capture_id": window["camera"]["capture_id"],
        "selection_manifest_sha256": analysis_selection_sha,
        "verified_maximum_source_interval_ns": timing["maximum_source_interval_ns"],
        "reader_method_version": campaign["reader_runtime"]["method_version"],
        "reader_sha256": campaign["implementation_sha256"]["encounter_reader.py"],
    }
    if "redraw_probe_method_version" in identity:
        context.update(
            redraw_probe_method_version=identity["redraw_probe_method_version"],
            redraw_probe_sha256=campaign["implementation_sha256"]["encounter_redraw_probe.py"])
    if "secondary_probe_method_version" in identity:
        context.update(
            secondary_probe_method_version=identity["secondary_probe_method_version"],
            secondary_probe_sha256=
                campaign["implementation_sha256"]["encounter_secondary_probe.py"])
    return context


def _prepare_classifier(stage: Path, campaign_root: Path, campaign: dict[str, Any],
                        classifier_id: str, candidates: list[dict[str, Any]],
                        analysis_selection: Path, capture_path: Path, window_path: Path,
                        window: dict[str, Any], data: dict[str, Any]) -> dict[str, Any]:
    from encounter_qualification import (
        TEMPORAL_SOURCE_HASH_FIELDS, TEMPORAL_V2_OBSERVER_RUBRICS,
        temporal_v2_observer_instructions,
    )

    classifier_root = stage / "classifiers" / classifier_id
    source_root = classifier_root / "source"
    observer = source_root / "observer_packet"
    restricted = source_root / "restricted"
    clips = observer / "clips"
    observer.mkdir(parents=True)
    restricted.mkdir(parents=True)
    retained_capture = source_root / "capture"
    capture_manifest = _link_capture_view(data["retained_capture"], retained_capture)
    capture_artifacts = capture_manifest["identity"]["artifacts"]
    _copy_frozen(campaign_root, campaign, classifier_id, "spec", classifier_root / "spec.json")
    _copy_frozen(campaign_root, campaign, classifier_id, "pre_pixel_freeze",
                 source_root / "pre-pixel-freeze.json")
    _copy_frozen(campaign_root, campaign, classifier_id, "observer_readme",
                 observer / "README.txt")
    shutil.copyfile(analysis_selection, source_root / "analysis-selection.json")
    shutil.copyfile(capture_path, source_root / "qualification-capture.json")
    shutil.copyfile(window_path, source_root / "window_result.json")
    _link_or_copy(data["analysis_result_path"], restricted / "analysis-result.json")

    admitted = [item["record"] for item in candidates if item["decision"] == "ADMITTED"]
    rejected = [item["record"] for item in candidates if item["decision"] == "REJECTED"]
    selected_candidates = _observer_candidates(candidates, classifier_id, window["camera"]["capture_id"])
    opaque = _opaque_ids(len(selected_candidates))
    paired = list(zip(selected_candidates, opaque))
    random.SystemRandom().shuffle(paired)
    manifest_items: list[dict[str, Any]] = []
    hidden_items: list[dict[str, Any]] = []
    source_rows = {
        index: {"source_frame_seq": row["frame_seq"],
                "capture_ns": row["host_capture_ns"]}
        for index, row in enumerate(data["rows"])
    }
    for candidate, opaque_id in paired:
        target = candidate["indices"]
        full_run = candidate["full_indices"]
        _require(full_run[-1] < data["timing"]["encoded_frame_count"],
                 f"classifier candidate leaves the encoded video: {classifier_id}")
        clip = clips / f"{opaque_id}.mov"
        scope_indices = ([*full_run, *[point["video_frame_index"]
                          for side in ("left_support", "right_support")
                          for point in candidate["record"][side]]]
                         if classifier_id == ARROW_ACQUISITION_CLASSIFIER_ID else None)
        clip_indices, size = _build_clip(
            data["video"], clip, classifier_id, target,
            source_rows, data["registration"],
            data["width"], data["height"], scope_indices)
        clip_hash = sha256(clip)
        target_clip_indices = [clip_indices.index(value) for value in target]
        full_run_clip_indices = [clip_indices.index(value) for value in full_run]
        inset_source_box = list(_raw_box(
            _logical_inset(classifier_id), data["registration"], data["width"], data["height"]))
        manifest_items.append({
            "opaque_id": opaque_id,
            "clip": f"clips/{opaque_id}.mov",
            "sha256": clip_hash,
            "size_bytes": size,
            "target_run_video_indices": target,
            "clip_source_video_indices": clip_indices,
            "target_run_clip_frame_indices": target_clip_indices,
            "inset_source_box": inset_source_box,
        })
        record = candidate["record"]
        if classifier_id == ARROW_ACQUISITION_CLASSIFIER_ID:
            manifest_items[-1].update({
                "full_run_clip_frame_indices": full_run_clip_indices,
                **{f"{side}_clip_frame_indices": [
                    clip_indices.index(point["video_frame_index"])
                    for point in record[side]]
                   for side in ("left_support", "right_support")},
            })
            _validate_temporal_v2_acquisition_scope_binding(
                manifest_items[-1], record, full_run)
        hidden_items.append({
            "opaque_id": opaque_id,
            "clip_sha256": clip_hash,
            "frozen_classifier_decision": candidate["decision"],
            "frozen_classifier_record": record,
            "frozen_classifier_record_sha256": canonical_sha256(record),
            "target_run_video_indices": target,
            "clip_source_video_indices": clip_indices,
            "target_run_clip_frame_indices": target_clip_indices,
            "full_run_video_indices": full_run,
            "full_run_clip_frame_indices": full_run_clip_indices,
        })

    spec_hash = campaign["classifiers"][classifier_id]["spec"]["sha256"]
    rubric_hash = campaign["classifiers"][classifier_id]["observer_rubric_sha256"]
    manifest = {
        "schema_version": 2,
        "kind": "blind_temporal_observer_manifest",
        "classifier_id": classifier_id,
        "classifier_spec_sha256": spec_hash,
        "observer_rubric_sha256": rubric_hash,
        "blind_protocol": deepcopy(BLIND_PROTOCOL),
        "item_count": len(manifest_items),
        "items": manifest_items,
    }
    manifest_path = observer / "manifest.json"
    write_json(manifest_path, manifest)
    fields = TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["literal_fields"]
    observations = {
        "instructions_sha256": sha256(observer / "README.txt"),
        "observer_rubric_sha256": rubric_hash,
        "blind_protocol": deepcopy(BLIND_PROTOCOL_TEMPLATE),
        "observations": [
            {"opaque_id": item["opaque_id"], **{name: None for name in fields}}
            for item in manifest_items
        ],
    }
    observations_path = observer / "observations.json"
    write_json(observations_path, observations)
    hidden = {
        "do_not_provide_to_observer": True,
        "classifier_id": classifier_id,
        "classifier_spec_sha256": spec_hash,
        "allowlist_status": "NOT_ALLOWLISTED_PENDING_BLIND_ADJUDICATION",
        "items": hidden_items,
    }
    hidden_path = restricted / "hidden-key.json"
    write_json(hidden_path, hidden)
    selection = temporal_selection_document(
        len(admitted), len(rejected), [item["opaque_id"] for item in manifest_items])
    selection_path = source_root / "selection.json"
    write_json(selection_path, selection)
    implementation = campaign["classifiers"][classifier_id]["implementation_sha256"]
    frozen_result = {
        "classifier_id": classifier_id,
        "classifier_spec_sha256": spec_hash,
        "classifier_implementation_sha256": implementation,
        "reader_binding": campaign["reader_binding"],
        "observer_rubric_sha256": rubric_hash,
        "classifier_context": _classifier_context(
            campaign, classifier_id, window, sha256(source_root / "analysis-selection.json")),
        "pre_pixel_freeze_sha256": sha256(source_root / "pre-pixel-freeze.json"),
        "selection_sha256": sha256(selection_path),
        "classifications": admitted,
        "rejected_runs": rejected,
        "errors": [],
    }
    frozen_path = restricted / "frozen-classifier-result.json"
    write_json(frozen_path, frozen_result)

    sources = {
        "completed_observations": observations_path,
        "frozen_classifier_result": frozen_path,
        "observer_manifest": manifest_path,
        "observer_readme": observer / "README.txt",
        "pre_pixel_freeze": source_root / "pre-pixel-freeze.json",
        "restricted_hidden_key": hidden_path,
        "selection": selection_path,
        "analysis_selection": source_root / "analysis-selection.json",
        "qualification_capture": source_root / "qualification-capture.json",
        "window_result": source_root / "window_result.json",
        "capture_manifest": retained_capture / "capture_manifest.json",
        "qualification_video": retained_capture / capture_artifacts["video"]["path"],
        "frame_timing": retained_capture / capture_artifacts["frame_timing"]["path"],
        "video_timing_verification": (
            retained_capture / capture_artifacts["video_timing_verification"]["path"]),
        "analysis_result": restricted / "analysis-result.json",
    }
    seal = {
        "classifier_id": classifier_id,
        "classifier_spec_sha256": spec_hash,
        "classifier_implementation_sha256": implementation,
        "reader_binding": campaign["reader_binding"],
        "observer_rubric_sha256": rubric_hash,
        "allowlist_status": "NOT_ALLOWLISTED",
    }
    for name, path in sources.items():
        if name != "completed_observations":
            seal[TEMPORAL_SOURCE_HASH_FIELDS[name]] = sha256(path)
    seal_path = source_root / "seal.json"
    write_json(seal_path, seal)
    sources["seal"] = seal_path
    _require(set(sources) == set(TEMPORAL_SOURCE_HASH_FIELDS),
             f"workflow source set differs from verifier: {classifier_id}")
    return {
        "classifier_id": classifier_id,
        "candidate_count": len(candidates),
        "admitted_count": len(admitted),
        "rejected_count": len(rejected),
        "observer_candidate_count": len(selected_candidates),
        "observer_packet": str(observer.relative_to(stage)),
        "classifier_root": str(classifier_root.relative_to(stage)),
    }


def _validate_capture_classifier_contexts(campaign: dict[str, Any],
                                           window: dict[str, Any]) -> None:
    """Reject incompatible metadata before reading any qualification pixels."""
    import encounter_arrow_acquisition as arrow_acquisition
    import encounter_arrow_transition as arrow
    import encounter_bar_transition as bar
    import encounter_frequency_context as frequency_context
    import encounter_mute_redraw_transition as mute
    import encounter_secondary_context as secondary_context
    import encounter_secondary_optical_bridge as secondary_optical

    classifiers = {
        arrow_acquisition.CLASSIFIER_ID: arrow_acquisition.classify_arrow_acquisition_runs,
        arrow.CLASSIFIER_ID: arrow.classify_arrow_runs,
        bar.CLASSIFIER_ID: bar.classify_main_bar_runs,
        frequency_context.CLASSIFIER_ID: frequency_context.classify_frequency_context_runs,
        mute.BADGE_CLASSIFIER_ID: mute.classify_mute_redraw_runs,
        secondary_context.CLASSIFIER_ID: secondary_context.classify_secondary_context_runs,
        secondary_optical.CLASSIFIER_ID:
            secondary_optical.classify_secondary_optical_bridge,
    }
    for classifier_id in _campaign_classifiers(campaign):
        # Selection does not exist yet. This digest checks only context shape;
        # empty samples cannot produce a classification or a retained claim.
        context = _classifier_context(campaign, classifier_id, window, "0" * 64)
        result = classifiers[classifier_id]([], [], context)
        _require(result.get("errors") == [],
                 f"capture classifier context rejected for {classifier_id}: "
                 + "; ".join(result.get("errors", [])))


def _candidate_coverage(result: dict[str, Any], classifier_ids: tuple[str, ...]) -> dict[str, Any]:
    """Stop impossible campaigns before clip work; counts never grant qualification."""
    from encounter_qualification import (
        _temporal_v2_frequency_context_band, _temporal_v2_secondary_optical_band,
    )

    temporal = result["temporal_classification"]
    coverage, blockers = {}, []
    for classifier_id in classifier_ids:
        admitted = [record for record in temporal["classifications"]
                    if record.get("classifier_id") == classifier_id]
        rejected = [record for record in temporal["rejected_runs"]
                    if record.get("classifier_id") == classifier_id]
        counts = {"admitted_candidates": len(admitted), "rejected_candidates": len(rejected)}
        coverage[classifier_id] = counts
        # These are upper bounds on blind true admits/rejects. Five candidates
        # cannot promise five correct labels; fewer than five cannot supply them.
        if len(admitted) < 5 or len(rejected) < 5:
            blockers.append(f"{classifier_id}: needs at least 5 admitted and 5 rejected "
                            f"candidates; has {len(admitted)} and {len(rejected)}")
        if classifier_id == FREQUENCY_CONTEXT_CLASSIFIER_ID:
            counts["branches"] = {}
            for branch in read_json(SPEC_BY_CLASSIFIER[classifier_id])["validation"]["branch_gates"]:
                positives = sum(record.get("branch") == branch for record in admitted)
                negatives = sum(record.get("branch") == branch for record in rejected)
                counts["branches"][branch] = {
                    "admitted_candidates": positives, "rejected_candidates": negatives}
                if positives < 5 or negatives < 5:
                    blockers.append(f"{classifier_id}/{branch}: needs at least 5 admitted "
                                    f"and 5 rejected candidates; has {positives} and {negatives}")
        band_reader = ({FREQUENCY_CONTEXT_CLASSIFIER_ID: _temporal_v2_frequency_context_band,
                        SECONDARY_OPTICAL_CLASSIFIER_ID: _temporal_v2_secondary_optical_band}
                       .get(classifier_id))
        if band_reader is not None:
            bands = {band_reader(record, result) for record in admitted}
            counts["admitted_candidate_bands"] = [band for band in ("X", "K", "Ka") if band in bands]
            missing = [band for band in ("X", "K", "Ka") if band not in bands]
            if missing:
                blockers.append(f"{classifier_id}: no admitted candidate for {', '.join(missing)}")
    return {"schema_version": 1, "kind": "temporal_candidate_coverage",
            "qualification_possible": not blockers,
            "meaning": "Candidate counts can rule out qualification, never establish it. "
                       "Every admitted and rejected candidate still requires blind observation.",
            "classifiers": coverage, "blockers": blockers}


def prepare(campaign_path: Path, run_dir: Path) -> dict[str, Any]:
    """Read the reserved capture once and make isolated, frozen-selection packets."""
    from encounter_check import analyze, load_run

    campaign_root, campaign = _campaign(campaign_path)
    _verify_frozen_source(campaign_root, campaign, require_unallowlisted=True)
    run_dir = run_dir.resolve()
    capture, window = _validate_capture(run_dir, campaign)
    _validate_capture_classifier_contexts(campaign, window)
    destination = campaign_root / "prepared"
    summary: dict[str, Any] = {}

    def build(stage: Path) -> None:
        retained_run = stage / "replay"
        replay_inventory = _retain_replay_run(run_dir, retained_run)
        retained_capture_doc, retained_window = _validate_capture(retained_run, campaign)
        _require(retained_capture_doc == capture and retained_window == window,
                 "retained replay boundary differs from reserved capture")
        analysis = stage / "restricted" / "analysis"
        analysis.mkdir(parents=True)
        runtime = reader_runtime(analysis / "reader-cache")
        _require(runtime == campaign.get("reader_runtime"),
                 "analysis reader runtime changed after freeze")
        result = analyze(
            retained_run, analysis, None, 2, inspect_transitions=True,
            reader_qualification=None, temporal_classifier_ids=_campaign_classifiers(campaign))
        _require(result.get("errors") == [],
                 "camera analysis contains errors: " + "; ".join(result.get("errors", [])))
        temporal = result.get("temporal_classification")
        _require(isinstance(temporal, dict) and temporal.get("errors") == [],
                 "temporal classifier execution contains errors: " + "; ".join(
                     temporal.get("errors", []) if isinstance(temporal, dict)
                     else ["classification is missing"]))
        _require(result.get("evidence", {}).get("reader") == campaign.get("reader_runtime"),
                 "analysis reader runtime differs from freeze")
        analysis_method = result.get("implementation_sha256")
        _require(isinstance(analysis_method, dict), "analysis implementation identity is missing")
        for name, digest in campaign["implementation_sha256"].items():
            _require(analysis_method.get(name) == digest,
                     f"analysis implementation differs from freeze: {name}")
        analysis_selection = analysis / "selection.json"
        _require(analysis_selection.is_file(), "analysis did not retain its pre-pixel selection")
        coverage = _candidate_coverage(result, _campaign_classifiers(campaign))
        write_json(analysis / "candidate-coverage.json", coverage)
        if not coverage["qualification_possible"]:
            summary.update(
                status="INSUFFICIENT_CANDIDATE_COVERAGE",
                retained_evidence=str(destination),
                analysis_result_sha256=sha256(analysis / "result.json"),
                candidate_coverage=coverage,
            )
            write_json(stage / "preparation-stopped.json", summary)
            return
        data = load_run(retained_run)
        retained_capture = retained_run / "camera"
        from camera_artifacts import load_capture_manifest
        retained_manifest = load_capture_manifest(retained_capture / "capture_manifest.json")
        _require(retained_manifest.get("capture_id") == window["camera"]["capture_id"],
                 "retained capture identity differs from qualification window")
        data["retained_capture"] = retained_capture
        data["video"] = retained_capture / retained_manifest["identity"]["artifacts"]["video"]["path"]
        data["analysis_result_path"] = analysis / "result.json"
        capture_path = retained_run / "qualification_capture.json"
        window_path = retained_run / "window_result.json"
        classifiers = []
        for classifier_id in _campaign_classifiers(campaign):
            candidates = _candidate_records(temporal, classifier_id)
            classifiers.append(_prepare_classifier(
                stage, campaign_root, campaign, classifier_id, candidates,
                analysis_selection, capture_path, window_path, window, data))
        prepared = {
            "schema_version": 1,
            "kind": PREPARED_NAME,
            "campaign_sha256": sha256(campaign_root / "campaign.json"),
            "source_git_sha": campaign["source_git_sha"],
            "capture_id": window["camera"]["capture_id"],
            "run_window_sha256": sha256(window_path),
            "analysis_selection_sha256": sha256(analysis_selection),
            "analysis_result_sha256": sha256(analysis / "result.json"),
            "replay_input_manifest_sha256": sha256(replay_inventory),
            "classifiers": classifiers,
        }
        write_json(stage / "prepared.json", prepared)
        summary.update(
            status="READY_FOR_BLIND_OBSERVATION",
            prepared=str(destination), capture_id=prepared["capture_id"],
            observer_packets=[str(destination / item["observer_packet"])
                              for item in classifiers],
            counts={item["classifier_id"]: {
                "candidates": item["candidate_count"], "admitted": item["admitted_count"],
                "rejected": item["rejected_count"],
                "observer_candidates": item["observer_candidate_count"]} for item in classifiers},
        )

    _atomic_directory(destination, build)
    return summary


def _rederive_analysis(prepared: Path, prepared_doc: dict[str, Any],
                       campaign: dict[str, Any]) -> dict[str, Any]:
    """Repeat the exact analyzer once before key access; normal bench runs do not repeat it."""
    from encounter_check import analyze

    replay = prepared / "replay"
    inventory = prepared / "replay-input-manifest.json"
    _require(sha256(inventory) == prepared_doc.get("replay_input_manifest_sha256"),
             "retained replay input manifest changed")
    _verify_replay_inputs(replay, inventory)
    output = Path(tempfile.mkdtemp(prefix=".qualification-reanalysis-", dir=prepared))
    try:
        _require(reader_runtime(output / "reader-cache") == campaign.get("reader_runtime"),
                 "qualification reanalysis reader runtime differs before pixel reading")
        result = analyze(
            replay, output, None, 2, inspect_transitions=True,
            reader_qualification=None, temporal_classifier_ids=_campaign_classifiers(campaign))
        _require(result.get("errors") == []
                 and result.get("temporal_classification", {}).get("errors") == [],
                 "qualification reanalysis contains errors: " + "; ".join(
                     result.get("errors", [])
                     + result.get("temporal_classification", {}).get("errors", [])))
        _require((output / "selection.json").is_file()
                 and sha256(output / "selection.json") ==
                     prepared_doc.get("analysis_selection_sha256"),
                 "qualification reanalysis selected different source frames")
        _require(result.get("evidence", {}).get("reader") == campaign.get("reader_runtime"),
                 "qualification reanalysis reader runtime differs")
        implementation = result.get("implementation_sha256")
        _require(isinstance(implementation, dict)
                 and all(implementation.get(name) == digest
                         for name, digest in campaign["implementation_sha256"].items()),
                 "qualification reanalysis implementation differs")
        return deepcopy(result)
    finally:
        shutil.rmtree(output, ignore_errors=True)


def _completed_observations(prepared: Path, prepared_doc: dict[str, Any],
                            campaign: dict[str, Any]) -> dict[str, dict[str, Any]]:
    from encounter_qualification import _temporal_v2_observer_result

    completed: dict[str, dict[str, Any]] = {}
    for item in prepared_doc["classifiers"]:
        classifier_id = item["classifier_id"]
        root = prepared / item["classifier_root"]
        manifest = read_json(root / "source/observer_packet/manifest.json")
        observations = read_json(root / "source/observer_packet/observations.json")
        values = observations.get("observations") if isinstance(observations, dict) else None
        _require(observations.get("blind_protocol") == BLIND_PROTOCOL,
                 f"blind protocol was not completed: {classifier_id}")
        _require(observations.get("instructions_sha256") ==
                 sha256(root / "source/observer_packet/README.txt")
                 and observations.get("observer_rubric_sha256") ==
                 campaign["classifiers"][classifier_id]["observer_rubric_sha256"],
                 f"observer instructions changed: {classifier_id}")
        _require(isinstance(values, list)
                 and [value.get("opaque_id") for value in values if isinstance(value, dict)] ==
                 [entry.get("opaque_id") for entry in manifest.get("items", [])],
                 f"observer did not complete every opaque item in order: {classifier_id}")
        for value in values:
            _temporal_v2_observer_result(classifier_id, value)
        completed[classifier_id] = {"manifest": manifest, "document": observations}
    return completed


def _validate_pre_key_sources(prepared: Path, prepared_doc: dict[str, Any],
                              campaign: dict[str, Any],
                              completed: dict[str, dict[str, Any]]) -> dict[str, dict[str, Path]]:
    """Validate all visible/sealed inputs without parsing either restricted result file."""
    from encounter_qualification import (
        TEMPORAL_SOURCE_HASH_FIELDS, _validate_temporal_v2_capture,
        _validate_temporal_v2_media,
    )
    from camera_timing import load_frame_sidecar

    result: dict[str, dict[str, Path]] = {}
    for item in prepared_doc.get("classifiers", []):
        classifier_id = item.get("classifier_id") if isinstance(item, dict) else None
        _require(classifier_id in _campaign_classifiers(campaign) and classifier_id not in result,
                 "prepared classifier set is malformed")
        classifier_root = prepared / item["classifier_root"]
        paths = _source_paths(classifier_root)
        _require(set(paths) == set(TEMPORAL_SOURCE_HASH_FIELDS),
                 f"pre-key source set is incomplete: {classifier_id}")
        _require(all(path.is_file() for path in paths.values()),
                 f"pre-key evidence file is missing: {classifier_id}")
        seal = read_json(paths["seal"])
        manifest = completed[classifier_id]["manifest"]
        observations = completed[classifier_id]["document"]
        selection = read_json(paths["selection"])
        analysis_selection = read_json(paths["analysis_selection"])
        capture = read_json(paths["qualification_capture"])
        window = read_json(paths["window_result"])
        timing = read_json(paths["video_timing_verification"])
        frame_records = load_frame_sidecar(paths["frame_timing"])
        source_rows = {
            index: {"source_frame_seq": row["frame_seq"],
                    "capture_ns": row["host_capture_ns"]}
            for index, row in enumerate(
                record for record in frame_records if record.get("status") == "written")
        }
        pre_pixel = read_json(paths["pre_pixel_freeze"])
        spec_hash = campaign["classifiers"][classifier_id]["spec"]["sha256"]
        implementation = campaign["classifiers"][classifier_id]["implementation_sha256"]
        rubric_hash = campaign["classifiers"][classifier_id]["observer_rubric_sha256"]
        _require(seal.get("classifier_id") == classifier_id
                 and seal.get("classifier_spec_sha256") == spec_hash
                 and seal.get("classifier_implementation_sha256") == implementation
                 and seal.get("reader_binding") == campaign["reader_binding"]
                 and seal.get("observer_rubric_sha256") == rubric_hash
                 and seal.get("allowlist_status") == "NOT_ALLOWLISTED",
                 f"pre-key seal differs: {classifier_id}")
        for name, path in paths.items():
            if name in {"completed_observations", "seal"}:
                continue
            _require(seal.get(TEMPORAL_SOURCE_HASH_FIELDS[name]) == sha256(path),
                     f"pre-key sealed evidence changed: {classifier_id} {name}")
        _require(pre_pixel == {
                     "allowlist_status": "NOT_ALLOWLISTED",
                     "classifier": {"id": classifier_id, "spec_sha256": spec_hash,
                                    "implementation_sha256": implementation},
                     "reader_binding": campaign["reader_binding"],
                     "observer_rubric_sha256": rubric_hash,
                     "candidate_selection": deepcopy(TEMPORAL_CANDIDATE_SELECTION),
                 }, f"pre-key freeze differs: {classifier_id}")
        _require(set(manifest) == {
                     "schema_version", "kind", "classifier_id", "classifier_spec_sha256",
                     "observer_rubric_sha256", "blind_protocol", "item_count", "items"}
                 and manifest.get("schema_version") == 2
                 and manifest.get("kind") == "blind_temporal_observer_manifest"
                 and manifest.get("classifier_id") == classifier_id
                 and manifest.get("classifier_spec_sha256") == spec_hash
                 and manifest.get("observer_rubric_sha256") == rubric_hash
                 and manifest.get("blind_protocol") == BLIND_PROTOCOL,
                 f"pre-key observer manifest differs: {classifier_id}")
        manifest_items = manifest.get("items")
        _require(isinstance(manifest_items, list)
                 and manifest.get("item_count") == len(manifest_items)
                 and all(isinstance(value, dict) for value in manifest_items),
                 f"pre-key observer items are malformed: {classifier_id}")
        ids = [value.get("opaque_id") for value in manifest_items]
        _require(all(isinstance(value, str) and value for value in ids)
                 and len(ids) == len(set(ids))
                 and selection == temporal_selection_document(
                     selection.get("candidate_counts", {}).get("admitted"),
                     selection.get("candidate_counts", {}).get("rejected"), ids)
                 and len(ids) == sum(selection["selected_counts"].values())
                 and [value.get("opaque_id") for value in observations["observations"]] == ids,
                 f"pre-key blind identities differ: {classifier_id}")
        for value in manifest_items:
            _require(set(value) == {
                         "opaque_id", "clip", "sha256", "size_bytes",
                         "target_run_video_indices", "clip_source_video_indices",
                         "target_run_clip_frame_indices", "inset_source_box"} |
                         (ACQUISITION_OBSERVER_SCOPE_FIELDS
                          if classifier_id == ARROW_ACQUISITION_CLASSIFIER_ID else set()),
                     f"pre-key observer mapping shape differs: {value.get('opaque_id')}")
            source = value["clip_source_video_indices"]
            target = value["target_run_video_indices"]
            scope = (temporal_acquisition_observer_scope(value)
                     if classifier_id == ARROW_ACQUISITION_CLASSIFIER_ID else None)
            expected_source = (_observer_clip_source_indices(
                                   target, source_rows,
                                   [index for values in scope.values() for index in values]
                                   if scope is not None else None)
                               if isinstance(target, list) and target else [])
            _require(isinstance(source, list) and bool(source)
                     and all(type(index) is int and index >= 0 for index in source)
                     and all(right == left + 1 for left, right in zip(source, source[1:]))
                     and isinstance(target, list) and bool(target)
                     and all(type(index) is int for index in target)
                     and all(right == left + 1 for left, right in zip(target, target[1:]))
                     and source == expected_source
                     and all(type(index) is int and index in source for index in target)
                     and value["target_run_clip_frame_indices"] ==
                         [source.index(index) for index in target]
                     and isinstance(value["clip"], str)
                     and Path(value["clip"]).parent == Path("clips")
                     and Path(value["clip"]).stem == value["opaque_id"]
                     and Path(value["clip"]).suffix == ".mov"
                     and isinstance(value["sha256"], str)
                     and _SHA256.fullmatch(value["sha256"]) is not None
                     and type(value["size_bytes"]) is int and value["size_bytes"] > 0
                     and isinstance(value["inset_source_box"], list)
                     and len(value["inset_source_box"]) == 4
                     and all(type(coordinate) is int
                             for coordinate in value["inset_source_box"]),
                     f"pre-key observer mapping differs: {value['opaque_id']}")
            if classifier_id == ARROW_ACQUISITION_CLASSIFIER_ID:
                observation = next(item for item in observations["observations"]
                                   if item["opaque_id"] == value["opaque_id"])
                temporal_acquisition_observer_scope(value, observation)
        _validate_temporal_v2_capture(
            capture, window, paths["qualification_capture"], paths["window_result"],
            {"window_result": {"sha256": sha256(paths["window_result"])}},
            campaign["bench_source_sha256"])
        _validate_temporal_v2_media(
            classifier_id, paths,
            {"video_timing_verification": timing}, window, analysis_selection, manifest_items)
        result[classifier_id] = paths
    _require(set(result) == set(_campaign_classifiers(campaign)),
             "prepared classifier set differs from workflow")
    return result


def _matrix_document(classifier_id: str, spec_hash: str, source_paths: dict[str, Path],
                     manifest: dict[str, Any], observations: dict[str, Any],
                     hidden: dict[str, Any]) -> dict[str, Any]:
    from encounter_qualification import (
        TEMPORAL_SOURCE_HASH_FIELDS, TEMPORAL_V2_INTEGRITY_CHECKS,
        _temporal_v2_frequency_context_band,
        _temporal_v2_secondary_optical_band,
        _temporal_v2_claim_matches_record, _temporal_v2_observer_ground_truth,
    )

    comparisons: list[dict[str, Any]] = []
    clip_checks: list[dict[str, Any]] = []
    outcome_ids = {name: [] for name in
                   ("true_admit", "false_admit", "true_reject", "false_reject",
                    "abstain")}
    ground_truth_counts: Counter[str] = Counter()
    frequency_branches = tuple(read_json(SPEC_BY_CLASSIFIER[FREQUENCY_CONTEXT_CLASSIFIER_ID])[
        "validation"]["branch_gates"])
    branch_outcomes = ({branch: Counter() for branch in frequency_branches}
                       if classifier_id == FREQUENCY_CONTEXT_CLASSIFIER_ID else {})
    true_admit_bands: set[str] = set()
    analysis_result = (read_json(source_paths["analysis_result"])
                       if (branch_outcomes
                           or classifier_id == SECONDARY_OPTICAL_CLASSIFIER_ID) else {})
    by_hidden = {item["opaque_id"]: item for item in hidden["items"]}
    by_observation = {item["opaque_id"]: item for item in observations["observations"]}
    for manifest_item in manifest["items"]:
        opaque_id = manifest_item["opaque_id"]
        _require(opaque_id in by_hidden and opaque_id in by_observation,
                 f"blind item identities differ: {classifier_id}")
        hidden_item, observation = by_hidden[opaque_id], by_observation[opaque_id]
        if classifier_id == ARROW_ACQUISITION_CLASSIFIER_ID:
            temporal_acquisition_observer_scope(manifest_item, observation)
            _validate_temporal_v2_acquisition_scope_binding(
                manifest_item, hidden_item["frozen_classifier_record"],
                hidden_item["full_run_video_indices"])
        literal, ground_truth = _temporal_v2_observer_ground_truth(
            classifier_id, observation)
        eligible = ground_truth == "ELIGIBLE"
        ground_truth_counts[ground_truth] += 1
        decision, record = (hidden_item["frozen_classifier_decision"],
                            hidden_item["frozen_classifier_record"])
        claim_matches = (_temporal_v2_claim_matches_record(
            classifier_id, spec_hash, literal, record) if decision == "ADMITTED" else None)
        if decision == "ADMITTED":
            outcome = "TRUE_ADMIT" if eligible and claim_matches else "FALSE_ADMIT"
        elif eligible:
            outcome = "FALSE_REJECT"
        elif ground_truth == "DEFINITE_NEGATIVE":
            outcome = "TRUE_REJECT"
        else:
            outcome = "ABSTAIN"
        outcome_ids[outcome.casefold()].append(opaque_id)
        if (classifier_id == SECONDARY_OPTICAL_CLASSIFIER_ID
                and decision == "ADMITTED"):
            band = _temporal_v2_secondary_optical_band(record, analysis_result)
            if outcome == "TRUE_ADMIT":
                true_admit_bands.add(band)
        if branch_outcomes:
            branch = record.get("branch")
            _require(branch in branch_outcomes,
                     f"frozen frequency-context branch is invalid: {opaque_id}")
            branch_outcomes[branch][outcome.casefold()] += 1
            if outcome == "TRUE_ADMIT":
                true_admit_bands.add(
                    _temporal_v2_frequency_context_band(record, analysis_result))
        record_hash = canonical_sha256(record)
        comparisons.append({
            "opaque_id": opaque_id,
            "observer_literal": literal,
            "observer_strict_visual_eligible_for_admission": eligible,
            "observer_ground_truth": ground_truth,
            "observer_claim_matches_frozen_record": claim_matches,
            "frozen_classifier_decision": decision,
            "frozen_rejection_code": None if decision == "ADMITTED" else record.get("code"),
            "frozen_classifier_record_sha256": record_hash,
            "source_video_frame_indices": hidden_item["target_run_video_indices"],
            "comparison_outcome": outcome,
        })
        clip = source_paths["observer_manifest"].parent / manifest_item["clip"]
        actual_hash, actual_size = sha256(clip), clip.stat().st_size
        target = hidden_item["target_run_video_indices"]
        clip_source = hidden_item.get("clip_source_video_indices", [])
        target_clip = hidden_item.get("target_run_clip_frame_indices", [])
        full_run = hidden_item.get("full_run_video_indices", [])
        full_run_clip = hidden_item.get("full_run_clip_frame_indices", [])
        path_matches = (Path(manifest_item["clip"]).parent == Path("clips")
                        and Path(manifest_item["clip"]).stem == opaque_id)
        source_matches = (isinstance(clip_source, list) and bool(clip_source)
                          and all(right == left + 1
                                  for left, right in zip(clip_source, clip_source[1:])))
        inside = source_matches and all(value in clip_source for value in target)
        expected_target_clip = ([clip_source.index(value) for value in target]
                                if inside else [])
        full_inside = source_matches and all(value in clip_source for value in full_run)
        expected_full_run_clip = ([clip_source.index(value) for value in full_run]
                                  if full_inside else [])
        _require(actual_hash == manifest_item["sha256"] == hidden_item["clip_sha256"]
                 and actual_size == manifest_item["size_bytes"]
                 and path_matches and source_matches and inside
                 and manifest_item.get("clip_source_video_indices") == clip_source
                 and manifest_item.get("target_run_clip_frame_indices") == target_clip
                 and target_clip == expected_target_clip
                 and full_run_clip == expected_full_run_clip,
                 f"observer clip mapping differs: {opaque_id}")
        clip_checks.append({
            "opaque_id": opaque_id,
            "actual_sha256": actual_hash,
            "hidden_key_sha256": hidden_item["clip_sha256"],
            "sealed_sha256": manifest_item["sha256"],
            "actual_size_bytes": actual_size,
            "sealed_size_bytes": manifest_item["size_bytes"],
            "target_run_video_indices": target,
            "clip_source_video_indices": clip_source,
            "target_run_clip_frame_indices": target_clip,
            "path_matches_id": path_matches,
            "source_frame_indices_match_target_run": source_matches,
            "target_run_inside_clip": inside,
        })

    matrix = {name: len(values) for name, values in outcome_ids.items()}
    matrix["total"] = len(comparisons)
    admissions = matrix["true_admit"] + matrix["false_admit"]
    rejections = matrix["true_reject"] + matrix["false_reject"] + matrix["abstain"]
    positives = matrix["true_admit"] + matrix["false_reject"]
    definite_negatives = ground_truth_counts["DEFINITE_NEGATIVE"]
    indeterminate = ground_truth_counts["INDETERMINATE"]
    negatives_or_uncertain = definite_negatives + indeterminate
    scored = matrix["total"] - matrix["abstain"]
    branch_fields: dict[str, Any] = {}
    branch_allowed = True
    if branch_outcomes:
        branch_matrices = {
            branch: {
                **{name: counts[name] for name in outcome_ids},
                "total": sum(counts.values()),
            }
            for branch, counts in branch_outcomes.items()
        }
        branch_minima = {
            branch: {
                "required_true_admit_minimum": 5,
                "required_true_reject_minimum": 5,
                "required_false_admits": 0,
                "observed_true_admit": values["true_admit"],
                "observed_true_reject": values["true_reject"],
                "observed_false_admit": values["false_admit"],
                "true_admit_minimum_met": values["true_admit"] >= 5,
                "true_reject_minimum_met": values["true_reject"] >= 5,
                "false_admit_requirement_met": values["false_admit"] == 0,
            }
            for branch, values in branch_matrices.items()
        }
        required_bands = ["X", "K", "Ka"]
        band_coverage = [band for band in required_bands if band in true_admit_bands]
        branch_allowed = (
            all(values["false_admit"] == 0
                and values["true_admit"] >= 5 and values["true_reject"] >= 5
                for values in branch_matrices.values())
            and band_coverage == required_bands)
        branch_fields = {
            "branch_confusion_matrices": branch_matrices,
            "branch_numerical_minima": branch_minima,
            "true_admit_band_coverage": band_coverage,
            "required_band_coverage_met": band_coverage == required_bands,
        }
    coverage_allowed = True
    if classifier_id == SECONDARY_OPTICAL_CLASSIFIER_ID:
        required_bands = ["X", "K", "Ka"]
        band_coverage = [band for band in required_bands if band in true_admit_bands]
        coverage_allowed = band_coverage == required_bands
        branch_fields = {
            "true_admit_band_coverage": band_coverage,
            "required_band_coverage_met": coverage_allowed,
        }
    allowed = (matrix["false_admit"] == 0 and matrix["true_admit"] >= 5
               and matrix["true_reject"] >= 5 and branch_allowed and coverage_allowed)
    return {
        "schema_version": 2,
        "classifier_id": classifier_id,
        "classifier_spec_sha256": spec_hash,
        "source_artifacts": {
            TEMPORAL_SOURCE_HASH_FIELDS[name]: sha256(path)
            for name, path in source_paths.items()
        },
        "comparisons": comparisons,
        "integrity": {
            "checks": {name: True for name in TEMPORAL_V2_INTEGRITY_CHECKS},
            "clip_checks": clip_checks,
        },
        "confusion_matrix": matrix,
        **{f"{name}_ids": values for name, values in outcome_ids.items()},
        **branch_fields,
        "denominators": {
            "classifier_admissions": admissions,
            "classifier_rejections": rejections,
            "observer_visual_positives": positives,
            "observer_definite_negatives": definite_negatives,
            "observer_indeterminate": indeterminate,
            "observer_visual_negatives_or_uncertain": negatives_or_uncertain,
            "scored_observations": scored,
            "false_admit": {"count": matrix["false_admit"],
                            "denominator_classifier_admissions": admissions,
                            "rate": matrix["false_admit"] / admissions if admissions else None},
            "false_reject": {"count": matrix["false_reject"],
                             "denominator_observer_visual_positives": positives,
                             "rate": matrix["false_reject"] / positives if positives else None},
        },
        "rates": {
            "accuracy": ((matrix["true_admit"] + matrix["true_reject"]) / scored
                         if scored else None),
            "precision": matrix["true_admit"] / admissions if admissions else None,
            "recall": matrix["true_admit"] / positives if positives else None,
            "specificity": (matrix["true_reject"] / definite_negatives
                            if definite_negatives else None),
        },
        "numerical_minima": {
            "required_true_admit_minimum": 5,
            "required_true_reject_minimum": 5,
            "observed_true_admit": matrix["true_admit"],
            "observed_true_reject": matrix["true_reject"],
            "true_admit_minimum_met": matrix["true_admit"] >= 5,
            "true_reject_minimum_met": matrix["true_reject"] >= 5,
        },
        "integrity_pass": allowed,
        "allowlist_decision": {"allowlist_exact_classifier": allowed},
    }


def _source_paths(classifier_root: Path) -> dict[str, Path]:
    capture_root = classifier_root / "source/capture"
    capture_manifest = read_json(capture_root / "capture_manifest.json")
    artifacts = capture_manifest.get("identity", {}).get("artifacts", {})
    _require(all(isinstance(artifacts.get(name), dict)
                 and isinstance(artifacts[name].get("path"), str)
                 for name in ("video", "frame_timing", "video_timing_verification")),
             "retained capture manifest is incomplete")
    return {
        "completed_observations": classifier_root / "source/observer_packet/observations.json",
        "frozen_classifier_result": classifier_root / "source/restricted/frozen-classifier-result.json",
        "observer_manifest": classifier_root / "source/observer_packet/manifest.json",
        "observer_readme": classifier_root / "source/observer_packet/README.txt",
        "pre_pixel_freeze": classifier_root / "source/pre-pixel-freeze.json",
        "restricted_hidden_key": classifier_root / "source/restricted/hidden-key.json",
        "seal": classifier_root / "source/seal.json",
        "selection": classifier_root / "source/selection.json",
        "analysis_selection": classifier_root / "source/analysis-selection.json",
        "qualification_capture": classifier_root / "source/qualification-capture.json",
        "window_result": classifier_root / "source/window_result.json",
        "capture_manifest": capture_root / "capture_manifest.json",
        "qualification_video": capture_root / artifacts["video"]["path"],
        "frame_timing": capture_root / artifacts["frame_timing"]["path"],
        "video_timing_verification": (
            capture_root / artifacts["video_timing_verification"]["path"]),
        "analysis_result": classifier_root / "source/restricted/analysis-result.json",
    }


def _clone_file_tree(source: Path, destination: Path) -> None:
    for path in sorted(source.rglob("*")):
        _require(not path.is_symlink(), f"qualification evidence contains a symbolic link: {path}")
        if path.is_file():
            _link_or_copy(path, destination / path.relative_to(source))


def _publish_comparison_tree(prepared: Path, prepared_doc: dict[str, Any],
                             comparisons: dict[str, dict[str, Any]]) -> dict[str, dict[str, Any]]:
    destination = prepared / "qualified"
    _require(not destination.exists(),
             "blind campaign already contains post-key results; a fresh campaign is required")

    def build(stage: Path) -> None:
        for item in prepared_doc["classifiers"]:
            classifier_id = item["classifier_id"]
            source_root = prepared / item["classifier_root"]
            bundle = stage / classifier_id
            _link_or_copy(source_root / "spec.json", bundle / "spec.json")
            _clone_file_tree(source_root / "source", bundle / "source")
            write_json(bundle / "comparison.json", comparisons[classifier_id])

    _atomic_directory(destination, build)
    entries: dict[str, dict[str, Any]] = {}
    for classifier_id in comparisons:
        bundle = destination / classifier_id
        sources = _source_paths(bundle)
        entries[classifier_id] = {
            "classifier_spec_sha256": comparisons[classifier_id]["classifier_spec_sha256"],
            "spec": bundle / "spec.json",
            "validation": bundle / "comparison.json",
            "source_artifacts": {
                name: reference(path, bundle) for name, path in sources.items()},
        }
    return entries


def _consume_campaign(prepared: Path, prepared_doc: dict[str, Any],
                      sources: dict[str, dict[str, Path]], base_manifest: Path,
                      prospective_policy: bytes) -> Path:
    marker = prepared / CONSUMED_NAME
    payload = {
        "schema_version": 1,
        "kind": "blind_temporal_qualification_consumed",
        "campaign_sha256": prepared_doc["campaign_sha256"],
        "capture_id": prepared_doc["capture_id"],
        "base_manifest_sha256": sha256(base_manifest),
        "prospective_policy_sha256": hashlib.sha256(prospective_policy).hexdigest(),
        "restricted_hashes": {
            classifier_id: {
                "analysis_result_sha256": sha256(paths["analysis_result"]),
                "frozen_classifier_result_sha256": sha256(paths["frozen_classifier_result"]),
                "restricted_hidden_key_sha256": sha256(paths["restricted_hidden_key"]),
            }
            for classifier_id, paths in sources.items()
        },
        "state": "CONSUMED_BEFORE_RESTRICTED_KEY_ACCESS",
    }
    _write_exclusive_json(marker, payload)
    return marker


def _resolve_base_evidence(base_path: Path) -> tuple[dict[str, Path], dict[str, Any]]:
    """Resolve byte-bound static and carried temporal entries without duplication.

    The complete qualification verifier owns their reader and classifier proof.
    A static-only base is valid when qualifying the first temporal classifier.
    """
    base_root = base_path.parent.resolve()
    base = read_json(base_path)
    _require(base.get("kind") == "encounter_reader_qualification", "base manifest is invalid")
    top: dict[str, Path] = {}
    for name in ("field_validation", "visible_secondary_validation", "fault_controls"):
        top[name] = resolve_reference(base_root, base.get(name), f"base {name}")
    temporal = base.get("temporal_classifiers")
    _require(isinstance(temporal, dict), "base temporal qualification map is malformed")
    entries = {}
    for classifier_id, entry in temporal.items():
        _require(isinstance(entry, dict), "base temporal qualification entry is malformed")
        validation_source = resolve_reference(
            base_root, entry.get("validation"), f"base {classifier_id} validation")
        source_root = validation_source.parent
        spec_source = resolve_reference(base_root, entry.get("spec"), f"base {classifier_id} spec")
        try:
            spec_source.relative_to(source_root)
        except ValueError as exc:
            raise WorkflowError("base temporal spec is outside its validation tree") from exc
        sources = entry.get("source_artifacts")
        _require(isinstance(sources, dict), "base temporal sources are malformed")
        entries[classifier_id] = {
            "classifier_spec_sha256": entry["classifier_spec_sha256"],
            "implementation_sha256": deepcopy(entry.get("implementation_sha256")),
            "spec": spec_source,
            "validation": validation_source,
            "source_artifacts": {
                name: reference(resolve_reference(source_root, value, f"base {classifier_id} {name}"),
                                source_root)
                for name, value in sources.items()},
        }
    return top, entries


def validate_base_evidence(base_path: Path, method: dict[str, str],
                           runtime: dict[str, Any], camera: dict[str, Any],
                           policy: dict[str, Any]) -> None:
    """Rerun every retained base proof under this method before using it."""
    from encounter_qualification import verify_qualification

    _resolve_base_evidence(base_path)
    base = read_json(base_path)
    _require(base.get("reader", {}).get("runtime") == runtime,
             "base static qualification uses a different reader runtime")
    _require(base.get("camera") == camera,
             "base static qualification uses a different camera")
    # Retain every original label, comparison, source reference and runtime.
    # The verifier re-reads source pixels and rederives the proof under the
    # current method. This permits orchestration changes, not inherited claims.
    candidate = deepcopy(base)
    candidate["reader"]["implementation_sha256"] = static_method_hashes(method)
    with tempfile.NamedTemporaryFile(
            prefix=".base-qualification-", suffix=".json", dir=base_path.parent,
            delete=False) as stream:
        candidate_path = Path(stream.name)
        stream.write(json_bytes(candidate))
    try:
        verified = verify_qualification(
            candidate_path, implementation_sha256=method, reader_runtime=runtime,
            camera_name=camera["name"], camera_profile=camera["profile"],
            policy=policy, bench_source_sha256=sha256(BENCH_PATH))
    finally:
        candidate_path.unlink(missing_ok=True)
    _require(verified.get("status") == "QUALIFIED",
             "retained base qualification rejected: " + "; ".join(verified.get("errors", [])))


def _copy_static_reference(source_root: Path, destination_root: Path, value: Any,
                           name: str, copied: dict[Path, str]) -> None:
    """Copy one hash-bound static input while preserving its relative path."""
    source = resolve_reference(source_root, value, name)
    _require(not source.is_symlink(), f"{name} is a symbolic link")
    relative = Path(value["path"])
    destination = destination_root / relative
    expected = value["sha256"]
    if destination in copied:
        _require(copied[destination] == expected,
                 f"static evidence path is reused with different bytes: {relative}")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    _require(sha256(destination) == expected,
             f"static evidence changed while copying: {relative}")
    copied[destination] = expected


def _copy_static_inputs(source_root: Path, destination_root: Path,
                        document: dict[str, Any], kind: str) -> None:
    copied: dict[Path, str] = {}
    sources = document.get("source_artifacts")
    if kind == "fault" and sources is None:
        sources = {}
    _require(isinstance(sources, dict), f"source {kind} evidence has no source artifacts")
    for name, value in sources.items():
        _copy_static_reference(source_root, destination_root, value,
                               f"source {kind} {name}", copied)
        if kind == "field" and name == "primary_frequency_reference":
            from encounter_primary_frequency_reference import copy_reference
            copy_reference(resolve_reference(source_root, value, name), destination_root / value["path"])
    collection_name = {"field": "frames", "secondary": "items", "fault": "cases"}[kind]
    records = document.get(collection_name)
    _require(isinstance(records, list) and bool(records),
             f"source {kind} evidence has no retained images")
    for index, record in enumerate(records):
        _require(isinstance(record, dict), f"source {kind} record is malformed")
        _copy_static_reference(source_root, destination_root, record.get("image"),
                               f"source {kind} image {index}", copied)


def _bind_current_static_method(document: dict[str, Any], runtime: dict[str, Any],
                                method: dict[str, str], camera: dict[str, Any]) -> None:
    from encounter_qualification import CORE_READER_FILES

    binding = document.get("method")
    _require(isinstance(binding, dict), "static evidence has no method binding")
    _require(all(name in method for name in CORE_READER_FILES),
             "current static reader implementation is incomplete")
    binding["method_version"] = runtime.get("method_version")
    binding["files"] = {name: method[name] for name in CORE_READER_FILES}
    document["reader"] = deepcopy(runtime)
    document["camera"] = deepcopy(camera)


def _reanalyze_field_document(source: Path, destination: Path,
                              runtime: dict[str, Any], method: dict[str, str],
                              camera: dict[str, Any], primary_frequency_reference: Path | None = None) -> dict[str, Any]:
    from encounter_qualification import FIELDS, _derived_field_status, _observe_image

    document = read_json(source)
    _require(isinstance(document, dict), "source field validation is malformed")
    _copy_static_inputs(source.parent, destination.parent, document, "field")
    selection = read_json(resolve_reference(
        destination.parent, document["source_artifacts"].get("selection"),
        "copied field selection"))
    registration = selection.get("registration") if isinstance(selection, dict) else None
    _require(isinstance(registration, dict) and registration.get("result") == "PASS",
             "source field validation registration is unavailable")
    adjudication = None
    if primary_frequency_reference is not None:
        from encounter_primary_frequency_reference import copy_reference
        retained = destination.parent / "source/primary-frequency/reference.json"
        copy_reference(primary_frequency_reference, retained)
        document["source_artifacts"]["primary_frequency_reference"] = reference(retained, destination.parent)
    if "primary_frequency_reference" in document["source_artifacts"]:
        from encounter_primary_frequency_reference import reference_reread_binding, validate_reference
        try:
            frequency_reference = resolve_reference(
                destination.parent, document["source_artifacts"]["primary_frequency_reference"], "frequency reference")
            binding = reference_reread_binding(frequency_reference, method)
            if binding is None:
                document.pop("primary_frequency_reader_reanalysis", None)
            else:
                document["primary_frequency_reader_reanalysis"] = binding
            adjudication = validate_reference(
                frequency_reference,
                resolve_reference(destination.parent, document["source_artifacts"]["blind_manifest"], "original blind manifest"),
                resolve_reference(destination.parent, document["source_artifacts"]["blind_observations"], "original blind labels"),
                method, registration, _observe_image, reader_reanalysis=binding)
        except (OSError, ValueError, KeyError, TypeError) as exc:
            raise WorkflowError(str(exc)) from exc
        document["primary_frequency_adjudication"] = adjudication["summary"]
    totals: Counter[str] = Counter()
    fields = {field: Counter() for field in FIELDS}
    for frame in document["frames"]:
        image = resolve_reference(destination.parent, frame.get("image"),
                                  f"field image {frame.get('frame_id')}")
        observed = _observe_image(image, registration)
        checks = frame.get("checks")
        _require(isinstance(checks, list), "source field checks are malformed")
        by_field = {check.get("field"): check for check in checks if isinstance(check, dict)}
        _require(len(by_field) == len(checks) and set(by_field) == set(FIELDS),
                 "source field frame does not check every field")
        for field in FIELDS:
            reading = observed.get(field)
            _require(isinstance(reading, dict), f"current reader omitted {field}")
            check = by_field[field]
            if field == "primary_frequency" and adjudication and frame["frame_id"] in adjudication["overrides"]:
                check.setdefault("original_reference", deepcopy(check.get("reference")))
                check["reference"] = deepcopy(adjudication["overrides"][frame["frame_id"]])
            check["observed"] = deepcopy(reading)
            check["status"] = _derived_field_status(field, reading, check.get("reference"))
            totals[check["status"]] += 1
            fields[field][check["status"]] += 1
    document["counts"] = dict(totals)
    document["fields"] = {field: dict(fields[field]) for field in FIELDS}
    document["unique_original_frames"] = len(document["frames"])
    document["required_field_labels"] = len(document["frames"]) * len(FIELDS)
    document.pop("method_freeze_sha256", None)
    document.pop("source_validation_sha256", None)
    _bind_current_static_method(document, runtime, method, camera)
    write_json(destination, document)
    return document


def _reanalyze_secondary_document(source: Path, destination: Path,
                                  runtime: dict[str, Any], method: dict[str, str],
                                  camera: dict[str, Any]) -> dict[str, Any]:
    from encounter_qualification import (
        CORE_READER_FILES, _blind_secondary_reference, _derived_field_status, _observe_image)

    document = read_json(source)
    _require(isinstance(document, dict), "source visible-secondary validation is malformed")
    source_reader = deepcopy(document.get("reader"))
    source_method = deepcopy(document.get("method"))
    _copy_static_inputs(source.parent, destination.parent, document, "secondary")
    registration = document.get("registration")
    _require(isinstance(registration, dict) and registration.get("result") == "PASS",
             "source visible-secondary registration is unavailable")
    counts: Counter[str] = Counter()
    for item in document["items"]:
        image = resolve_reference(destination.parent, item.get("image"),
                                  f"secondary image {item.get('source_image')}")
        observed = _observe_image(image, registration).get("secondary")
        _require(isinstance(observed, dict), "current reader omitted secondary")
        primary = _blind_secondary_reference(item.get("blind_label"))
        item["observed"] = deepcopy(observed)
        item["primary_reference"] = primary
        item["status"] = _derived_field_status("secondary", observed, item.get("reference"))
        counts[item["status"]] += 1
    document["counts"] = dict(counts)
    document["unique_original_frames"] = len(document["items"])
    _bind_current_static_method(document, runtime, method, camera)
    sealed_reference = document["source_artifacts"].get("sealed_key")
    sealed = read_json(resolve_reference(
        destination.parent, sealed_reference, "copied visible-secondary sealed key"))
    source_implementation = (sealed.get("implementation")
                             if isinstance(sealed, dict) else None)
    source_setup = (source_implementation.get("reader_setup")
                    if isinstance(source_implementation, dict) else None)
    _require(isinstance(source_setup, dict)
             and isinstance(source_implementation.get("reader_sha256"), str),
             "source visible-secondary reader identity is unavailable")
    current_core_files = {name: method.get(name) for name in CORE_READER_FILES}
    same_reader = (
        source_reader == runtime
        and isinstance(source_method, dict)
        and source_method.get("method_version") == runtime.get("method_version")
        and source_method.get("files") == current_core_files
        and source_setup == runtime
        and source_implementation["reader_sha256"] == method.get("encounter_reader.py")
    )
    if same_reader:
        # A qualification-logic change can require a fresh verification even
        # when the exact pixel reader is unchanged. In that case the verifier
        # requires each regenerated observation to match the sealed original;
        # describing it as a different-reader reread would be false.
        document.pop("reader_reanalysis", None)
    else:
        document["reader_reanalysis"] = {
            "kind": "complete_exact_reader_reread",
            "source_sealed_key_sha256": sealed_reference["sha256"],
            "source_method_version": source_setup["method_version"],
            "source_reader_sha256": source_implementation["reader_sha256"],
            "current_method_version": runtime.get("method_version"),
            "current_reader_sha256": method.get("encounter_reader.py"),
            "complete_source_set_reread": True,
        }
    write_json(destination, document)
    return document


def _fault_control_demonstrated(case: dict[str, Any], comparison: dict[str, Any]) -> bool:
    from encounter_qualification import REQUIRED_FAULT_CONTROLS, FIELDS

    desired, required_fields, required_unresolved = REQUIRED_FAULT_CONTROLS[case["name"]]
    checks = comparison.get("checks")
    if not isinstance(checks, dict) or set(checks) != set(FIELDS):
        return False
    passed = comparison.get("status") == desired
    passed = passed and all(checks[field].get("status") == "DIFFERENCE"
                            for field in required_fields)
    passed = passed and all(checks[field].get("status") == "UNRESOLVED"
                            for field in required_unresolved)
    if desired == "MATCH":
        passed = passed and all(checks[field].get("status") == "MATCH" for field in FIELDS)
    if desired == "INCONCLUSIVE":
        passed = passed and not any(
            checks[field].get("status") == "DIFFERENCE" for field in FIELDS)
    return passed


def _reanalyze_fault_document(source: Path, destination: Path,
                              runtime: dict[str, Any], method: dict[str, str],
                              camera: dict[str, Any]) -> dict[str, Any]:
    from encounter_expectation import compare_sample
    from encounter_qualification import FIELDS, REQUIRED_FAULT_CONTROLS, _observe_image

    document = read_json(source)
    _require(isinstance(document, dict), "source fault controls are malformed")
    _copy_static_inputs(source.parent, destination.parent, document, "fault")
    registration = document.get("registration")
    _require(isinstance(registration, dict) and registration.get("result") == "PASS",
             "source fault-control registration is unavailable")
    cases = document.get("cases")
    _require(isinstance(cases, list), "source fault-control cases are malformed")
    by_name = {case.get("name"): case for case in cases if isinstance(case, dict)}
    _require(len(by_name) == len(cases) and set(by_name) == set(REQUIRED_FAULT_CONTROLS),
             "source fault controls do not cover the product contract")
    demonstrated = 0
    for name, case in by_name.items():
        desired, differences, unresolved = REQUIRED_FAULT_CONTROLS[name]
        image = resolve_reference(destination.parent, case.get("image"),
                                  f"fault-control image {name}")
        regenerated = _observe_image(image, registration)
        observed = {field: deepcopy(regenerated.get(field)) for field in FIELDS}
        _require(all(isinstance(value, dict) for value in observed.values()),
                 f"current reader omitted a field for fault control {name}")
        comparison = compare_sample(document.get("expected"), observed, role="held")
        case["desired_status"] = desired
        case["required_differences"] = list(differences)
        case["required_unresolved"] = list(unresolved)
        case["observed"] = observed
        case["comparison"] = comparison
        case["demonstrated"] = _fault_control_demonstrated(case, comparison)
        demonstrated += int(case["demonstrated"])
    document["required"] = len(REQUIRED_FAULT_CONTROLS)
    document["demonstrated"] = demonstrated
    _bind_current_static_method(document, runtime, method, camera)
    write_json(destination, document)
    return document


def _verification_summary(verification: dict[str, Any]) -> dict[str, Any]:
    keys = ("status", "errors", "qualification_id", "field_validation",
            "visible_secondary_validation", "fault_controls", "temporal_classifiers")
    summary = {key: deepcopy(verification[key]) for key in keys if key in verification}
    for key in ("field_validation", "visible_secondary_validation", "fault_controls"):
        if isinstance(summary.get(key), dict):
            summary[key].pop("path", None)
    return summary


def reanalyze_static(source_manifest: Path, destination: Path,
                     primary_frequency_reference: Path | None = None) -> dict[str, Any]:
    """Reread immutable static evidence and publish only a verified current bundle."""
    import encounter_reader
    from encounter_product import DEFAULT_POLICY_ID, load_policy
    from encounter_qualification import verify_qualification

    source_manifest = source_manifest.resolve()
    destination = destination.resolve()
    _require(source_manifest.is_file(), f"source qualification manifest is missing: {source_manifest}")
    _require(not destination.exists(), f"destination already exists: {destination}")
    commit, clean = git_identity()
    _require(clean, "static reanalysis requires a clean source tree")
    method = method_hashes()
    static_method = static_method_hashes(method)
    policy = load_policy(DEFAULT_POLICY_ID, POLICY_PATH)
    _require(policy.get("qualified_temporal_classifier_ids") == []
             and policy.get("qualified_temporal_classifiers") == {},
             "static reanalysis requires a default policy with no temporal allowlist")
    source_hash = sha256(source_manifest)
    source_top, _source_temporal = _resolve_base_evidence(source_manifest)
    source_document = read_json(source_manifest)
    _require(isinstance(source_document, dict)
             and isinstance(source_document.get("reader"), dict)
             and isinstance(source_document.get("camera"), dict),
             "source qualification identity is malformed")
    camera = deepcopy(source_document["camera"])
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{destination.name}-", dir=destination.parent))
    candidate = stage / ".static-qualification-candidate.json"
    try:
        runtime = reader_runtime(stage / "reader-cache")
        paths = {
            "field_validation": stage / "field" / source_top["field_validation"].name,
            "visible_secondary_validation": (
                stage / "secondary" / source_top["visible_secondary_validation"].name),
            "fault_controls": stage / "fault" / source_top["fault_controls"].name,
        }
        # One helper serves the complete reread and the verifier's independent
        # second pass. Pixel inputs and reader decisions remain identical.
        with encounter_reader.analysis_session():
            _reanalyze_field_document(
                source_top["field_validation"], paths["field_validation"],
                runtime, static_method, camera, primary_frequency_reference)
            _reanalyze_secondary_document(
                source_top["visible_secondary_validation"],
                paths["visible_secondary_validation"], runtime, static_method, camera)
            _reanalyze_fault_document(
                source_top["fault_controls"], paths["fault_controls"],
                runtime, static_method, camera)
            manifest = {
                "schema_version": 1,
                "kind": "encounter_reader_qualification",
                "qualification_id": f"encounter-reader-static-{commit[:12]}-{source_hash[:12]}",
                "reader": {
                    "method_version": runtime.get("method_version"),
                    "implementation_sha256": static_method,
                    "runtime": runtime,
                },
                "camera": camera,
                "field_validation": reference(paths["field_validation"], stage),
                "visible_secondary_validation": reference(
                    paths["visible_secondary_validation"], stage),
                "fault_controls": reference(paths["fault_controls"], stage),
                "temporal_classifiers": {},
            }
            write_json(candidate, manifest)
            verification = verify_qualification(
                candidate, implementation_sha256=method, reader_runtime=runtime,
                camera_name=camera.get("name"), camera_profile=camera.get("profile"),
                policy=policy, bench_source_sha256=sha256(BENCH_PATH))
        final_commit, final_clean = git_identity()
        _require(final_clean and final_commit == commit,
                 "source tree changed during static reanalysis")
        _require(method_hashes() == method,
                 "reader implementation changed during static reanalysis")
        _require(sha256(source_manifest) == source_hash,
                 "source qualification manifest changed during static reanalysis")
        diagnostic = {
            "schema_version": 1,
            "kind": "static_reader_reanalysis_result",
            "status": verification.get("status"),
            "source_git_sha": commit,
            "source_manifest_sha256": source_hash,
            "policy_id": DEFAULT_POLICY_ID,
            "reader_method_version": runtime.get("method_version"),
            "verification": _verification_summary(verification),
        }
        if verification.get("status") != "QUALIFIED":
            candidate.rename(stage / "rejected-candidate.json")
            write_json(stage / "reanalysis-result.json", diagnostic)
            stage.rename(destination)
            raise WorkflowError(
                "static reanalysis rejected; diagnostics retained at " + str(destination))
        manifest_path = stage / "encounter-reader.json"
        candidate.rename(manifest_path)
        diagnostic["manifest"] = manifest_path.name
        diagnostic["manifest_sha256"] = sha256(manifest_path)
        write_json(stage / "reanalysis-result.json", diagnostic)
        stage.rename(destination)
        return {
            "status": "QUALIFIED",
            "qualification_id": manifest["qualification_id"],
            "manifest": str(destination / manifest_path.name),
            "manifest_sha256": diagnostic["manifest_sha256"],
            "source_manifest_sha256": source_hash,
            "policy_id": DEFAULT_POLICY_ID,
            "temporal_classifiers": [],
        }
    except BaseException as exc:
        if stage.exists():
            try:
                write_json(stage / "reanalysis-result.json", {
                    "schema_version": 1,
                    "kind": "static_reader_reanalysis_result",
                    "status": "ERROR",
                    "source_git_sha": commit,
                    "source_manifest_sha256": source_hash,
                    "policy_id": DEFAULT_POLICY_ID,
                    "error": str(exc),
                })
                stage.rename(destination)
            except OSError:
                shutil.rmtree(stage, ignore_errors=True)
        raise


def _prospective_policy(campaign: dict[str, Any]) -> tuple[dict[str, Any], bytes, dict[str, Any]]:
    policy_document = read_json(POLICY_PATH)
    policy = policy_document["policies"][campaign["policy_id"]]
    ids = list(policy["qualified_temporal_classifier_ids"])
    specs = deepcopy(policy["qualified_temporal_classifiers"])
    targets = _campaign_classifiers(campaign)
    _require(not set(ids).intersection(targets),
             "candidate policy entries already exist before finalization")
    for classifier_id in targets:
        spec = read_json(SPEC_BY_CLASSIFIER[classifier_id])
        ids.append(classifier_id)
        specs[classifier_id] = {
            "classifier_spec_sha256": campaign["classifiers"][classifier_id]["spec"]["sha256"],
            "deadline_observation_semantics": spec["deadline_observation_semantics"],
            "raw_affected_fields": [spec["scope"]["field"]],
        }
        if "verification_closure_semantics" in spec:
            specs[classifier_id]["verification_closure_semantics"] = spec[
                "verification_closure_semantics"]
        if "auxiliary_closure_context_ns" in spec:
            specs[classifier_id]["auxiliary_closure_context_ns"] = spec[
                "auxiliary_closure_context_ns"]
    policy["qualified_temporal_classifier_ids"] = ids
    policy["qualified_temporal_classifiers"] = specs
    return policy_document, json_bytes(policy_document), policy


def _publish_transaction_path(manifest_path: Path) -> Path:
    return manifest_path.parent / f".{manifest_path.name}.publish-transaction.json"


def _encoded_optional(content: bytes | None) -> str | None:
    return None if content is None else base64.b64encode(content).decode("ascii")


def _decoded_optional(value: Any, name: str) -> bytes | None:
    _require(value is None or isinstance(value, str), f"publish transaction {name} is malformed")
    if value is None:
        return None
    try:
        return base64.b64decode(value, validate=True)
    except (ValueError, base64.binascii.Error) as exc:
        raise WorkflowError(f"publish transaction {name} is malformed") from exc


def _restore_bytes(path: Path, content: bytes | None) -> None:
    if content is None:
        _durable_unlink(path)
    else:
        _durable_replace(path, content)


def _publish_transaction(policy_bytes: bytes, manifest_bytes: bytes,
                         manifest_path: Path) -> tuple[Path, dict[str, Any]]:
    marker = _publish_transaction_path(manifest_path)
    originals = {
        "policy": POLICY_PATH.read_bytes() if POLICY_PATH.exists() else None,
        "manifest": manifest_path.read_bytes() if manifest_path.exists() else None,
    }
    transaction = {
        "schema_version": 1,
        "kind": "encounter_qualification_publish_transaction",
        "policy_path": str(POLICY_PATH.resolve()),
        "manifest_path": str(manifest_path.resolve()),
        "old_policy": _encoded_optional(originals["policy"]),
        "new_policy": _encoded_optional(policy_bytes),
        "old_manifest": _encoded_optional(originals["manifest"]),
        "new_manifest": _encoded_optional(manifest_bytes),
        "old_policy_sha256": (None if originals["policy"] is None else
                              hashlib.sha256(originals["policy"]).hexdigest()),
        "new_policy_sha256": hashlib.sha256(policy_bytes).hexdigest(),
        "old_manifest_sha256": (None if originals["manifest"] is None else
                                hashlib.sha256(originals["manifest"]).hexdigest()),
        "new_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
    }
    _require(not marker.exists(), "unfinished qualification publish transaction exists")
    _write_exclusive_json(marker, transaction)
    return marker, transaction


def _transaction_bytes(transaction: dict[str, Any]) -> dict[str, bytes | None]:
    expected_keys = {
        "schema_version", "kind", "policy_path", "manifest_path",
        "old_policy", "new_policy", "old_manifest", "new_manifest",
        "old_policy_sha256", "new_policy_sha256", "old_manifest_sha256",
        "new_manifest_sha256",
    }
    _require(set(transaction) == expected_keys and transaction.get("schema_version") == 1
             and transaction.get("kind") == "encounter_qualification_publish_transaction",
             "unfinished qualification publish transaction is malformed")
    values = {
        "old_policy": _decoded_optional(transaction["old_policy"], "old policy"),
        "new_policy": _decoded_optional(transaction["new_policy"], "new policy"),
        "old_manifest": _decoded_optional(transaction["old_manifest"], "old manifest"),
        "new_manifest": _decoded_optional(transaction["new_manifest"], "new manifest"),
    }
    _require(values["new_policy"] is not None and values["new_manifest"] is not None,
             "unfinished qualification publish transaction has no new bytes")
    for name, content in values.items():
        expected = transaction[f"{name}_sha256"]
        actual = None if content is None else hashlib.sha256(content).hexdigest()
        _require(actual == expected, f"publish transaction {name} digest differs")
    return values


def _recover_publish(manifest_path: Path,
                     postcheck: Callable[[], Any] | None = None) -> Any | None:
    marker = _publish_transaction_path(manifest_path)
    if not marker.exists():
        return None
    transaction = read_json(marker)
    _require(isinstance(transaction, dict)
             and transaction.get("policy_path") == str(POLICY_PATH.resolve())
             and transaction.get("manifest_path") == str(manifest_path.resolve()),
             "unfinished qualification publish transaction targets different files")
    values = _transaction_bytes(transaction)
    current_policy = POLICY_PATH.read_bytes() if POLICY_PATH.exists() else None
    current_manifest = manifest_path.read_bytes() if manifest_path.exists() else None
    _require(current_policy in {values["old_policy"], values["new_policy"]}
             and current_manifest in {values["old_manifest"], values["new_manifest"]},
             "published files differ from recoverable transaction states")
    try:
        # Manifest first keeps an interrupted state fail closed under the old policy.
        _durable_replace(manifest_path, values["new_manifest"])
        _durable_replace(POLICY_PATH, values["new_policy"])
        result = postcheck() if postcheck is not None else None
        _durable_unlink(marker)
        return result
    except Exception as exc:
        _restore_bytes(manifest_path, values["old_manifest"])
        _restore_bytes(POLICY_PATH, values["old_policy"])
        _durable_unlink(marker)
        raise WorkflowError(
            "qualification publish recovery failed; the blind campaign is consumed and a fresh "
            "campaign is required") from exc


def _atomic_publish(policy_bytes: bytes, manifest_bytes: bytes, manifest_path: Path,
                    postcheck: Callable[[], Any] | None = None) -> Any:
    """Publish manifest first with a durable, idempotently recoverable transaction."""
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    marker, transaction = _publish_transaction(policy_bytes, manifest_bytes, manifest_path)
    values = _transaction_bytes(transaction)
    try:
        _durable_replace(manifest_path, manifest_bytes)
        _durable_replace(POLICY_PATH, policy_bytes)
        result = postcheck() if postcheck is not None else None
        _durable_unlink(marker)
        return result
    except Exception:
        _restore_bytes(manifest_path, values["old_manifest"])
        _restore_bytes(POLICY_PATH, values["old_policy"])
        _durable_unlink(marker)
        raise


def finalize(campaign_path: Path, base_manifest: Path, manifest_path: Path) -> dict[str, Any]:
    """Derive blind results, verify the whole bundle, then publish it transactionally."""
    from encounter_product import load_policy
    from encounter_qualification import verify_qualification

    campaign_root, campaign = _campaign(campaign_path)
    manifest_path = manifest_path.resolve()

    def verify_published() -> dict[str, Any]:
        current_policy = load_policy(campaign["policy_id"], POLICY_PATH)
        current_method = method_hashes()
        checked = verify_qualification(
            manifest_path, implementation_sha256=current_method,
            reader_runtime=campaign["reader_runtime"],
            camera_name=campaign["camera"]["name"],
            camera_profile=campaign["camera"]["profile"],
            policy=current_policy, bench_source_sha256=sha256(BENCH_PATH))
        _require(checked.get("status") == "QUALIFIED",
                 "published qualification failed its final verification")
        return checked

    # This happens before the unallowlisted check: a crash may have installed
    # the new manifest while deliberately leaving the old policy fail closed.
    recovered = _recover_publish(manifest_path, verify_published)
    if recovered is not None:
        prepared = campaign_root / "prepared"
        prepared_doc = read_json(prepared / "prepared.json")
        published = read_json(manifest_path)
        matrices = {
            classifier_id: read_json(
                prepared / "qualified" / classifier_id / "comparison.json")["confusion_matrix"]
            for classifier_id in _campaign_classifiers(campaign)
        }
        result = {
            "schema_version": 1,
            "kind": "blind_temporal_qualification_finalized",
            "qualification_id": published["qualification_id"],
            "manifest": str(manifest_path),
            "manifest_sha256": sha256(manifest_path),
            "policy_sha256": sha256(POLICY_PATH),
            "matrices": matrices,
            "verification_status": recovered["status"],
            "publish_recovered": True,
        }
        write_json(prepared / "finalized.json", result)
        return result

    _verify_frozen_source(
        campaign_root, campaign, require_unallowlisted=True,
        runtime_cache=campaign_root / "reader-cache")
    _require(campaign.get("base_manifest_sha256") == sha256(base_manifest.resolve()),
             "base qualification changed after freeze")
    prepared = campaign_root / "prepared"
    prepared_doc = read_json(prepared / "prepared.json")
    _require(prepared_doc.get("kind") == PREPARED_NAME
             and prepared_doc.get("campaign_sha256") == sha256(campaign_root / "campaign.json"),
             "prepared evidence differs from campaign")

    # Complete and validate every visible/source input, the carried base, and
    # one exact analyzer replay before opening any restricted result.
    completed = _completed_observations(prepared, prepared_doc, campaign)
    visible_sources = _validate_pre_key_sources(prepared, prepared_doc, campaign, completed)
    _require(not (prepared / "qualified").exists(),
             "blind campaign already contains post-key results; a fresh campaign is required")
    _policy_document, policy_bytes, policy = _prospective_policy(campaign)
    prospective_method = method_hashes(policy_bytes)
    prospective_static_method = static_method_hashes(prospective_method)
    base_manifest = base_manifest.resolve()
    _require(base_manifest.is_file(), "base qualification manifest is missing")
    validate_base_evidence(
        base_manifest, method_hashes(), campaign["reader_runtime"], campaign["camera"],
        load_policy(campaign["policy_id"], POLICY_PATH))
    static_top, carried_temporal = _resolve_base_evidence(base_manifest)
    _require(not set(carried_temporal).intersection(_campaign_classifiers(campaign)),
             "qualification target already appears in the retained base")
    base = read_json(base_manifest)
    _require(base.get("reader", {}).get("runtime") == campaign["reader_runtime"],
             "base static qualification uses a different reader runtime")
    _require(base.get("camera") == campaign["camera"],
             "base static qualification uses a different camera")
    rederived = _rederive_analysis(prepared, prepared_doc, campaign)

    # From this durable point onward any error requires a fresh campaign.  The
    # observer's answers cannot be tuned after machine/key disclosure.
    _consume_campaign(
        prepared, prepared_doc, visible_sources, base_manifest, policy_bytes)

    rederived_temporal = rederived.get("temporal_classification")
    _require(isinstance(rederived_temporal, dict),
             "qualification reanalysis has no temporal result")
    comparisons: dict[str, dict[str, Any]] = {}
    matrices: dict[str, Any] = {}
    for item in prepared_doc["classifiers"]:
        classifier_id = item["classifier_id"]
        classifier_root = prepared / item["classifier_root"]
        sources = visible_sources[classifier_id]
        analysis_result = read_json(sources["analysis_result"])
        _require(analysis_result.get("temporal_classification") == rederived_temporal,
                 f"qualification reanalysis differs from frozen analysis: {classifier_id}")
        hidden = read_json(sources["restricted_hidden_key"])
        spec_hash = campaign["classifiers"][classifier_id]["spec"]["sha256"]
        comparison = _matrix_document(
            classifier_id, spec_hash, sources,
            completed[classifier_id]["manifest"], completed[classifier_id]["document"], hidden)
        matrices[classifier_id] = comparison["confusion_matrix"]
        _require(comparison["allowlist_decision"]["allowlist_exact_classifier"] is True,
                 f"blind qualification minima failed: {classifier_id} {matrices[classifier_id]}")
        comparisons[classifier_id] = comparison

    # All comparisons appear together or not at all.
    temporal_entries = _publish_comparison_tree(prepared, prepared_doc, comparisons)
    root = manifest_path.parent
    temporal_manifest = {
        classifier_id: {
            "classifier_spec_sha256": entry["classifier_spec_sha256"],
            "implementation_sha256": deepcopy(entry["implementation_sha256"]),
            "spec": reference(entry["spec"], root),
            "validation": reference(entry["validation"], root),
            "source_artifacts": entry["source_artifacts"],
        }
        for classifier_id, entry in carried_temporal.items()
    }
    for classifier_id, entry in temporal_entries.items():
        temporal_manifest[classifier_id] = {
            "classifier_spec_sha256": entry["classifier_spec_sha256"],
            "implementation_sha256": deepcopy(
                campaign["classifiers"][classifier_id]["implementation_sha256"]),
            "spec": reference(entry["spec"], root),
            "validation": reference(entry["validation"], root),
            "source_artifacts": entry["source_artifacts"],
        }
    manifest = {
        "schema_version": 1,
        "kind": "encounter_reader_qualification",
        "qualification_id": (
            f"encounter-reader-{campaign['source_git_sha'][:12]}-"
            f"{prepared_doc['capture_id'][:12]}"),
        "reader": {
            "method_version": campaign["reader_runtime"]["method_version"],
            "implementation_sha256": prospective_static_method,
            "runtime": campaign["reader_runtime"],
        },
        "camera": campaign["camera"],
        "field_validation": reference(static_top["field_validation"], root),
        "visible_secondary_validation": reference(static_top["visible_secondary_validation"], root),
        "fault_controls": reference(static_top["fault_controls"], root),
        "temporal_classifiers": temporal_manifest,
    }
    manifest_bytes = json_bytes(manifest)
    temporary_manifest = root / f".{manifest_path.name}.qualification-candidate"
    _require(not temporary_manifest.exists(), "temporary qualification manifest already exists")
    temporary_manifest.write_bytes(manifest_bytes)
    try:
        verification = verify_qualification(
            temporary_manifest, implementation_sha256=prospective_method,
            reader_runtime=campaign["reader_runtime"],
            camera_name=campaign["camera"]["name"], camera_profile=campaign["camera"]["profile"],
            policy=policy, bench_source_sha256=campaign["bench_source_sha256"])
    finally:
        temporary_manifest.unlink(missing_ok=True)
    _require(verification.get("status") == "QUALIFIED",
             "assembled qualification rejected: " + "; ".join(verification.get("errors", [])))
    final_verification = _atomic_publish(
        policy_bytes, manifest_bytes, manifest_path, verify_published)
    result = {
        "schema_version": 1,
        "kind": "blind_temporal_qualification_finalized",
        "qualification_id": manifest["qualification_id"],
        "manifest": str(manifest_path),
        "manifest_sha256": sha256(manifest_path),
        "policy_sha256": sha256(POLICY_PATH),
        "matrices": matrices,
        "verification_status": final_verification["status"],
        "publish_recovered": False,
    }
    write_json(prepared / "finalized.json", result)
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    freeze_parser = commands.add_parser("freeze", help="freeze code, runtime and blind rubric")
    freeze_parser.add_argument("--out", type=Path, required=True, help="new ignored campaign directory")
    freeze_parser.add_argument(
        "--classifier", action="append", choices=sorted(FIELD_BY_CLASSIFIER),
        dest="classifiers", help=(
            "classifier to qualify; repeat to select several "
            "(default: the four current target classifiers)"))
    freeze_parser.add_argument("--base-manifest", type=Path, default=_default_manifest_path(),
                               help="retained static and any carried temporal evidence to verify before capture")
    prepare_parser = commands.add_parser("prepare", help="read one reserved capture and make blind packets")
    prepare_parser.add_argument("--campaign", type=Path, required=True)
    prepare_parser.add_argument("--run-dir", type=Path, required=True,
                                help="replay directory containing qualification_capture.json")
    finalize_parser = commands.add_parser("finalize", help="derive, verify and publish completed qualification")
    finalize_parser.add_argument("--campaign", type=Path, required=True)
    default_manifest = _default_manifest_path()
    finalize_parser.add_argument("--base-manifest", type=Path, default=default_manifest,
                                 help="existing manifest supplying retained static and carried temporal evidence")
    finalize_parser.add_argument("--manifest", type=Path, default=default_manifest,
                                 help="qualification manifest to publish")
    static_parser = commands.add_parser(
        "reanalyze-static", help="reread immutable static evidence with the current reader")
    static_parser.add_argument("--source-manifest", type=Path, default=default_manifest,
                               help="existing qualification supplying immutable static evidence")
    static_parser.add_argument("--out", type=Path, required=True,
                               help="new ignored directory for the verified static-only bundle")
    static_parser.add_argument("--primary-frequency-reference", type=Path,
                               help="explicit independent frequency adjudication; original labels stay unchanged")
    return parser


def main() -> int:
    from encounter_qualification import QualificationError

    args = build_parser().parse_args()
    try:
        if args.command == "freeze":
            result = freeze(args.out, args.base_manifest,
                            classifier_ids=args.classifiers if args.classifiers is not None else TARGET_CLASSIFIERS)
        elif args.command == "prepare":
            result = prepare(args.campaign, args.run_dir)
        elif args.command == "finalize":
            result = finalize(args.campaign, args.base_manifest, args.manifest)
        else:
            result = reanalyze_static(args.source_manifest, args.out, args.primary_frequency_reference)
        print(json.dumps(result, indent=2, sort_keys=True))
        if result.get("status") == "INSUFFICIENT_CANDIDATE_COVERAGE":
            return 2
        return 0
    except (WorkflowError, QualificationError, KeyError, TypeError, OSError) as exc:
        print(f"qualification workflow failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
