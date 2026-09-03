#!/usr/bin/env python3
"""Compare declared fields in sampled frames; this is not a full-run verdict.

The caller verifies original artifacts and supplies independently reviewed rules
and qualified observations. A readable value is an observation, not a confidence
score. This module neither reads pixels nor establishes a reader's reliability.

Offline usage: python3 scripts/bench/visual_compare.py --expected expected.json
    --observed observed.json --out new-result.json
Exit codes: 0 for matching samples, 1 for a discrepancy, 2 for inconclusive
evidence or an output error. Existing output files are never overwritten.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any


FIELDS = (
    "primary_band", "primary_frequency", "primary_direction", "count",
    "secondary", "main_volume", "mute_volume", "mode", "muted",
)
FRAME_IDENTITY = ("frame_id", "image_sha256", "source_frame_seq", "capture_ns")
QUALIFICATION = "Sampled record comparison only; no reader, source, or full-run qualification."


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _digest(value: Any) -> str | None:
    try:
        return hashlib.sha256(_canonical(value).encode()).hexdigest()
    except (TypeError, ValueError):
        return None


def _valid_hash(value: Any) -> bool:
    return (isinstance(value, str) and len(value) == 64
            and all(char in "0123456789abcdef" for char in value))


def _valid_source(source: Any) -> bool:
    return (isinstance(source, dict)
            and isinstance(source.get("run_id"), str) and bool(source["run_id"])
            and _valid_hash(source.get("video_sha256"))
            and _valid_hash(source.get("stimulus_sha256")))


def _frame_identity(frame: dict[str, Any]) -> dict[str, Any]:
    return {name: frame.get(name) for name in FRAME_IDENTITY}


def _valid_frame(frame: Any) -> bool:
    return (isinstance(frame, dict)
            and isinstance(frame.get("frame_id"), str) and bool(frame["frame_id"])
            and _valid_hash(frame.get("image_sha256"))
            and all(type(frame.get(name)) is int and frame[name] >= 0
                    for name in ("source_frame_seq", "capture_ns")))


def _value_key(field: str, value: Any) -> str:
    if field == "secondary":
        if not isinstance(value, list) or any(
            not isinstance(item, dict) or set(item) != {"band", "frequency", "direction"}
            or any(member is not None and not isinstance(member, str) for member in item.values())
            for item in value
        ):
            raise ValueError("secondary must contain complete alert objects")
        # Ordering is not part of this field's contract; multiplicity still is.
        return _canonical(sorted(_canonical(item) for item in value))
    if field in ("count", "main_volume", "mute_volume"):
        valid = value is None or type(value) is int
    elif field == "muted":
        valid = value is None or type(value) is bool
    else:
        valid = value is None or isinstance(value, str)
    if not valid:
        raise ValueError("field value has the wrong type")
    return _canonical(value)


def _field_result(field: str, expected: Any, observed: Any, problem: str) -> dict[str, Any]:
    result = {"field": field, "expected": expected, "observed": observed,
              "result": "UNRESOLVED", "reason": problem}
    if problem:
        return result
    if not isinstance(expected, dict):
        result["reason"] = "missing or malformed expectation"
    elif "unresolved" in expected:
        result["reason"] = "unresolved expectation: " + str(expected["unresolved"])
    elif not isinstance(expected.get("allowed"), list) or not expected["allowed"]:
        result["reason"] = "expectation requires a nonempty allowed list"
    elif not isinstance(observed, dict):
        result["reason"] = "missing or malformed observation"
    elif observed.get("state") != "readable":
        result["reason"] = "observation is not readable"
    elif "value" not in observed:
        result["reason"] = "readable observation has no value"
    else:
        try:
            allowed = [_value_key(field, value) for value in expected["allowed"]]
            matched = _value_key(field, observed["value"]) in allowed
        except (TypeError, ValueError):
            result["reason"] = "malformed field value"
        else:
            result.update(result="MATCH" if matched else "MISMATCH", reason="")
    return result


def compare(expected: Any, observed: Any) -> dict[str, Any]:
    """Return field denominators and traceable results for schema-v1 samples.

    Every expected frame requires the declared fields; exclusions need reasons.
    Missing observations or rules keep required checks unresolved. A supported
    mismatch establishes FAIL even if other checks remain unresolved; invalid
    source identity supports none.
    """
    expected_hash, observed_hash = _digest(expected), _digest(observed)
    expected = expected if isinstance(expected, dict) and expected_hash is not None else {}
    observed = observed if isinstance(observed, dict) and observed_hash is not None else {}
    errors: list[str] = []
    identity_problem = ""
    if (type(expected.get("schema_version")) is not int or expected["schema_version"] != 1
            or type(observed.get("schema_version")) is not int or observed["schema_version"] != 1
            or expected_hash is None or observed_hash is None):
        identity_problem = "missing or invalid input schema"
    elif (not _valid_source(expected.get("source"))
            or not _valid_source(observed.get("source"))
            or expected["source"] != observed["source"]):
        identity_problem = "missing, invalid, or different source identity"
    if identity_problem:
        errors.append(identity_problem)

    required, excluded = expected.get("required_fields"), expected.get("excluded_fields")
    if not (isinstance(required, list) and required
            and all(isinstance(field, str) for field in required)
            and len(set(required)) == len(required)
            and isinstance(excluded, dict)
            and all(isinstance(reason, str) and reason.strip() for reason in excluded.values())
            and not set(required).intersection(excluded)
            and set(required).union(excluded) == set(FIELDS)):
        errors.append("required and reasoned excluded fields must partition schema v1")
        identity_problem = identity_problem or "invalid declared field scope"
        required, excluded = list(FIELDS), {}

    expected_frames, observed_frames = expected.get("frames"), observed.get("frames")
    if not isinstance(expected_frames, list) or not expected_frames:
        errors.append("expected scope requires a nonempty frame list")
        expected_frames = []
    if not isinstance(observed_frames, list):
        errors.append("missing or invalid observed frame list")
        observed_frames = []
    expected_ids = Counter(frame["frame_id"] for frame in expected_frames if _valid_frame(frame))
    observed_ids = Counter(frame["frame_id"] for frame in observed_frames if _valid_frame(frame))
    by_id = {frame["frame_id"]: frame for frame in observed_frames if _valid_frame(frame)}
    if any(not _valid_frame(frame) for frame in observed_frames):
        errors.append("malformed observed frame identity")
    if set(observed_ids) - set(expected_ids):
        errors.append("observations include frames outside the declared scope")
    counts = dict(required=0, attempted=0, evaluated=0, matched=0, mismatched=0,
                  unresolved=0, excluded=len(expected_frames) * len(excluded))
    frames = []
    for raw_frame in expected_frames:
        frame = raw_frame if isinstance(raw_frame, dict) else {}
        frame_id = frame.get("frame_id") if _valid_frame(frame) else None
        actual = by_id.get(frame_id, {})
        problem = identity_problem
        if not _valid_frame(frame):
            problem = problem or "malformed expected frame identity"
        elif expected_ids[frame_id] != 1 or observed_ids[frame_id] > 1:
            problem = problem or "duplicate frame identity"
        elif not actual or _frame_identity(frame) != _frame_identity(actual):
            problem = problem or "missing or different observed frame identity"
        elif actual.get("inference_status") != "complete":
            problem = problem or "inference did not complete"
        expected_fields, actual_fields = frame.get("fields"), actual.get("fields")
        if not isinstance(expected_fields, dict) or not isinstance(actual_fields, dict):
            problem = problem or "missing or malformed fields"
        expected_fields = expected_fields if isinstance(expected_fields, dict) else {}
        actual_fields = actual_fields if isinstance(actual_fields, dict) else {}
        if (set(expected_fields) | set(actual_fields)) - set(FIELDS):
            problem = problem or "fields outside schema v1"
        anomalies = actual.get("anomalies")
        if not isinstance(anomalies, list) or anomalies:
            errors.append(f"{frame_id}: full-frame anomaly review is unresolved")
        checks = []
        for field in required:
            check = _field_result(field, expected_fields.get(field), actual_fields.get(field), problem)
            check["attempted"] = field in actual_fields
            counts["required"] += 1
            counts["attempted"] += int(check["attempted"])
            counts[{"MATCH": "matched", "MISMATCH": "mismatched",
                    "UNRESOLVED": "unresolved"}[check["result"]]] += 1
            checks.append(check)
        frames.append({**_frame_identity(frame), "observed_identity": _frame_identity(actual),
                       "anomalies": anomalies, "checks": checks})
    counts["evaluated"] = counts["matched"] + counts["mismatched"]
    verdict = ("FAIL" if counts["mismatched"] else "INCONCLUSIVE"
               if errors or counts["unresolved"] or not counts["required"] else "PASS")
    return {
        "schema_version": 1,
        "kind": "sampled_frame_comparison",
        "scope": {"meaning": "declared fields at retained sampled frames only",
                  "required_fields": required, "excluded_fields": excluded},
        "result": verdict,
        "source": expected.get("source"),
        "input_content_sha256": {"expected": expected_hash, "observed": observed_hash},
        "comparator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "counts": counts,
        "errors": errors,
        "frames": frames,
    }


def _publish_new(path: Path, result: dict[str, Any]) -> None:
    descriptor, temporary = tempfile.mkstemp(prefix=".visual-compare-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(result, handle, indent=2, allow_nan=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.link(temporary, path)  # Atomic publication; an existing destination is an error.
    finally:
        Path(temporary).unlink()


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON object key")
        result[key] = value
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=QUALIFICATION)
    for name in ("expected", "observed", "out"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    args = parser.parse_args(argv)
    inputs = {}
    errors = []
    for name in ("expected", "observed"):
        try:
            inputs[name] = json.loads(getattr(args, name).read_text(encoding="utf-8"),
                                      object_pairs_hook=_unique_object)
        except (OSError, UnicodeError, ValueError, RecursionError):
            inputs[name] = None
            errors.append(f"{name} input could not be read as JSON")
    result = compare(inputs["expected"], inputs["observed"])
    result["qualification"] = QUALIFICATION
    result["errors"].extend(errors)
    try:
        _publish_new(args.out, result)
    except (OSError, ValueError):
        print("Sampled record comparison: INCONCLUSIVE; could not create new output.", file=sys.stderr)
        print(QUALIFICATION, file=sys.stderr)
        return 2
    label, status = {"PASS": ("MATCH", 0), "FAIL": ("MISMATCH", 1),
                     "INCONCLUSIVE": ("INCONCLUSIVE", 2)}[result["result"]]
    print(f"Sampled record comparison: {label}. {QUALIFICATION}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
