#!/usr/bin/env python3
"""Resume-safe regrading of archived replay camera captures."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterable

from bench_identity import current_grader_fingerprint, load_identity_manifest
from camera_artifacts import (
    CAPTURE_MANIFEST_NAME,
    CameraArtifactConflict,
    CameraArtifactError,
    grade_path,
    load_or_adapt_capture,
    load_owned_grade,
    publish_grade,
    publish_immutable_json,
    validate_resumable_grade,
    verify_capture_files,
)
from camera_grade import grade_camera
from artifact_privacy import sanitize_artifact_value


ROOT = Path(__file__).resolve().parents[2]


def _read_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return payload if isinstance(payload, dict) else {}


def inventory_camera_dirs(corpus_root: Path) -> list[Path]:
    candidates = {
        path.parent
        for pattern in (f"**/replay/camera/{CAPTURE_MANIFEST_NAME}", "**/replay/camera/camera_result.json")
        for path in corpus_root.glob(pattern)
    }
    return sorted(candidates)


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus-root", required=True)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Inventory captures without hashing or decoding legacy media",
    )
    parser.add_argument(
        "--report",
        default="",
        help="Optional immutable JSON completion report path",
    )
    return parser.parse_args(argv)


def regrade_corpus(
    corpus_root: Path,
    *,
    dry_run: bool = False,
    entries: list[dict[str, Any]] | None = None,
) -> tuple[dict[str, int], int]:
    corpus_root = corpus_root.resolve()
    report_entries = entries if entries is not None else []
    grader_fingerprint = current_grader_fingerprint(ROOT)
    counts = {
        "discovered": 0,
        "processed": 0,
        "graded": 0,
        "skipped": 0,
        "pass": 0,
        "fail": 0,
        "inconclusive": 0,
        "incompatible": 0,
        "conflict": 0,
    }
    for camera_dir in inventory_camera_dirs(corpus_root):
        counts["discovered"] += 1
        entry: dict[str, Any] = {
            "capture_index": counts["discovered"],
            "capture_id": "",
            "result": "",
            "confidence_result": "",
            "ownership_valid": False,
            "grade": {
                "status": "inventory_only" if dry_run else "missing",
                "grader_fingerprint": grader_fingerprint,
                "grade_id": "",
                "ownership_valid": False,
            },
            "diagnostic": "",
        }
        if dry_run:
            current_path = grade_path(camera_dir, grader_fingerprint)
            if current_path.is_file():
                counts["skipped"] += 1
            report_entries.append(entry)
            continue
        counts["processed"] += 1
        replay_dir = camera_dir.parent
        window_result = _read_json(replay_dir / "window_result.json")
        capture: dict[str, Any] = {}
        try:
            identity_path = replay_dir / str(window_result.get("identity_manifest") or "identity.json")
            identity = load_identity_manifest(identity_path) if identity_path.is_file() else {}
            capture = load_or_adapt_capture(
                camera_dir=camera_dir,
                replay_dir=replay_dir,
                window_result=window_result,
                identity_manifest=identity,
                publish_adapted=True,
            )
            entry["capture_id"] = str(capture.get("capture_id") or "")
            verify_capture_files(camera_dir, capture)
            current_grade_path = grade_path(camera_dir, grader_fingerprint)
            try:
                existing = load_owned_grade(camera_dir, capture, grader_fingerprint)
                if existing is not None:
                    entry["ownership_valid"] = True
                    entry["grade"]["ownership_valid"] = True
                    entry["grade"]["grade_id"] = str(existing.get("grade_id") or "")
                    validate_resumable_grade(existing)
            except CameraArtifactError as exc:
                if current_grade_path.exists():
                    raise CameraArtifactConflict(str(exc)) from exc
                raise
            if existing is not None:
                counts["skipped"] += 1
                result = str(existing.get("result") or "").lower()
                if result in {"pass", "fail", "inconclusive"}:
                    counts[result] += 1
                entry.update(
                    {
                        "result": str(existing.get("result") or ""),
                        "confidence_result": str(
                            (existing.get("confidence") or {}).get("result") or ""
                        ),
                        "ownership_valid": True,
                    }
                )
                entry["grade"] = {
                    "status": "skipped",
                    "grader_fingerprint": grader_fingerprint,
                    "grade_id": str(existing.get("grade_id") or ""),
                    "ownership_valid": True,
                }
                report_entries.append(entry)
                continue
            grade = grade_camera(
                camera_dir=camera_dir,
                capture_manifest=capture,
                grader_fingerprint=grader_fingerprint,
            )
            _published_path, _created = publish_grade(
                camera_dir,
                capture,
                grader_fingerprint,
                grade,
            )
            counts["graded"] += 1
            result = str(grade.get("result") or "").lower()
            if result in {"pass", "fail", "inconclusive"}:
                counts[result] += 1
            entry.update(
                {
                    "result": str(grade.get("result") or ""),
                    "confidence_result": str((grade.get("confidence") or {}).get("result") or ""),
                    "ownership_valid": True,
                }
            )
            entry["grade"] = {
                "status": "graded",
                "grader_fingerprint": grader_fingerprint,
                "grade_id": str(grade.get("grade_id") or ""),
                "ownership_valid": True,
            }
            report_entries.append(entry)
        except CameraArtifactConflict as exc:
            counts["conflict"] += 1
            entry["grade"]["status"] = "conflict"
            entry["diagnostic"] = str(exc)
            report_entries.append(entry)
        except (CameraArtifactError, RuntimeError) as exc:
            counts["incompatible"] += 1
            entry["grade"]["status"] = "incompatible"
            entry["diagnostic"] = str(exc)
            report_entries.append(entry)
    return counts, 2 if counts["conflict"] or counts["incompatible"] else 0


def build_regrade_report(corpus_root: Path, *, dry_run: bool = False) -> tuple[dict[str, Any], int]:
    corpus_root = corpus_root.resolve()
    entries: list[dict[str, Any]] = []
    counts, returncode = regrade_corpus(corpus_root, dry_run=dry_run, entries=entries)
    accounted = counts["graded"] + counts["skipped"] + counts["conflict"] + counts["incompatible"]
    completed = bool(
        not dry_run
        and counts["processed"] == counts["discovered"]
        and accounted == counts["discovered"]
        and len(entries) == counts["discovered"]
        and counts["conflict"] == 0
        and counts["incompatible"] == 0
    )
    report = {
        "schema_version": 2,
        "kind": "bench_camera_regrade_report",
        "completed": completed,
        "dry_run": dry_run,
        "grader_fingerprint": current_grader_fingerprint(ROOT),
        "scope": "complete_corpus_inventory",
        "counts": counts,
        "captures": entries,
    }
    return sanitize_artifact_value(report, run_dir=corpus_root), returncode


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    corpus_root = Path(args.corpus_root).resolve()
    report, returncode = build_regrade_report(corpus_root, dry_run=args.dry_run)
    if args.report:
        try:
            publish_immutable_json(Path(args.report).resolve(), report)
        except (CameraArtifactError, OSError, TypeError, ValueError) as exc:
            safe_error = sanitize_artifact_value(str(exc), run_dir=corpus_root)
            print(f"camera regrade report publication failed: {safe_error}", file=sys.stderr)
            return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return returncode


if __name__ == "__main__":
    raise SystemExit(main())
