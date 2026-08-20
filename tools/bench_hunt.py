#!/usr/bin/env python3
"""Write one deterministic, advisory investigation report for a replay bench run."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from bench_identity import load_identity_manifest  # noqa: E402
from camera_artifacts import (  # noqa: E402
    CameraArtifactError,
    load_capture_manifest,
    load_owned_grade,
    publish_immutable_json,
    resolve_manifest_artifact,
    validate_capture_window_identity,
    validate_resumable_grade,
    verify_capture_files,
)
from camera_grade import CONSENSUS_MIN_RATIO  # noqa: E402
from import_perf_csv import load_sessions, select_segment  # noqa: E402
from bench_score import score_replay_encounter_csv  # noqa: E402
from run_window import summarize_display_commit_artifact  # noqa: E402

FIELDS = ("alert_visible", "frequency_mhz", "direction")
DIRECTIONS = {"FRONT", "SIDE", "REAR"}
EVIDENCE_ORDER = (
    "bench_result",
    "window_result",
    "identity",
    "performance",
    "encounters",
    "display_commits",
    "camera_grade",
    "scoring",
)
OWNER_REFS = {
    "bench_result": ["bench_result.json"],
    "window_result": ["replay/window_result.json"],
    "identity": ["replay/window_result.json#/identity_manifest"],
    "performance": [
        "replay/window_result.json#/csv_path",
        "replay/window_result.json#/manifest_path",
    ],
    "encounters": ["replay/window_result.json#/encounter_csv_path"],
    "display_commits": ["replay/window_result.json#/artifacts/display_commits"],
    "camera_grade": ["replay/window_result.json#/camera"],
    "scoring": ["replay/window_result.json#/scoring_path"],
}


class HuntError(RuntimeError):
    pass


def _json(path: Path | None) -> tuple[dict[str, Any], str]:
    if path is None:
        return {}, "missing_file"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}, "missing_file"
    except (OSError, json.JSONDecodeError):
        return {}, "invalid_json"
    return (value, "") if isinstance(value, dict) else ({}, "invalid_json")


def _resolve(run: Path, base: Path, reference: Any) -> tuple[Path | None, str]:
    if not isinstance(reference, str) or not reference:
        return None, "missing_reference"
    path = Path(reference)
    path = path if path.is_absolute() else base / path
    try:
        path = path.resolve(strict=True)
        path.relative_to(run)
    except FileNotFoundError:
        return None, "missing_file"
    except (OSError, ValueError):
        return None, "outside_run"
    return (path, "") if path.is_file() else (None, "not_a_file")


def _unavailable(reason: str) -> str:
    return "missing" if reason in {"missing_file", "missing_reference"} else "partial"


def _fact(run: Path, path: Path | None, status: str, reason: str = "") -> dict[str, Any]:
    result = {"status": status, "path": "", "sha256": "", "size_bytes": 0, "reason": reason}
    if path is None:
        return result
    try:
        path = path.resolve(strict=True)
        path.relative_to(run)
        payload = path.read_bytes()
    except (OSError, ValueError):
        result.update({"status": "partial", "reason": "outside_run"})
        return result
    result.update(
        {
            "path": path.relative_to(run).as_posix(),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "size_bytes": len(payload),
        }
    )
    return result


def _finding(
    code: str,
    state: str,
    expected: str,
    observed: str,
    refs: Iterable[str],
    unknown: str = "",
) -> dict[str, Any]:
    return {
        "id": code,
        "state": state,
        "expected": expected,
        "observed": observed,
        "evidence_refs": list(dict.fromkeys(ref for ref in refs if ref)),
        "code_refs": [],
        "unknown": unknown,
    }


def _gap(role: str, item: dict[str, Any]) -> dict[str, Any]:
    return _finding(
        f"evidence-{role}",
        "unknown",
        f"Owned {role.replace('_', ' ')} evidence is complete.",
        f"Evidence is {item['status']}: {item['reason']}.",
        [item.get("path", ""), *(item.get("ownership_refs") or [])],
        "No negative functional claim can be made from missing or partial evidence.",
    )


def _camera_reason(error: CameraArtifactError) -> str:
    message = str(error)
    for prefix, code in (
        ("camera capture input is missing: ", "capture_input_missing"),
        ("camera capture input size changed: ", "capture_input_size_mismatch"),
        ("camera capture input hash changed: ", "capture_input_hash_mismatch"),
    ):
        if message.startswith(prefix):
            return f"{code}:{message[len(prefix):].split(' ', 1)[0]}"
    return "ownership_invalid"


def _number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def _valid_comparison(
    value: Any,
    video_duration: float,
    encounter_expectations: set[tuple[int, str, int, str]],
) -> bool:
    if not isinstance(value, dict):
        return False
    expected, observed, outcome, interval = (
        value.get("expected"),
        value.get("observed"),
        value.get("outcome"),
        value.get("video_consensus_window"),
    )
    if not all(isinstance(item, dict) for item in (expected, observed, outcome, interval)):
        return False
    if any(set(item) != set(FIELDS) for item in (expected, observed, outcome)):
        return False
    start, center, end = (
        interval.get("first_sample_seconds"),
        interval.get("center_seconds"),
        interval.get("last_sample_seconds"),
    )
    if not all(_number(item) for item in (start, center, end, video_duration)):
        return False
    if not 0 <= start <= center <= end <= video_duration or expected["alert_visible"] is not True:
        return False
    encounter_id = value.get("encounter_id")
    replay_time = value.get("replay_time_seconds")
    sample_count = value.get("sample_count")
    visible_count = value.get("visible_sample_count")
    if (
        not isinstance(encounter_id, int)
        or isinstance(encounter_id, bool)
        or encounter_id < 1
        or value.get("event") != "SAMPLE"
        or not _number(replay_time)
        or replay_time < 0
        or not isinstance(sample_count, int)
        or isinstance(sample_count, bool)
        or sample_count < 1
        or not isinstance(visible_count, int)
        or isinstance(visible_count, bool)
        or not 0 <= visible_count <= sample_count
    ):
        return False
    if not isinstance(expected["frequency_mhz"], int) or isinstance(expected["frequency_mhz"], bool):
        return False
    if expected["frequency_mhz"] <= 0 or expected["direction"] not in DIRECTIONS:
        return False
    expected_key = (encounter_id, "SAMPLE", expected["frequency_mhz"], expected["direction"])
    if expected_key not in encounter_expectations:
        return False
    observed_types = (
        observed["alert_visible"] is None or isinstance(observed["alert_visible"], bool),
        observed["frequency_mhz"] is None
        or (isinstance(observed["frequency_mhz"], int) and not isinstance(observed["frequency_mhz"], bool)),
        observed["direction"] is None or observed["direction"] in DIRECTIONS,
    )
    if not all(observed_types):
        return False
    ratios = value.get("consensus_ratio")
    if not isinstance(ratios, dict) or set(ratios) != set(FIELDS):
        return False
    if any(not _number(ratios[field]) or not 0 <= ratios[field] <= 1 for field in FIELDS):
        return False
    for field in FIELDS:
        detail = outcome[field]
        if not isinstance(detail, dict) or detail.get("state") not in {"match", "mismatch", "abstain"}:
            return False
        state = detail["state"]
        if state != "abstain" and ratios[field] < CONSENSUS_MIN_RATIO:
            return False
        if state == "match" and observed[field] != expected[field]:
            return False
        if state == "mismatch" and (observed[field] is None or observed[field] == expected[field]):
            return False
        if state == "abstain" and observed[field] is not None:
            return False
    return True


def build_report(run_directory: Path) -> dict[str, Any]:
    run = run_directory.resolve(strict=True)
    if not run.is_dir():
        raise HuntError("run root is not a directory")
    replay = run / "replay"

    bench_path, bench_path_error = _resolve(run, run, "bench_result.json")
    window_path, window_path_error = _resolve(run, replay, "window_result.json")
    bench, bench_error = _json(bench_path)
    window, window_error = _json(window_path)
    bench_error = bench_path_error or bench_error
    window_error = window_path_error or window_error
    if not bench_error and (
        bench.get("schema_version") not in {2, 3, 4}
        or bench.get("kind") != "bench_result"
        or bench.get("result") not in {"PASS", "WARN", "FAIL", "EVIDENCE_FAILED", "COLLECTION_FAILED"}
    ):
        bench_error = "unsupported_schema"
        bench = {}
    if not window_error and (
        window.get("schema_version") not in {1, 2, 3}
        or window.get("suite") != "replay"
        or window.get("result") not in {"COLLECTED", "RECONNECT_FAILED", "EVIDENCE_FAILED", "COLLECTION_FAILED"}
    ):
        window_error = "unsupported_schema"
        window = {}
    evidence: dict[str, dict[str, Any]] = {
        "bench_result": _fact(run, bench_path, "complete" if not bench_error else _unavailable(bench_error), bench_error),
        "window_result": _fact(run, window_path, "complete" if not window_error else _unavailable(window_error), window_error),
    }

    identity_path, identity_path_error = _resolve(run, replay, window.get("identity_manifest"))
    identity: dict[str, Any] = {}
    identity_reason = identity_path_error
    if identity_path is not None:
        try:
            identity = load_identity_manifest(identity_path)
        except (OSError, RuntimeError, ValueError):
            identity_reason = "ownership_invalid"
    evidence["identity"] = _fact(
        run,
        identity_path,
        "complete" if not identity_reason else _unavailable(identity_reason),
        identity_reason,
    )

    manifest_path, manifest_path_error = _resolve(run, replay, window.get("manifest_path"))
    manifest, manifest_json_error = _json(manifest_path)
    manifest_reason = manifest_path_error or manifest_json_error
    if not manifest_reason and manifest.get("schema_version") != 1:
        manifest_reason = "unsupported_schema"
        manifest = {}
    perf_path, perf_path_error = _resolve(run, replay, window.get("csv_path"))
    perf_reason = perf_path_error
    performance = _fact(run, perf_path, "complete" if not perf_reason else _unavailable(perf_reason), perf_reason)
    manifest_fact = _fact(
        run,
        manifest_path,
        "complete" if not manifest_reason else _unavailable(manifest_reason),
        manifest_reason,
    )
    performance.update(
        {
            "manifest_path": manifest_fact["path"],
            "manifest_sha256": manifest_fact["sha256"],
            "manifest_size_bytes": manifest_fact["size_bytes"],
            "source_schema": manifest.get("source_schema"),
            "selected_rows": (
                manifest.get("selected_segment", {}).get("row_count")
                if isinstance(manifest.get("selected_segment"), dict)
                else None
            ),
        }
    )
    manifest_source, source_error = _resolve(run, replay, manifest.get("source_input"))
    if manifest_reason:
        performance.update(
            {
                "status": "partial" if perf_path is not None or manifest_path is not None else "missing",
                "reason": f"manifest_{manifest_reason}",
            }
        )
    elif manifest_source != perf_path:
        performance.update(
            {"status": "partial", "reason": "source_reference_mismatch" if not source_error else source_error}
        )
    elif perf_path is not None:
        selected = manifest.get("selected_segment")
        try:
            if not isinstance(selected, dict):
                raise ValueError("selected segment missing")
            session_index = selected.get("session_index")
            if not isinstance(session_index, int) or isinstance(session_index, bool) or session_index < 1:
                raise ValueError("selected session invalid")
            _meta, _rows, summary, _summaries, _selector = select_segment(
                load_sessions(perf_path), str(session_index)
            )
            expected_schema = manifest.get("source_schema")
            expected_rows = selected.get("row_count")
            expected_token = selected.get("token")
            if (
                not isinstance(expected_schema, int)
                or isinstance(expected_schema, bool)
                or summary.schema != expected_schema
                or not isinstance(expected_rows, int)
                or isinstance(expected_rows, bool)
                or summary.row_count != expected_rows
                or (isinstance(expected_token, str) and expected_token and summary.token != expected_token)
            ):
                raise ValueError("selected segment mismatch")
        except (OSError, RuntimeError, ValueError):
            performance.update({"status": "partial", "reason": "selected_segment_mismatch"})
    evidence["performance"] = performance

    encounter_path, encounter_path_error = _resolve(run, replay, window.get("encounter_csv_path"))
    encounter_expectations: set[tuple[int, str, int, str]] = set()
    encounter_validation: dict[str, Any] = {}
    encounters = _fact(
        run,
        encounter_path,
        "complete" if not encounter_path_error else _unavailable(encounter_path_error),
        encounter_path_error,
    )
    if encounter_path is not None:
        try:
            with encounter_path.open("r", encoding="utf-8", newline="") as handle:
                numbered = [(number, line) for number, line in enumerate(handle, 1) if not line.startswith("#")]
            rows = list(csv.DictReader(line for _number, line in numbered))
            drops = [int(row["dropped_snapshots"]) for row in rows]
            priority_count = sum(
                str(row.get("event") or "") in {"START", "SAMPLE", "END"}
                and int(row.get("priority") or 0) == 1
                for row in rows
            )
            maximum_drop = max(drops)
            encounter_expectations = {
                (
                    int(row["encounter_id"]),
                    str(row["event"]).upper(),
                    int(row["frequency_mhz"]),
                    str(row["direction"]).upper(),
                )
                for row in rows
                if int(row.get("priority") or 0) == 1 and str(row.get("event") or "").upper() == "SAMPLE"
            }
        except (OSError, ValueError, KeyError, csv.Error):
            encounters.update({"status": "partial", "reason": "parse_error"})
            maximum_drop, priority_count, rows = 0, 0, []
        encounters.update(
            {
                "row_count": len(rows),
                "priority_observation_count": priority_count,
                "reported_dropped_snapshots": maximum_drop,
            }
        )
        encounter_validation = score_replay_encounter_csv(encounter_path)
        if encounter_validation.get("result") == "COLLECTION_FAILED" and not maximum_drop:
            encounters.update({"status": "partial", "reason": "collection_invalid"})
        if maximum_drop:
            encounters.update({"status": "partial", "reason": "reported_drops"})
    evidence["encounters"] = encounters

    artifacts = window.get("artifacts") if isinstance(window.get("artifacts"), dict) else {}
    owned_display = artifacts.get("display_commits")
    if not isinstance(owned_display, dict):
        display = _fact(run, None, "missing", "not_owned_by_window")
    else:
        display_path, display_path_error = _resolve(run, replay, owned_display.get("path"))
        if display_path is None:
            display = _fact(run, None, _unavailable(display_path_error), display_path_error)
        else:
            summary = summarize_display_commit_artifact(display_path, replay)
            display = _fact(run, display_path, str(summary.get("status") or "partial"))
            summary_fields = (
                "scope",
                "csv_schema_version",
                "timebase",
                "source",
                "row_count",
                "first_sequence",
                "last_sequence",
                "terminal_sequence",
                "reported_dropped_commits",
                "sequence_contiguous_from_one",
                "drop_counter_monotonic",
            )
            for key in summary_fields:
                display[key] = summary.get(key)
            compared = ("status", "sha256", "size_bytes", *summary_fields)
            if any(owned_display.get(key) != summary.get(key) for key in compared):
                display.update({"status": "partial", "reason": "window_summary_mismatch"})
            elif summary.get("status") != "complete":
                display["reason"] = str(summary.get("reason") or "parse_error")
    evidence["display_commits"] = display

    capture: dict[str, Any] = {}
    grade: dict[str, Any] = {}
    camera_diagnostics: list[Any] = []
    grade_ref = video_ref = ""
    camera = window.get("camera") if isinstance(window.get("camera"), dict) else {}
    camera_dir = replay / "camera"
    capture_path, capture_path_error = _resolve(run, camera_dir, camera.get("capture_manifest"))
    camera_reason = capture_path_error or ("not_owned_by_window" if not camera else "")
    camera_fact = _fact(run, capture_path, "complete" if not camera_reason else _unavailable(camera_reason), camera_reason)
    capture_paths: dict[str, Path] = {}
    grader = str(window.get("grader_fingerprint") or "")
    if capture_path is not None:
        try:
            capture = load_capture_manifest(capture_path)
            validate_capture_window_identity(
                capture,
                suite="replay",
                product_fingerprint=str(window.get("product_fingerprint") or ""),
                scenario_fingerprint=str(window.get("scenario_fingerprint") or ""),
            )
            capture_identity = capture.get("identity") if isinstance(capture.get("identity"), dict) else {}
            capture_artifacts = (
                capture_identity.get("artifacts")
                if isinstance(capture_identity.get("artifacts"), dict)
                else {}
            )
            for name in sorted(capture_artifacts):
                candidate = resolve_manifest_artifact(camera_dir, capture, name)
                if candidate is None:
                    raise HuntError(f"capture_input_missing:{name}")
                candidate = candidate.resolve(strict=True)
                try:
                    candidate.relative_to(run)
                except ValueError as exc:
                    raise HuntError(f"capture_input_outside:{name}") from exc
                capture_paths[name] = candidate
            verify_capture_files(camera_dir, capture)
            grade = load_owned_grade(camera_dir, capture, grader) or {}
            if not grade:
                raise HuntError("grade_missing")
            validate_resumable_grade(grade)
            camera_diagnostics = list(grade.get("diagnostics") or [])
        except HuntError as exc:
            camera_reason = str(exc)
        except CameraArtifactError as exc:
            camera_reason = _camera_reason(exc)
        except (OSError, RuntimeError, ValueError):
            camera_reason = "ownership_invalid"
    grade_path, grade_path_error = _resolve(run, camera_dir, camera.get("grade"))
    expected_grade = (camera_dir / "grades" / f"{grader}.json").resolve()
    if not camera_reason and (grade_path is None or grade_path != expected_grade):
        camera_reason = grade_path_error or "reference_mismatch"
    if not camera_reason and grade_path is not None:
        camera_fact = _fact(run, grade_path, "complete")
        capture_payload = capture.get("capture") if isinstance(capture.get("capture"), dict) else {}
        capture_identity = capture.get("identity") if isinstance(capture.get("identity"), dict) else {}
        capture_artifacts = (
            capture_identity.get("artifacts") if isinstance(capture_identity.get("artifacts"), dict) else {}
        )
        video_identity = capture_artifacts.get("video") if isinstance(capture_artifacts.get("video"), dict) else {}
        video_duration = video_identity.get("duration_seconds")
        if capture_payload.get("video_duration_seconds") != video_duration:
            camera_reason = "capture_summary_mismatch"
        elif capture_paths.get("encounter_csv") != encounter_path:
            camera_reason = "encounter_reference_mismatch"
        elif camera.get("capture_id") and camera.get("capture_id") != capture.get("capture_id"):
            camera_reason = "window_summary_mismatch"
        elif camera.get("grader_fingerprint") and camera.get("grader_fingerprint") != grader:
            camera_reason = "window_summary_mismatch"
        elif camera.get("grade_result") and camera.get("grade_result") != grade.get("result"):
            camera_reason = "window_summary_mismatch"
        comparisons = grade.get("encounter_comparisons")
        if not camera_reason and not isinstance(comparisons, list):
            camera_reason = "encounter_comparisons_missing"
        elif not camera_reason and grade.get("result") in {"PASS", "FAIL"} and not comparisons:
            camera_reason = "encounter_comparisons_missing"
        elif not camera_reason:
            invalid = next(
                (
                    index
                    for index, item in enumerate(comparisons)
                    if not _valid_comparison(item, video_duration, encounter_expectations)
                ),
                None,
            )
            if invalid is not None:
                camera_reason = f"comparison_invalid:{invalid}"
        if grade.get("result") == "INCONCLUSIVE":
            camera_reason = "grade_inconclusive"
        camera_fact.update(
            {
                "comparison_count": len(comparisons) if isinstance(comparisons, list) else 0,
                "grade_result": grade.get("result"),
            }
        )
        grade_ref = camera_fact["path"]
        if "video" in capture_paths:
            video_ref = capture_paths["video"].relative_to(run).as_posix()
    if camera_reason:
        camera_fact.update({"status": "partial" if capture_path else _unavailable(camera_reason), "reason": camera_reason})
        if camera_reason != "grade_inconclusive":
            camera_diagnostics = []
        grade = {}
    evidence["camera_grade"] = camera_fact

    scoring_path, scoring_path_error = _resolve(run, replay, window.get("scoring_path"))
    scoring, scoring_json_error = _json(scoring_path)
    scoring_reason = scoring_path_error or scoring_json_error
    scoring_fact = _fact(
        run,
        scoring_path,
        "complete" if not scoring_reason else _unavailable(scoring_reason),
        scoring_reason,
    )
    scored_metrics = scoring.get("metrics") if isinstance(scoring.get("metrics"), list) else None
    scoring_manifest = scoring.get("manifest") if isinstance(scoring.get("manifest"), dict) else {}
    if not scoring_reason and scoring.get("schema_version") != 1:
        scoring_reason = "unsupported_schema"
    if not scoring_reason and scoring_manifest.get("hardware_scoring_fingerprint") != window.get(
        "hardware_scoring_fingerprint"
    ):
        scoring_reason = "hardware_scoring_identity_mismatch"
    if not scoring_reason and scored_metrics is None:
        scoring_reason = "metrics_missing"
    if not scoring_reason:
        for index, metric in enumerate(scored_metrics or []):
            valid = (
                isinstance(metric, dict)
                and isinstance(metric.get("metric"), str)
                and re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", metric.get("metric")) is not None
                and metric.get("score_status") in {"pass", "warn", "info", "fail", "unsupported"}
                and isinstance(metric.get("messages"), list)
            )
            if valid and metric.get("score_status") == "fail":
                value = metric.get("current_value")
                valid = (
                    isinstance(value, (int, float))
                    and not isinstance(value, bool)
                    and math.isfinite(value)
                    and metric.get("unit") in {"bitmask", "bytes", "count", "ms", "percent", "px", "us"}
                )
            if not valid:
                scoring_reason = f"metric_invalid:{index}"
                break
    if not scoring_reason:
        scored_manifest_path, scored_manifest_path_error = _resolve(
            run, replay, scoring_manifest.get("path")
        )
        if (
            scored_manifest_path_error
            or scored_manifest_path != manifest_path
            or scoring_manifest.get("run_id") != manifest.get("run_id")
            or scoring_manifest.get("git_sha") != manifest.get("git_sha")
            or scoring_manifest.get("selected_segment") != manifest.get("selected_segment")
        ):
            scoring_reason = "manifest_identity_mismatch"
    if scoring_reason:
        scoring_fact.update({"status": "partial" if scoring_path else _unavailable(scoring_reason), "reason": scoring_reason})
        scored_metrics = []
    evidence["scoring"] = scoring_fact

    for role, refs in OWNER_REFS.items():
        evidence[role]["ownership_refs"] = refs
    findings = [_gap(role, evidence[role]) for role in EVIDENCE_ORDER if evidence[role]["status"] != "complete"]

    trace = identity.get("traceability") if isinstance(identity.get("traceability"), dict) else {}
    claims = {
        "git_sha": (bench.get("git_sha"), window.get("git_sha"), manifest.get("git_sha"), trace.get("repository_sha")),
        "product_fingerprint": (
            bench.get("product_fingerprint"),
            window.get("product_fingerprint"),
            manifest.get("product_fingerprint"),
            identity.get("product_fingerprint"),
        ),
        "scenario_fingerprint": (
            window.get("scenario_fingerprint"),
            manifest.get("scenario_fingerprint"),
            identity.get("scenario_fingerprint"),
        ),
        "grader_fingerprint": (
            bench.get("grader_fingerprint"),
            window.get("grader_fingerprint"),
            identity.get("grader_fingerprint"),
        ),
        "hardware_scoring_fingerprint": (
            bench.get("hardware_scoring_fingerprint"),
            window.get("hardware_scoring_fingerprint"),
            manifest.get("hardware_scoring_fingerprint"),
            identity.get("hardware_scoring_fingerprint"),
        ),
    }
    agreed: dict[str, str] = {}
    disagreements: list[str] = []
    for field, values in claims.items():
        present = [value for value in values if isinstance(value, str) and value]
        width = 40 if field == "git_sha" else 64
        valid = bool(present) and all(re.fullmatch(rf"[0-9A-Fa-f]{{{width}}}", value) for value in present)
        agreed[field] = present[0].lower() if valid and len(set(present)) == 1 else "unknown"
        if present and (not valid or len(set(present)) > 1):
            disagreements.append(field)
    if disagreements:
        refs = ["bench_result.json", "replay/window_result.json"]
        refs.extend(
            path
            for path in (evidence["identity"]["path"], evidence["performance"]["manifest_path"])
            if path
        )
        findings.append(
            _finding(
                "evidence-identity-disagreement",
                "unknown",
                "Owned artifacts agree on run identity.",
                "Identity claims disagree: " + ", ".join(disagreements) + ".",
                refs,
                "The disagreeing fields cannot be used for cross-evidence attribution.",
            )
        )
    identity_agreed = (
        not disagreements
        and all(
            evidence[role]["status"] == "complete"
            for role in ("bench_result", "window_result", "identity", "performance")
        )
        and all(value != "unknown" for value in agreed.values())
    )

    if encounter_validation.get("result") == "FAIL":
        details = encounter_validation.get("evidence")
        detail = details[0] if isinstance(details, list) and details and isinstance(details[0], str) else ""
        if not re.fullmatch(r"[A-Za-z0-9 .,:_-]+", detail):
            detail = "replay encounter semantics differ from the authored sequence"
        findings.append(
            _finding(
                "encounter-code-oracle-mismatch",
                "confirmed" if identity_agreed else "unknown",
                "The replay encounter trace matches the sequence authored in code.",
                f"The deterministic encounter validator returned FAIL: {detail}.",
                [evidence["encounters"]["path"]],
                (
                    "The mismatch is exact, but its causal function is unknown."
                    if identity_agreed
                    else "Run identity is incomplete or disagrees across evidence."
                ),
            )
        )

    encounter_drops = evidence["encounters"].get("reported_dropped_snapshots")
    if isinstance(encounter_drops, int) and encounter_drops > 0:
        findings.append(
            _finding(
                "encounter-snapshots-dropped",
                "confirmed" if identity_agreed else "unknown",
                "The encounter logger reports no lost snapshots in the owned prefix.",
                f"The encounter logger reported {encounter_drops} dropped snapshots.",
                [evidence["encounters"]["path"]],
                (
                    "The missing parser states cannot be reconstructed."
                    if identity_agreed
                    else "Run identity disagreement prevents cross-evidence attribution."
                ),
            )
        )
    display_drops = evidence["display_commits"].get("reported_dropped_commits")
    display_reasons = str(evidence["display_commits"].get("reason") or "").split(",")
    if isinstance(display_drops, int) and display_drops > 0 and "reported_drops" in display_reasons:
        findings.append(
            _finding(
                "renderer-commits-dropped",
                "confirmed" if identity_agreed else "unknown",
                "The renderer logger reports no lost commits in the owned prefix.",
                f"The renderer logger reported {display_drops} dropped commits.",
                [evidence["display_commits"]["path"]],
                (
                    "The missing renderer commits cannot be reconstructed."
                    if identity_agreed
                    else "Run identity disagreement prevents cross-evidence attribution."
                ),
            )
        )

    for index, metric in sorted(
        (
            (index, item)
            for index, item in enumerate(scored_metrics or [])
            if isinstance(item, dict) and item.get("score_status") == "fail"
        ),
        key=lambda pair: (str(pair[1].get("metric") or ""), pair[0]),
    ):
        name = str(metric["metric"])
        observed = f"{name} current={metric.get('current_value')} {metric.get('unit') or ''} score_status=fail".strip()
        messages = metric.get("messages") or []
        if messages:
            match = re.fullmatch(
                r"value (-?[0-9]+(?:\.[0-9]+)?) (above max|below min) (-?[0-9]+(?:\.[0-9]+)?)",
                str(messages[0]),
            )
            if match:
                observed = f"value {match.group(1)} {match.group(2)} {match.group(3)}"
        findings.append(
            _finding(
                f"metric-threshold-{name}",
                "confirmed" if identity_agreed else "unknown",
                "The existing deterministic metric check passes.",
                observed,
                [f"{evidence['scoring']['path']}#/metrics/{index}"],
                (
                    "The threshold result does not identify a causal function."
                    if identity_agreed
                    else "Run identity disagreement prevents cross-evidence attribution."
                ),
            )
        )

    comparisons = grade.get("encounter_comparisons") if evidence["camera_grade"]["status"] == "complete" else []
    for index, item in enumerate(comparisons if isinstance(comparisons, list) else []):
        outcome = item["outcome"]
        mismatch = sorted(field for field in FIELDS if outcome[field]["state"] == "mismatch")
        abstain = sorted(field for field in FIELDS if outcome[field]["state"] == "abstain")
        expected = json.dumps(item["expected"], sort_keys=True, separators=(",", ":"))
        observed = json.dumps(item["observed"], sort_keys=True, separators=(",", ":"))
        interval = item["video_consensus_window"]
        refs = [f"{grade_ref}#/encounter_comparisons/{index}", evidence["encounters"]["path"]]
        if video_ref:
            refs.append(
                f"{video_ref}#t={interval['first_sample_seconds']:.3f}-{interval['last_sample_seconds']:.3f}"
            )
        if mismatch:
            findings.append(
                _finding(
                    f"camera-mismatch-{index + 1:04d}",
                    "confirmed" if identity_agreed else "unknown",
                    f"Encounter expectation {expected} matches camera consensus.",
                    f"Consensus {observed} differs in {', '.join(mismatch)}.",
                    refs,
                    (
                        "No renderer commit or causal function is linked to this disagreement."
                        if identity_agreed
                        else "Run identity disagreement prevents cross-evidence attribution."
                    ),
                )
            )
        if abstain:
            findings.append(
                _finding(
                    f"camera-abstention-{index + 1:04d}",
                    "unknown",
                    f"Encounter expectation {expected} is readable in the camera interval.",
                    f"Camera consensus abstained for {', '.join(abstain)}.",
                    refs,
                    "The displayed value is unknown for the abstained fields.",
                )
            )
    for index, diagnostic in enumerate(camera_diagnostics):
        code = diagnostic.get("code") if isinstance(diagnostic, dict) else ""
        code = code if isinstance(code, str) and re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", code) else ""
        findings.append(
            _finding(
                f"camera-diagnostic-{index + 1:03d}",
                "unknown",
                "The camera grader can compare replay evidence.",
                f"Camera diagnostic recorded: {code or 'unspecified'}.",
                [f"{grade_ref}#/diagnostics/{index}"],
                "The diagnostic prevents a complete visual conclusion.",
            )
        )

    if evidence["display_commits"]["status"] == "complete" and comparisons:
        findings.extend(
            (
                _finding(
                    "renderer-camera-correlation-unmeasured",
                    "unknown",
                    "Each camera comparison links to the renderer commit that caused it.",
                    "Renderer evidence uses DUT millis/sequence; camera evidence uses aligned 3 Hz windows without a shared event ID.",
                    [evidence["display_commits"]["path"], grade_ref + "#/encounter_comparisons"],
                    "The causing renderer commit is unknown.",
                ),
                _finding(
                    "transition-latency-unmeasured",
                    "unknown",
                    "Stimulus-to-visible latency is measured against a code-owned deadline.",
                    "Camera comparisons are steady-state 3 Hz windows and exclude transitions.",
                    [grade_ref + "#/encounter_comparisons"],
                    "Renderer-to-panel/video latency and deadline compliance are unknown.",
                ),
            )
        )

    findings.sort(key=lambda item: (0 if item["state"] == "confirmed" else 1, item["id"]))
    return {
        "format": 1,
        "kind": "bench_function_hunt",
        "verdict_effect": "none",
        "run": {"git_sha": agreed["git_sha"]},
        "evidence": evidence,
        "findings": findings,
    }


def publish_report(run_directory: Path) -> tuple[Path, bool]:
    run = run_directory.resolve(strict=True)
    output = run / "hunt.json"
    try:
        created = publish_immutable_json(output, build_report(run))
    except CameraArtifactError as exc:
        raise HuntError("hunt.json already differs") from exc
    return output, created


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", required=True)
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        output, created = publish_report(Path(args.run_dir))
    except FileNotFoundError:
        print("bench hunt failed: invalid_run_root", file=sys.stderr)
        return 2
    except OSError:
        print("bench hunt failed: io_error", file=sys.stderr)
        return 2
    except HuntError as exc:
        reason = str(exc)
        if reason not in {"run root is not a directory", "hunt.json already differs"}:
            reason = "internal_error"
        print(f"bench hunt failed: {reason.replace(' ', '_')}", file=sys.stderr)
        return 2
    print(f"{'wrote' if created else 'verified'} {output.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
