#!/usr/bin/env python3
"""Requalify the static pixel reader against retained, independent reference images.

Original labels and evidence remain unchanged. A new manifest is published only
after the reader and its fault controls pass independent verification.
"""

from __future__ import annotations

import argparse
from collections import Counter
from copy import deepcopy
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Any

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parents[1]
BENCH_PATH = REPO_ROOT / "bench.sh"

def _default_manifest_path() -> Path:
    artifact_root = Path(os.environ.get(
        "BENCH_ARTIFACT_ROOT", REPO_ROOT / ".artifacts/bench"))
    return Path(os.environ.get(
        "BENCH_ENCOUNTER_QUALIFICATION", artifact_root / "qualification/encounter-reader.json"))


DEFAULT_MANIFEST = _default_manifest_path()
_SHA256 = re.compile(r"[0-9a-f]{64}")


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


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise WorkflowError(f"unreadable evidence file: {path}") from exc
    return digest.hexdigest()


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


def method_hashes() -> dict[str, str]:
    files = list(BENCH_DIR.glob("*.py"))
    files += list(BENCH_DIR.glob("encounter_*.swift"))
    files += list(BENCH_DIR.glob("encounter_*.png"))
    files += list(BENCH_DIR.glob("encounter_*.b64"))
    result = {path.name: sha256(path) for path in files}
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
    if kind == "secondary" and "secondary_reference" in document:
        from encounter_secondary_reference import copy_reference
        value = document["secondary_reference"]
        copy_reference(resolve_reference(source_root, value, "secondary reference"),
                       destination_root / value["path"])
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
        # Preserve the copied historical packet when an explicit supplement
        # reuses its artifact names with newly frozen labels or images.
        retained = destination.parent / f"source/primary-frequency-{sha256(primary_frequency_reference)}/reference.json"
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
                                  camera: dict[str, Any], secondary_reference: Path | None = None) -> dict[str, Any]:
    from encounter_qualification import (
        CORE_READER_FILES, _blind_secondary_reference, _derived_field_status, _observe_image)

    document = read_json(source)
    _require(isinstance(document, dict), "source visible-secondary validation is malformed")
    source_reader = deepcopy(document.get("reader"))
    source_method = deepcopy(document.get("method"))
    _copy_static_inputs(source.parent, destination.parent, document, "secondary")
    if secondary_reference is not None:
        from encounter_secondary_reference import copy_reference
        retained = destination.parent / f"source/secondary-reference-{sha256(secondary_reference)}/reference.json"
        copy_reference(secondary_reference, retained)
        document["secondary_reference"] = reference(retained, destination.parent)
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
    if "secondary_reference" in document:
        from encounter_secondary_reference import reference_reread_binding, validate_reference
        supplement = resolve_reference(destination.parent, document["secondary_reference"], "secondary reference")
        try:
            binding = reference_reread_binding(supplement, method)
            if binding is None:
                document.pop("secondary_reference_reanalysis", None)
            else:
                document["secondary_reference_reanalysis"] = binding
            document["secondary_reference_summary"] = validate_reference(
                supplement, method, _observe_image, camera=camera, reader_reanalysis=binding)
        except (OSError, ValueError, TypeError, KeyError) as exc:
            raise WorkflowError(str(exc)) from exc
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
                     primary_frequency_reference: Path | None = None,
                     secondary_reference: Path | None = None) -> dict[str, Any]:
    """Reread immutable static evidence and publish only a verified current bundle."""
    import encounter_reader
    from encounter_qualification import verify_qualification

    source_manifest = source_manifest.resolve()
    destination = destination.resolve()
    _require(source_manifest.is_file(), f"source qualification manifest is missing: {source_manifest}")
    _require(not destination.exists(), f"destination already exists: {destination}")
    commit, clean = git_identity()
    method = method_hashes()
    static_method = static_method_hashes(method)
    method_digest = hashlib.sha256(json_bytes(static_method)).hexdigest()
    policy = {"contract_version": 3, "qualified_temporal_classifiers": {}}
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
                paths["visible_secondary_validation"], runtime, static_method, camera, secondary_reference)
            _reanalyze_fault_document(
                source_top["fault_controls"], paths["fault_controls"],
                runtime, static_method, camera)
            manifest = {
                "schema_version": 1,
                "kind": "encounter_reader_qualification",
                "qualification_id": f"encounter-reader-static-{method_digest[:12]}-{source_hash[:12]}",
                "source": {"git_sha": commit, "worktree_clean": clean,
                           "static_method_sha256": method_digest},
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
        _require(final_commit == commit and final_clean == clean,
                 "source tree identity changed during static reanalysis")
        _require(method_hashes() == method,
                 "reader implementation changed during static reanalysis")
        _require(sha256(source_manifest) == source_hash,
                 "source qualification manifest changed during static reanalysis")
        diagnostic = {
            "schema_version": 1,
            "kind": "static_reader_reanalysis_result",
            "status": verification.get("status"),
            "source_git_sha": commit,
            "source_worktree_clean": clean,
            "static_method_sha256": method_digest,
            "source_manifest_sha256": source_hash,
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
                    "source_worktree_clean": clean,
                    "static_method_sha256": method_digest,
                    "source_manifest_sha256": source_hash,
                    "error": str(exc),
                })
                stage.rename(destination)
            except OSError:
                shutil.rmtree(stage, ignore_errors=True)
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    default_manifest = _default_manifest_path()
    static_parser = commands.add_parser(
        "reanalyze-static", help="reread immutable static evidence with the current reader")
    static_parser.add_argument("--source-manifest", type=Path, default=default_manifest,
                               help="existing qualification supplying immutable static evidence")
    static_parser.add_argument("--out", type=Path, required=True,
                               help="new ignored directory for the verified static-only bundle")
    static_parser.add_argument("--primary-frequency-reference", type=Path,
                               help="explicit independent frequency adjudication; original labels stay unchanged")
    static_parser.add_argument("--secondary-reference", type=Path,
                               help="additional independent card originals; historical packet stays unchanged")
    return parser


def main() -> int:
    from encounter_qualification import QualificationError

    args = build_parser().parse_args()
    try:
        result = reanalyze_static(args.source_manifest, args.out, args.primary_frequency_reference,
                                 args.secondary_reference)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (WorkflowError, QualificationError, KeyError, TypeError, OSError) as exc:
        print(f"qualification workflow failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
